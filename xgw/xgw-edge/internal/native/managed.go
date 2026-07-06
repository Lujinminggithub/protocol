package native

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

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"go.uber.org/zap"
)

type ManagedClient struct {
	cfg         config.ClientConfig
	logger      *zap.SugaredLogger
	client      *Client
	session     *Session
	candidate   *Session
	switchingTo string
	mu          sync.RWMutex

	inboundCh chan Datagram
}

func NewManagedClient(cfg config.ClientConfig, logger *zap.SugaredLogger) *ManagedClient {
	cfg.Normalize()
	return &ManagedClient{
		cfg:       cfg,
		logger:    logger,
		client:    NewClient(cfg, logger),
		inboundCh: make(chan Datagram, 4096),
	}
}

func (m *ManagedClient) Connect(ctx context.Context) (*Session, error) {
	session, err := m.client.Connect(ctx)
	if err != nil {
		return nil, err
	}
	m.mu.Lock()
	m.session = session
	m.mu.Unlock()
	m.attachDatagramPump(ctx, session)
	return session, nil
}

func (m *ManagedClient) attachDatagramPump(ctx context.Context, session *Session) {
	go func() {
		for {
			dg, err := session.ReceiveDatagramFrom(ctx)
			if err != nil {
				return
			}
			select {
			case m.inboundCh <- dg:
			case <-ctx.Done():
				return
			}
		}
	}()
}

func (m *ManagedClient) CurrentSession() *Session {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.session
}

func (m *ManagedClient) StartAutoReconnect(ctx context.Context) {
	go func() {
		interval := m.cfg.Control.RouteUpdateInterval.Std()
		if interval <= 0 {
			interval = 15 * time.Second
		}
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				session := m.CurrentSession()
				if session == nil {
					continue
				}
				update, err := session.FetchRouteUpdate(ctx)
				if err != nil {
					m.logger.Warnf("native route update failed: %v", err)
					continue
				}
				m.logger.Infof("native route update route=%s line=%s hint=%s reason=%s score=%.2f",
					update.SelectedRoute.Name, update.SelectedRoute.LineID, update.ReconnectHint, update.Reason, update.SelectedRoute.Score)
				_ = session.Keepalive(ctx, control.Keepalive{
					LatestRTTMillis: int64(routeRTTMillis(update.SelectedRoute)),
					LossPPM:         int64(routeLossPPM(update.SelectedRoute)),
					InFlightBytes:   uint64(routeRTTMillis(update.SelectedRoute)) * 1024,
					SendCreditBytes: uint64(update.SelectedRoute.Score * 1024.0),
					AckCreditFrames: 1,
				})
				if !m.shouldSwitch(update) {
					continue
				}
				if err := m.switchToRoute(ctx, update); err != nil {
					m.logger.Warnf("native route switch failed: %v", err)
				}
			}
		}
	}()
}

func (m *ManagedClient) shouldSwitch(update *control.RouteUpdate) bool {
	if update == nil || update.SelectedRoute.Address == "" {
		return false
	}
	m.mu.RLock()
	current := m.session
	currentURL := m.cfg.ServerURL
	candidate := m.candidate
	switchingTo := m.switchingTo
	m.mu.RUnlock()
	if current != nil && sameRoute(current.Resp.SelectedRoute, update.SelectedRoute) {
		return false
	}
	if update.SelectedRoute.Address == currentURL {
		return false
	}
	if switchingTo == update.SelectedRoute.Address {
		return false
	}
	if candidate != nil && candidate.Resp.SelectedRoute.Address == update.SelectedRoute.Address {
		return false
	}
	return candidate == nil
}

func (m *ManagedClient) switchToRoute(ctx context.Context, update *control.RouteUpdate) error {
	m.mu.Lock()
	if m.switchingTo != "" || update.SelectedRoute.Address == m.cfg.ServerURL {
		m.mu.Unlock()
		return nil
	}
	m.switchingTo = update.SelectedRoute.Address
	m.mu.Unlock()
	defer func() {
		m.mu.Lock()
		if m.switchingTo == update.SelectedRoute.Address {
			m.switchingTo = ""
		}
		m.mu.Unlock()
	}()

	nextCfg := m.cfg
	nextCfg.ServerURL = update.SelectedRoute.Address
	nextCfg.BootstrapNode = update.SelectedRoute.Name
	nextClient := NewClient(nextCfg, m.logger)
	nextSession, err := nextClient.Connect(ctx)
	if err != nil {
		return err
	}
	m.attachDatagramPump(ctx, nextSession)
	m.mu.Lock()
	old := m.session
	m.candidate = nextSession
	m.client = nextClient
	m.cfg = nextCfg
	m.session = nextSession
	m.candidate = nil
	m.switchingTo = ""
	m.mu.Unlock()
	if old != nil {
		go func() {
			time.Sleep(5 * time.Second)
			_ = old.Close()
		}()
	}
	m.logger.Infof("native route switched route=%s line=%s url=%s", update.SelectedRoute.Name, update.SelectedRoute.LineID, update.SelectedRoute.Address)
	return nil
}

func sameRoute(a control.RouteCandidate, b control.RouteCandidate) bool {
	if a.LineID != "" || b.LineID != "" {
		return a.LineID != "" && a.LineID == b.LineID
	}
	return a.Name == b.Name && a.Address == b.Address
}

func routeRTTMillis(route control.RouteCandidate) float64 {
	for _, reason := range route.Reasons {
		if reason == "probe-degraded" {
			return 1000
		}
	}
	if route.Score <= 0 {
		return 500
	}
	return 50
}

func routeLossPPM(route control.RouteCandidate) float64 {
	for _, reason := range route.Reasons {
		if reason == "probe-degraded" {
			return 1000000
		}
	}
	return 0
}

func (m *ManagedClient) StartLocalProxy(ctx context.Context, socksAddr string, httpAddr string) error {
	if socksAddr != "" {
		go m.serveSOCKS5(ctx, socksAddr)
	}
	if httpAddr != "" {
		go m.serveHTTPConnect(ctx, httpAddr)
	}
	return nil
}

func (m *ManagedClient) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	session := m.CurrentSession()
	if session == nil {
		return nil, context.Canceled
	}
	return session.DialTCP(ctx, target)
}

func (m *ManagedClient) SendDatagramTo(ctx context.Context, target string, payload []byte) error {
	session := m.CurrentSession()
	if session == nil {
		return context.Canceled
	}
	return session.SendDatagramTo(ctx, target, payload)
}

func (m *ManagedClient) ReceiveDatagramFrom(ctx context.Context) (Datagram, error) {
	select {
	case dg := <-m.inboundCh:
		return dg, nil
	case <-ctx.Done():
		return Datagram{}, ctx.Err()
	}
}

func (m *ManagedClient) LocalUDPProxy(ctx context.Context, listenAddr string, defaultTarget string) error {
	laddr, err := net.ResolveUDPAddr("udp", listenAddr)
	if err != nil {
		return err
	}
	conn, err := net.ListenUDP("udp", laddr)
	if err != nil {
		return err
	}
	defer conn.Close()

	var peerMu sync.RWMutex
	var peer *net.UDPAddr

	go func() {
		for {
			dg, err := m.ReceiveDatagramFrom(ctx)
			if err != nil {
				return
			}
			peerMu.RLock()
			dst := peer
			peerMu.RUnlock()
			if dst != nil {
				_, _ = conn.WriteToUDP(dg.Payload, dst)
			}
		}
	}()

	buf := make([]byte, 64<<10)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, addr, err := conn.ReadFromUDP(buf)
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
		target := defaultTarget
		if target == "" {
			target = addr.String()
		}
		peerMu.Lock()
		peer = addr
		peerMu.Unlock()
		if err := m.SendDatagramTo(ctx, target, append([]byte(nil), buf[:n]...)); err != nil {
			return err
		}
	}
}

func (m *ManagedClient) serveSOCKS5(ctx context.Context, addr string) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		m.logger.Errorf("native local socks5 listen failed: %v", err)
		return
	}
	m.logger.Infof("native local socks5 listen=%s", addr)
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
	m.logger.Infof("native local socks5 connect target=%s", target)
	upstream, err := m.DialTCP(ctx, target)
	if err != nil {
		m.logger.Warnf("native local socks5 connect target=%s failed: %v", target, err)
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
		for {
			dg, err := m.ReceiveDatagramFrom(ctx)
			if err != nil {
				return
			}
			clientAddrMu.RLock()
			dst := clientAddr
			clientAddrMu.RUnlock()
			if dst == nil {
				continue
			}
			packet := buildSOCKS5UDPPacket(dg.Target, dg.Payload)
			_, _ = udpConn.WriteToUDP(packet, dst)
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
		payload, target, err := parseSOCKS5UDPPacket(buf[:n])
		if err != nil {
			continue
		}
		if err := m.SendDatagramTo(ctx, target, payload); err != nil {
			return err
		}
	}
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
			m.logger.Infof("native local http connect target=%s", r.Host)
			upstream, err := m.DialTCP(r.Context(), r.Host)
			if err != nil {
				m.logger.Warnf("native local http connect target=%s failed: %v", r.Host, err)
				_, _ = clientConn.Write([]byte("HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\n\r\n" + err.Error()))
				_ = clientConn.Close()
				return
			}
			_, _ = clientConn.Write([]byte("HTTP/1.1 200 Connection Established\r\n\r\n"))
			proxyTwoWay(clientConn, upstream)
		}),
	}
	m.logger.Infof("native local http-connect listen=%s", addr)
	go func() {
		<-ctx.Done()
		_ = server.Shutdown(context.Background())
	}()
	_ = server.ListenAndServe()
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

func buildSOCKS5UDPPacket(target string, payload []byte) []byte {
	host, portText, err := net.SplitHostPort(target)
	if err != nil {
		packet := make([]byte, 0, 10+len(payload))
		packet = append(packet, 0x00, 0x00, 0x00, 0x01)
		packet = append(packet, 127, 0, 0, 1, 0x00, 0x00)
		return append(packet, payload...)
	}
	portAddr, _ := net.ResolveTCPAddr("tcp", net.JoinHostPort("127.0.0.1", portText))
	port := 0
	if portAddr != nil {
		port = portAddr.Port
	}
	packet := make([]byte, 0, 4+1+len(host)+2+len(payload))
	packet = append(packet, 0x00, 0x00, 0x00)
	if ip := net.ParseIP(host).To4(); ip != nil {
		packet = append(packet, 0x01)
		packet = append(packet, ip...)
	} else {
		if len(host) > 255 {
			host = host[:255]
		}
		packet = append(packet, 0x03, byte(len(host)))
		packet = append(packet, []byte(host)...)
	}
	packet = append(packet, byte(port>>8), byte(port))
	packet = append(packet, payload...)
	return packet
}

func proxyTwoWay(a net.Conn, b net.Conn) {
	var wg sync.WaitGroup
	copyHalf := func(dst net.Conn, src net.Conn) {
		defer wg.Done()
		_, _ = copyWithBridgePool(dst, src)
		_ = dst.SetDeadline(time.Now())
	}
	wg.Add(2)
	go copyHalf(a, b)
	go copyHalf(b, a)
	wg.Wait()
	_ = a.Close()
	_ = b.Close()
}
