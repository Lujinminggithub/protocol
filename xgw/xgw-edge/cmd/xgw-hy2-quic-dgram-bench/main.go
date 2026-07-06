package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"sort"
	"time"

	hyclient "github.com/apernet/hysteria/core/v2/client"
	quic "github.com/quic-go/quic-go"
)

type benchSummary struct {
	OK            bool    `json:"ok"`
	Mode          string  `json:"mode"`
	DurationSec   float64 `json:"duration_sec"`
	IntervalMs    float64 `json:"interval_ms"`
	PayloadBytes  int     `json:"payload_bytes"`
	Sent          int     `json:"sent"`
	Received      int     `json:"received"`
	SuccessRate   float64 `json:"success_rate"`
	P50MS         float64 `json:"p50_ms"`
	P95MS         float64 `json:"p95_ms"`
	P99MS         float64 `json:"p99_ms"`
	LastErr       string  `json:"last_err,omitempty"`
}

func main() {
	var (
		serverAddr string
		targetAddr string
		token      string
		sni        string
		alpn       string
		duration   time.Duration
		interval   time.Duration
		payloadLen int
		timeout    time.Duration
	)

	flag.StringVar(&serverAddr, "server", "", "HY2 server udp address")
	flag.StringVar(&targetAddr, "target", "", "backend QUIC echo target address")
	flag.StringVar(&token, "token", "edge-secret", "auth token")
	flag.StringVar(&sni, "sni", "www.example.com", "HY2 TLS server name")
	flag.StringVar(&alpn, "alpn", "xgw-quic-echo", "quic echo alpn")
	flag.DurationVar(&duration, "duration", 30*time.Second, "bench duration")
	flag.DurationVar(&interval, "interval", 200*time.Millisecond, "send interval")
	flag.IntVar(&payloadLen, "payload-bytes", 1200, "payload size")
	flag.DurationVar(&timeout, "timeout", 5*time.Second, "one datagram timeout")
	flag.Parse()

	if serverAddr == "" || targetAddr == "" {
		fmt.Fprintln(os.Stderr, "server and target are required")
		os.Exit(2)
	}

	summary := run(serverAddr, targetAddr, token, sni, alpn, duration, interval, payloadLen, timeout)
	enc, _ := json.Marshal(summary)
	fmt.Println(string(enc))
	if !summary.OK {
		os.Exit(1)
	}
}

func run(serverAddr, targetAddr, token, sni, alpn string, duration, interval time.Duration, payloadLen int, timeout time.Duration) benchSummary {
	summary := benchSummary{
		Mode:         "quic-datagram",
		DurationSec:  duration.Seconds(),
		IntervalMs:   float64(interval.Microseconds()) / 1000,
		PayloadBytes: payloadLen,
	}

	raddr, err := net.ResolveUDPAddr("udp", serverAddr)
	if err != nil {
		summary.LastErr = err.Error()
		return summary
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
		summary.LastErr = err.Error()
		return summary
	}
	defer cli.Close()

	pc, err := newHY2PacketConn(cli)
	if err != nil {
		summary.LastErr = err.Error()
		return summary
	}
	defer pc.Close()

	targetUDP, err := net.ResolveUDPAddr("udp", targetAddr)
	if err != nil {
		summary.LastErr = err.Error()
		return summary
	}

	ctx, cancel := context.WithTimeout(context.Background(), duration+10*time.Second)
	defer cancel()
	conn, err := quic.Dial(ctx, pc, targetUDP, &tls.Config{
		ServerName:         "xgw-quic-echo",
		InsecureSkipVerify: true,
		NextProtos:         []string{alpn},
		MinVersion:         tls.VersionTLS13,
	}, &quic.Config{
		EnableDatagrams:      true,
		HandshakeIdleTimeout: 8 * time.Second,
		MaxIdleTimeout:       duration + 10*time.Second,
		KeepAlivePeriod:      5 * time.Second,
	})
	if err != nil {
		summary.LastErr = err.Error()
		return summary
	}
	defer conn.CloseWithError(0, "done")

	payload := make([]byte, payloadLen)
	for i := range payload {
		payload[i] = byte('a' + (i % 26))
	}

	var lats []float64
	deadline := time.Now().Add(duration)
	for time.Now().Before(deadline) {
		summary.Sent++
		start := time.Now()
		if err := conn.SendDatagram(payload); err != nil {
			summary.LastErr = err.Error()
			time.Sleep(interval)
			continue
		}
		recvCtx, recvCancel := context.WithTimeout(context.Background(), timeout)
		resp, err := conn.ReceiveDatagram(recvCtx)
		recvCancel()
		if err != nil {
			summary.LastErr = err.Error()
			time.Sleep(interval)
			continue
		}
		if len(resp) == len(payload) {
			summary.Received++
			lats = append(lats, float64(time.Since(start).Microseconds())/1000)
		}
		time.Sleep(interval)
	}

	if summary.Sent > 0 {
		summary.SuccessRate = float64(summary.Received) / float64(summary.Sent)
	}
	sort.Float64s(lats)
	summary.P50MS = percentile(lats, 50)
	summary.P95MS = percentile(lats, 95)
	summary.P99MS = percentile(lats, 99)
	summary.OK = summary.Received > 0 && summary.SuccessRate >= 0.95
	return summary
}

func percentile(values []float64, p float64) float64 {
	if len(values) == 0 {
		return 0
	}
	idx := int((p / 100) * float64(len(values)-1))
	if idx < 0 {
		idx = 0
	}
	if idx >= len(values) {
		idx = len(values) - 1
	}
	return values[idx]
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

func (c *hy2PacketConn) Close() error                     { return c.udp.Close() }
func (c *hy2PacketConn) LocalAddr() net.Addr              { return &net.UDPAddr{IP: net.IPv4zero, Port: 0} }
func (c *hy2PacketConn) SetDeadline(time.Time) error      { return nil }
func (c *hy2PacketConn) SetReadDeadline(time.Time) error  { return nil }
func (c *hy2PacketConn) SetWriteDeadline(time.Time) error { return nil }
