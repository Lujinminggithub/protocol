package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"sync"
	"sync/atomic"
	"time"

	hyclient "github.com/apernet/hysteria/core/v2/client"
	quic "github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
)

type probeSummary struct {
	OK         bool   `json:"ok"`
	URL        string `json:"url"`
	IP         string `json:"ip,omitempty"`
	Status     string `json:"status,omitempty"`
	ResolveMS  int64  `json:"resolve_ms"`
	DialMS     int64  `json:"dial_ms"`
	TTFBMS     int64  `json:"ttfb_ms"`
	ALPN       string `json:"alpn,omitempty"`
	ErrStage   string `json:"err_stage,omitempty"`
	ErrMessage string `json:"err_message,omitempty"`
}

func main() {
	var (
		serverAddr string
		targetURL  string
		token      string
		sni        string
		timeout    time.Duration
		method     string
	)

	flag.StringVar(&serverAddr, "server", "", "HY2 server udp address")
	flag.StringVar(&targetURL, "url", "", "https URL to probe over HTTP/3")
	flag.StringVar(&token, "token", "edge-secret", "auth token")
	flag.StringVar(&sni, "sni", "www.example.com", "HY2 TLS server name")
	flag.DurationVar(&timeout, "timeout", 15*time.Second, "probe timeout")
	flag.StringVar(&method, "method", http.MethodHead, "HTTP method")
	flag.Parse()

	if serverAddr == "" || targetURL == "" {
		fmt.Fprintln(os.Stderr, "server and url are required")
		os.Exit(2)
	}

	done := make(chan probeSummary, 1)
	go func() {
		done <- run(serverAddr, targetURL, token, sni, timeout, method)
	}()
	select {
	case summary := <-done:
		enc, _ := json.Marshal(summary)
		fmt.Println(string(enc))
		if !summary.OK {
			os.Exit(1)
		}
	case <-time.After(timeout + 5*time.Second):
		summary := probeSummary{
			OK:         false,
			URL:        targetURL,
			ErrStage:   "process_timeout",
			ErrMessage: "probe exceeded hard timeout",
		}
		enc, _ := json.Marshal(summary)
		fmt.Println(string(enc))
		os.Exit(1)
	}
}

func run(serverAddr, targetURL, token, sni string, timeout time.Duration, method string) probeSummary {
	summary := probeSummary{URL: targetURL}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	fmt.Fprintf(os.Stderr, "stage=request_build url=%s timeout=%s\n", targetURL, timeout)

	u, err := http.NewRequestWithContext(ctx, method, targetURL, nil)
	if err != nil {
		summary.ErrStage = "request"
		summary.ErrMessage = err.Error()
		return summary
	}
	host := u.URL.Hostname()
	port := u.URL.Port()
	if port == "" {
		port = "443"
	}

	resolveStart := time.Now()
	ips, err := net.DefaultResolver.LookupNetIP(ctx, "ip4", host)
	summary.ResolveMS = time.Since(resolveStart).Milliseconds()
	fmt.Fprintf(os.Stderr, "stage=resolve_done url=%s host=%s resolve_ms=%d ips=%d err=%v\n", targetURL, host, summary.ResolveMS, len(ips), err)
	if err != nil || len(ips) == 0 {
		summary.ErrStage = "resolve"
		if err != nil {
			summary.ErrMessage = err.Error()
		} else {
			summary.ErrMessage = "no ipv4 address"
		}
		return summary
	}

	raddr, err := net.ResolveUDPAddr("udp", serverAddr)
	if err != nil {
		summary.ErrStage = "server_resolve"
		summary.ErrMessage = err.Error()
		return summary
	}
	fmt.Fprintf(os.Stderr, "stage=hy2_connect_begin server=%s url=%s\n", serverAddr, targetURL)
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
		summary.ErrStage = "hy2_connect"
		summary.ErrMessage = err.Error()
		return summary
	}
	defer cli.Close()
	fmt.Fprintf(os.Stderr, "stage=hy2_connect_ok server=%s url=%s\n", serverAddr, targetURL)

	var lastErr error
	for _, ip := range ips {
		targetAddr := net.JoinHostPort(ip.String(), port)
		summary.IP = ip.String()
		fmt.Fprintf(os.Stderr, "stage=target_ip url=%s ip=%s\n", targetURL, summary.IP)
		pc, err := newHY2PacketConn(cli)
		if err != nil {
			summary.ErrStage = "udp_wrap"
			summary.ErrMessage = err.Error()
			return summary
		}

		var dialMS int64
		tr := &http3.Transport{
			TLSClientConfig: &tls.Config{
				ServerName:         host,
				InsecureSkipVerify: true,
				NextProtos:         []string{"h3", "h3-29"},
				MinVersion:         tls.VersionTLS13,
			},
			QUICConfig: &quic.Config{
				EnableDatagrams:      true,
				HandshakeIdleTimeout: 8 * time.Second,
				MaxIdleTimeout:       timeout,
				KeepAlivePeriod:      5 * time.Second,
			},
			EnableDatagrams: true,
			Dial: func(ctx context.Context, _ string, tlsCfg *tls.Config, cfg *quic.Config) (*quic.Conn, error) {
				dialStart := time.Now()
				udpAddr, derr := net.ResolveUDPAddr("udp", targetAddr)
				if derr != nil {
					return nil, derr
				}
				fmt.Fprintf(os.Stderr, "stage=quic_dial_begin url=%s target=%s\n", targetURL, targetAddr)
				conn, derr := quic.Dial(ctx, pc, udpAddr, tlsCfg, cfg)
				dialMS = time.Since(dialStart).Milliseconds()
				fmt.Fprintf(os.Stderr, "stage=quic_dial_done url=%s target=%s dial_ms=%d err=%v\n", targetURL, targetAddr, dialMS, derr)
				return conn, derr
			},
		}

		client := &http.Client{
			Transport: tr,
			Timeout:   timeout,
		}
		start := time.Now()
		fmt.Fprintf(os.Stderr, "stage=h3_request_begin url=%s ip=%s\n", targetURL, summary.IP)
		resp, err := client.Do(u)
		if err == nil {
			summary.OK = true
			summary.DialMS = dialMS
			summary.TTFBMS = time.Since(start).Milliseconds()
			summary.Status = resp.Status
			if resp.ProtoMajor == 3 {
				summary.ALPN = "h3"
			}
			_ = resp.Body.Close()
			_ = tr.Close()
			_ = pc.Close()
			fmt.Fprintf(os.Stderr, "stage=h3_request_ok url=%s ip=%s status=%s ttfb_ms=%d\n", targetURL, summary.IP, summary.Status, summary.TTFBMS)
			return summary
		}
		lastErr = err
		fmt.Fprintf(os.Stderr, "stage=h3_request_fail url=%s ip=%s err=%v\n", targetURL, summary.IP, err)
		_ = tr.Close()
		_ = pc.Close()
	}

	summary.ErrStage = "roundtrip"
	if lastErr != nil {
		summary.ErrMessage = lastErr.Error()
	}
	return summary
}

type hy2PacketConn struct {
	udp          hyclient.HyUDPConn
	readCh       chan packetMsg
	done         chan struct{}
	closeOnce    sync.Once
	readDeadline atomic.Int64
}

func newHY2PacketConn(cli hyclient.Client) (*hy2PacketConn, error) {
	udp, err := cli.UDP()
	if err != nil {
		return nil, err
	}
	pc := &hy2PacketConn{
		udp:    udp,
		readCh: make(chan packetMsg, 256),
		done:   make(chan struct{}),
	}
	go pc.readLoop()
	return pc, nil
}

func (c *hy2PacketConn) ReadFrom(p []byte) (int, net.Addr, error) {
	for {
		timer := deadlineTimer(c.readDeadline.Load())
		select {
		case msg, ok := <-c.readCh:
			if timer != nil {
				timer.Stop()
			}
			if !ok {
				return 0, nil, io.EOF
			}
			if msg.err != nil {
				return 0, nil, msg.err
			}
			n := copy(p, msg.data)
			return n, msg.addr, nil
		case <-c.done:
			if timer != nil {
				timer.Stop()
			}
			return 0, nil, io.EOF
		case <-timerChan(timer):
			return 0, nil, os.ErrDeadlineExceeded
		}
	}
}

func (c *hy2PacketConn) WriteTo(p []byte, addr net.Addr) (int, error) {
	if err := c.udp.Send(p, addr.String()); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *hy2PacketConn) Close() error {
	var err error
	c.closeOnce.Do(func() {
		close(c.done)
		err = c.udp.Close()
	})
	return err
}
func (c *hy2PacketConn) LocalAddr() net.Addr              { return &net.UDPAddr{IP: net.IPv4zero, Port: 0} }
func (c *hy2PacketConn) SetDeadline(t time.Time) error {
	return c.SetReadDeadline(t)
}
func (c *hy2PacketConn) SetReadDeadline(t time.Time) error {
	if t.IsZero() {
		c.readDeadline.Store(0)
		return nil
	}
	c.readDeadline.Store(t.UnixNano())
	return nil
}
func (c *hy2PacketConn) SetWriteDeadline(time.Time) error { return nil }

type packetMsg struct {
	data []byte
	addr net.Addr
	err  error
}

func (c *hy2PacketConn) readLoop() {
	defer close(c.readCh)
	for {
		data, addrText, err := c.udp.Receive()
		if err != nil {
			select {
			case c.readCh <- packetMsg{err: err}:
			case <-c.done:
			}
			return
		}
		addr, err := net.ResolveUDPAddr("udp", addrText)
		if err != nil {
			select {
			case c.readCh <- packetMsg{err: err}:
			case <-c.done:
			}
			return
		}
		buf := append([]byte(nil), data...)
		select {
		case c.readCh <- packetMsg{data: buf, addr: addr}:
		case <-c.done:
			return
		}
	}
}

func deadlineTimer(deadlineNano int64) *time.Timer {
	if deadlineNano == 0 {
		return nil
	}
	d := time.Until(time.Unix(0, deadlineNano))
	if d <= 0 {
		return time.NewTimer(time.Nanosecond)
	}
	return time.NewTimer(d)
}

func timerChan(t *time.Timer) <-chan time.Time {
	if t == nil {
		return nil
	}
	return t.C
}
