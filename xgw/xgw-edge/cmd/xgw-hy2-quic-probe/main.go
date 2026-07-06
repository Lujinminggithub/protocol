package main

import (
	"context"
	"crypto/tls"
	"flag"
	"fmt"
	"net"
	"os"
	"time"

	hyclient "github.com/apernet/hysteria/core/v2/client"
	quic "github.com/quic-go/quic-go"
)

func main() {
	var (
		serverAddr string
		targetAddr string
		token      string
		sni        string
		alpn       string
		timeout    time.Duration
		payload    string
		mode       string
	)

	flag.StringVar(&serverAddr, "server", "", "HY2 server udp address")
	flag.StringVar(&targetAddr, "target", "", "backend QUIC echo target address")
	flag.StringVar(&token, "token", "edge-secret", "auth token")
	flag.StringVar(&sni, "sni", "www.example.com", "tls server name")
	flag.StringVar(&alpn, "alpn", "xgw-quic-echo", "quic echo alpn")
	flag.DurationVar(&timeout, "timeout", 15*time.Second, "probe timeout")
	flag.StringVar(&payload, "payload", "quic-ping", "probe payload")
	flag.StringVar(&mode, "mode", "datagram", "probe mode: datagram or stream")
	flag.Parse()

	if serverAddr == "" || targetAddr == "" {
		fmt.Fprintln(os.Stderr, "server and target are required")
		os.Exit(2)
	}

	raddr, err := net.ResolveUDPAddr("udp", serverAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve server: %v\n", err)
		os.Exit(1)
	}

	cli, _, err := hyclient.NewClient(&hyclient.Config{
		ServerAddr: raddr,
		Auth:       token,
		TLSConfig: hyclient.TLSConfig{
			ServerName:         sni,
			InsecureSkipVerify: true,
		},
		QUICConfig: hyclient.QUICConfig{
			MaxIdleTimeout:  30 * time.Second,
			KeepAlivePeriod: 10 * time.Second,
		},
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "connect hy2: %v\n", err)
		os.Exit(1)
	}
	defer cli.Close()

	pc, err := newHY2PacketConn(cli)
	if err != nil {
		fmt.Fprintf(os.Stderr, "wrap hy2 udp: %v\n", err)
		os.Exit(1)
	}
	defer pc.Close()

	targetUDP, err := net.ResolveUDPAddr("udp", targetAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve target: %v\n", err)
		os.Exit(1)
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	conn, err := quic.Dial(ctx, pc, targetUDP, &tls.Config{
		ServerName:         "xgw-quic-echo",
		InsecureSkipVerify: true,
		NextProtos:         []string{alpn},
		MinVersion:         tls.VersionTLS13,
	}, &quic.Config{
		EnableDatagrams:      true,
		HandshakeIdleTimeout: 8 * time.Second,
		MaxIdleTimeout:       timeout,
		KeepAlivePeriod:      5 * time.Second,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "quic dial: %v\n", err)
		os.Exit(1)
	}
	defer conn.CloseWithError(0, "done")

	start := time.Now()
	switch mode {
	case "datagram":
		if err := conn.SendDatagram([]byte(payload)); err != nil {
			fmt.Fprintf(os.Stderr, "datagram send: %v\n", err)
			os.Exit(1)
		}
		data, err := conn.ReceiveDatagram(ctx)
		if err != nil {
			fmt.Fprintf(os.Stderr, "datagram recv: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("mode=datagram\n")
		fmt.Printf("elapsed_ms=%d\n", time.Since(start).Milliseconds())
		fmt.Printf("data=%s\n", string(data))
	case "stream":
		stream, err := conn.OpenStreamSync(ctx)
		if err != nil {
			fmt.Fprintf(os.Stderr, "stream open: %v\n", err)
			os.Exit(1)
		}
		defer stream.Close()
		if _, err := stream.Write([]byte(payload)); err != nil {
			fmt.Fprintf(os.Stderr, "stream write: %v\n", err)
			os.Exit(1)
		}
		if err := stream.Close(); err != nil {
			fmt.Fprintf(os.Stderr, "stream close: %v\n", err)
			os.Exit(1)
		}
		buf := make([]byte, len(payload)+64)
		n, err := stream.Read(buf)
		if err != nil && err.Error() != "EOF" {
			fmt.Fprintf(os.Stderr, "stream read: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("mode=stream\n")
		fmt.Printf("elapsed_ms=%d\n", time.Since(start).Milliseconds())
		fmt.Printf("data=%s\n", string(buf[:n]))
	default:
		fmt.Fprintf(os.Stderr, "unknown mode: %s\n", mode)
		os.Exit(2)
	}
}

type hy2PacketConn struct {
	udp hyclient.HyUDPConn
}

func newHY2PacketConn(cli hyclient.Client) (*hy2PacketConn, error) {
	udp, err := cli.UDP()
	if err != nil {
		return nil, err
	}
	return &hy2PacketConn{udp: udp}, nil
}

func (c *hy2PacketConn) ReadFrom(p []byte) (int, net.Addr, error) {
	data, addr, err := c.udp.Receive()
	if err != nil {
		return 0, nil, err
	}
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return 0, nil, err
	}
	n := copy(p, data)
	return n, udpAddr, nil
}

func (c *hy2PacketConn) WriteTo(p []byte, addr net.Addr) (int, error) {
	if err := c.udp.Send(p, addr.String()); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *hy2PacketConn) Close() error                       { return c.udp.Close() }
func (c *hy2PacketConn) LocalAddr() net.Addr                { return &net.UDPAddr{IP: net.IPv4zero, Port: 0} }
func (c *hy2PacketConn) SetDeadline(time.Time) error        { return nil }
func (c *hy2PacketConn) SetReadDeadline(time.Time) error    { return nil }
func (c *hy2PacketConn) SetWriteDeadline(time.Time) error   { return nil }
