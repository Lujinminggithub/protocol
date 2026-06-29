package edge

import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"time"
)

func (m *ManagedClient) StartLocalProxy(ctx context.Context, socksAddr string, httpAddr string) error {
	if socksAddr != "" {
		go m.serveSOCKS5(ctx, socksAddr)
	}
	if httpAddr != "" {
		go m.serveHTTPConnect(ctx, httpAddr)
	}
	return nil
}

func (m *ManagedClient) serveSOCKS5(ctx context.Context, addr string) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		m.logger.Errorf("local socks5 listen failed: %v", err)
		return
	}
	m.logger.Infof("local socks5 listen=%s", addr)
	go func() {
		<-ctx.Done()
		_ = ln.Close()
	}()
	for {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		go m.handleSOCKS5Conn(ctx, conn)
	}
}

func (m *ManagedClient) handleSOCKS5Conn(ctx context.Context, conn net.Conn) {
	defer conn.Close()
	m.logger.Infof("local socks5 accepted remote=%s", conn.RemoteAddr())
	reader := bufio.NewReader(conn)
	header := make([]byte, 2)
	if _, err := io.ReadFull(reader, header); err != nil {
		return
	}
	if header[0] != 0x05 {
		return
	}
	methods := make([]byte, int(header[1]))
	if _, err := io.ReadFull(reader, methods); err != nil {
		return
	}
	_, _ = conn.Write([]byte{0x05, 0x00})

	req := make([]byte, 4)
	if _, err := io.ReadFull(reader, req); err != nil {
		return
	}
	if req[1] == 0x03 {
		if err := m.handleSOCKS5UDPAssociate(ctx, conn, reader, req[3]); err != nil {
			_, _ = conn.Write([]byte{0x05, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		}
		return
	}
	if req[1] != 0x01 {
		_, _ = conn.Write([]byte{0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	target, err := readSOCKS5Addr(reader, req[3])
	if err != nil {
		_, _ = conn.Write([]byte{0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	m.logger.Infof("local socks5 connect target=%s", target)
	upstream, err := m.DialTCP(ctx, target)
	if err != nil {
		m.logger.Warnf("local socks5 connect target=%s failed: %v", target, err)
		_, _ = conn.Write([]byte{0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
		return
	}
	defer upstream.Close()
	_, _ = conn.Write([]byte{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
	proxyTwoWay(conn, upstream)
}

func (m *ManagedClient) handleSOCKS5UDPAssociate(ctx context.Context, tcpConn net.Conn, reader *bufio.Reader, atyp byte) error {
	_, err := readSOCKS5Addr(reader, atyp)
	if err != nil {
		return err
	}
	udpConn, err := net.ListenUDP("udp", nil)
	if err != nil {
		return err
	}
	defer udpConn.Close()

	localAddr := udpConn.LocalAddr().(*net.UDPAddr)
	reply := []byte{0x05, 0x00, 0x00, 0x01}
	ip4 := localAddr.IP.To4()
	if ip4 == nil {
		ip4 = net.IPv4(127, 0, 0, 1)
	}
	reply = append(reply, ip4...)
	reply = append(reply, byte(localAddr.Port>>8), byte(localAddr.Port))
	if _, err := tcpConn.Write(reply); err != nil {
		return err
	}

	var clientAddrMu sync.RWMutex
	var clientAddr *net.UDPAddr

	go func() {
		buf := make([]byte, 64<<10)
		for {
			msg, err := m.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			clientAddrMu.RLock()
			dst := clientAddr
			clientAddrMu.RUnlock()
			if dst == nil {
				continue
			}
			packet := make([]byte, 0, 3+1+4+2+len(msg))
			packet = append(packet, 0x00, 0x00, 0x00, 0x01)
			packet = append(packet, 127, 0, 0, 1, 0x00, 0x00)
			packet = append(packet, msg...)
			_, _ = udpConn.WriteToUDP(packet, dst)
			_ = buf
		}
	}()

	buf := make([]byte, 64<<10)
	for {
		_ = udpConn.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, addr, err := udpConn.ReadFromUDP(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				select {
				case <-ctx.Done():
					return ctx.Err()
				default:
					continue
				}
			}
			return err
		}
		clientAddrMu.Lock()
		clientAddr = addr
		clientAddrMu.Unlock()
		payload, _, err := parseSOCKS5UDPPacket(buf[:n])
		if err != nil {
			continue
		}
		if err := m.SendDatagram(ctx, payload); err != nil {
			return err
		}
	}
}

func readSOCKS5Addr(r *bufio.Reader, atyp byte) (string, error) {
	switch atyp {
	case 0x01:
		addr := make([]byte, 6)
		if _, err := io.ReadFull(r, addr); err != nil {
			return "", err
		}
		host := net.IP(addr[:4]).String()
		port := binary.BigEndian.Uint16(addr[4:])
		return fmt.Sprintf("%s:%d", host, port), nil
	case 0x03:
		l, err := r.ReadByte()
		if err != nil {
			return "", err
		}
		addr := make([]byte, int(l)+2)
		if _, err := io.ReadFull(r, addr); err != nil {
			return "", err
		}
		host := string(addr[:len(addr)-2])
		port := binary.BigEndian.Uint16(addr[len(addr)-2:])
		return fmt.Sprintf("%s:%d", host, port), nil
	default:
		return "", errors.New("unsupported atyp")
	}
}

func parseSOCKS5UDPPacket(pkt []byte) ([]byte, string, error) {
	if len(pkt) < 10 {
		return nil, "", errors.New("udp packet too short")
	}
	if pkt[2] != 0x00 {
		return nil, "", errors.New("fragmented udp not supported")
	}
	atyp := pkt[3]
	offset := 4
	var host string
	switch atyp {
	case 0x01:
		if len(pkt) < offset+4+2 {
			return nil, "", errors.New("ipv4 udp packet too short")
		}
		host = net.IP(pkt[offset : offset+4]).String()
		offset += 4
	case 0x03:
		if len(pkt) < offset+1 {
			return nil, "", errors.New("domain udp packet too short")
		}
		l := int(pkt[offset])
		offset++
		if len(pkt) < offset+l+2 {
			return nil, "", errors.New("domain udp packet too short")
		}
		host = string(pkt[offset : offset+l])
		offset += l
	default:
		return nil, "", errors.New("unsupported udp atyp")
	}
	port := binary.BigEndian.Uint16(pkt[offset : offset+2])
	offset += 2
	return append([]byte(nil), pkt[offset:]...), fmt.Sprintf("%s:%d", host, port), nil
}

func (m *ManagedClient) serveHTTPConnect(ctx context.Context, addr string) {
	server := &http.Server{
		Addr: addr,
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodConnect {
				w.WriteHeader(http.StatusMethodNotAllowed)
				return
			}
			hijacker, ok := w.(http.Hijacker)
			if !ok {
				http.Error(w, "hijack unsupported", http.StatusInternalServerError)
				return
			}
			clientConn, _, err := hijacker.Hijack()
			if err != nil {
				return
			}
			m.logger.Infof("local http connect target=%s", r.Host)
			upstream, err := m.DialTCP(r.Context(), r.Host)
			if err != nil {
				m.logger.Warnf("local http connect target=%s failed: %v", r.Host, err)
				_, _ = clientConn.Write([]byte("HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\n\r\n" + err.Error()))
				_ = clientConn.Close()
				return
			}
			_, _ = clientConn.Write([]byte("HTTP/1.1 200 Connection Established\r\n\r\n"))
			proxyTwoWay(clientConn, upstream)
		}),
	}
	m.logger.Infof("local http-connect listen=%s", addr)
	go func() {
		<-ctx.Done()
		_ = server.Shutdown(context.Background())
	}()
	_ = server.ListenAndServe()
}
