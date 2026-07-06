package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"time"

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
		targetURL string
		timeout   time.Duration
		method    string
	)
	flag.StringVar(&targetURL, "url", "", "https URL to probe over direct HTTP/3")
	flag.DurationVar(&timeout, "timeout", 20*time.Second, "probe timeout")
	flag.StringVar(&method, "method", http.MethodHead, "HTTP method")
	flag.Parse()

	if targetURL == "" {
		fmt.Fprintln(os.Stderr, "url is required")
		os.Exit(2)
	}

	done := make(chan probeSummary, 1)
	go func() {
		done <- run(targetURL, timeout, method)
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

func run(targetURL string, timeout time.Duration, method string) probeSummary {
	summary := probeSummary{URL: targetURL}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	fmt.Fprintf(os.Stderr, "stage=request_build url=%s timeout=%s\n", targetURL, timeout)

	req, err := http.NewRequestWithContext(ctx, method, targetURL, nil)
	if err != nil {
		summary.ErrStage = "request"
		summary.ErrMessage = err.Error()
		return summary
	}
	host := req.URL.Hostname()
	port := req.URL.Port()
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

	var lastErr error
	for _, ip := range ips {
		summary.IP = ip.String()
		targetAddr := net.JoinHostPort(summary.IP, port)
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
				fmt.Fprintf(os.Stderr, "stage=quic_dial_begin url=%s target=%s\n", targetURL, targetAddr)
				conn, derr := quic.DialAddr(ctx, targetAddr, tlsCfg, cfg)
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
		resp, err := client.Do(req)
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
			fmt.Fprintf(os.Stderr, "stage=h3_request_ok url=%s ip=%s status=%s ttfb_ms=%d\n", targetURL, summary.IP, summary.Status, summary.TTFBMS)
			return summary
		}
		lastErr = err
		fmt.Fprintf(os.Stderr, "stage=h3_request_fail url=%s ip=%s err=%v\n", targetURL, summary.IP, err)
		_ = tr.Close()
	}

	summary.ErrStage = "roundtrip"
	if lastErr != nil {
		summary.ErrMessage = lastErr.Error()
	}
	return summary
}
