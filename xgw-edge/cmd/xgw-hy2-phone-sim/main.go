package main

import (
	"context"
	"crypto/tls"
	"flag"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	hyclient "github.com/apernet/hysteria/core/v2/client"
	"github.com/local/xgw-edge/internal/config"
	"go.uber.org/zap"
)

func main() {
	var (
		configPath string
		targetURL  string
		runProbe   bool
		oneShot    bool
	)
	flag.StringVar(&configPath, "config", "", "client config file")
	flag.StringVar(&targetURL, "target", "https://ip.sb/", "probe URL through local HTTP CONNECT")
	flag.BoolVar(&runProbe, "probe", true, "run one minimal business request after connect")
	flag.BoolVar(&oneShot, "oneshot", false, "exit after the first probe attempt finishes")
	flag.Parse()

	cfg, err := config.LoadClient(configPath)
	if err != nil {
		panic(err)
	}
	logger, err := zap.NewProduction()
	if err != nil {
		panic(err)
	}
	defer logger.Sync()
	log := logger.Sugar()

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	client, hs, err := connectHY2(cfg)
	if err != nil {
		log.Errorf("hy2-phone-sim connect failed: %v", err)
		os.Exit(1)
	}
	defer client.Close()
	log.Infof("hy2-phone-sim connected server=%s udp_enabled=%t tx=%d", hs.ServerAddr, hs.UDPEnabled, hs.Tx)

	if runProbe {
		if err := runHTTPSProbe(client, targetURL, log); err != nil {
			log.Warnf("hy2-phone-sim probe failed target=%s err=%v", targetURL, err)
			os.Exit(2)
		}
		if oneShot {
			log.Infof("hy2-phone-sim oneshot done target=%s", targetURL)
			return
		}
	}

	<-ctx.Done()
}

func connectHY2(cfg config.ClientConfig) (hyclient.Client, *hyclient.HandshakeInfo, error) {
	serverURL, err := url.Parse(cfg.ServerURL)
	if err != nil {
		return nil, nil, err
	}
	host := serverURL.Host
	if _, _, splitErr := net.SplitHostPort(host); splitErr != nil {
		host = net.JoinHostPort(host, "443")
	}
	udpAddr, err := net.ResolveUDPAddr("udp", host)
	if err != nil {
		return nil, nil, err
	}
	hcfg := &hyclient.Config{
		ServerAddr: udpAddr,
		Auth:       cfg.Control.Token,
		TLSConfig: hyclient.TLSConfig{
			ServerName:         cfg.ServerName,
			InsecureSkipVerify: cfg.TLS.InsecureSkipVerify,
		},
		QUICConfig: hyclient.QUICConfig{
			MaxIdleTimeout:  cfg.Control.IdleTimeout.Std(),
			KeepAlivePeriod: cfg.Control.Keepalive.Std(),
		},
		CongestionConfig: hyclient.CongestionConfig{
			Type: "bbr",
		},
		BandwidthConfig: hyclient.BandwidthConfig{
			MaxTx: cfg.Control.AdvertisedTxMbps * 125000,
			MaxRx: cfg.Control.AdvertisedRxMbps * 125000,
		},
		FastOpen: false,
	}
	return hyclient.NewClient(hcfg)
}

func runHTTPSProbe(client hyclient.Client, targetURL string, log *zap.SugaredLogger) error {
	parsed, err := url.Parse(targetURL)
	if err != nil {
		return err
	}
	if parsed.Scheme == "" {
		parsed.Scheme = "https"
	}
	host := parsed.Hostname()
	portText := parsed.Port()
	port := 443
	if portText != "" {
		if n, convErr := strconv.Atoi(portText); convErr == nil {
			port = n
		}
	}
	targetAddr := net.JoinHostPort(host, strconv.Itoa(port))
	start := time.Now()
	conn, err := client.TCP(targetAddr)
	if err != nil {
		return err
	}
	defer conn.Close()
	log.Infof("hy2-phone-sim tcp.open target=%s", targetAddr)

	tlsConn := tls.Client(conn, &tls.Config{
		ServerName:         host,
		InsecureSkipVerify: true,
		NextProtos:         []string{"h2", "http/1.1"},
	})
	if err := tlsConn.SetDeadline(time.Now().Add(12 * time.Second)); err != nil {
		return err
	}
	if err := tlsConn.Handshake(); err != nil {
		return err
	}
	log.Infof("hy2-phone-sim tls.handshake.ok target=%s alpn=%s", targetAddr, tlsConn.ConnectionState().NegotiatedProtocol)
	reqPath := parsed.RequestURI()
	if reqPath == "" {
		reqPath = "/"
	}
	req, err := http.NewRequest(http.MethodGet, targetURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "xgw-hy2-phone-sim")
	if err := req.Write(tlsConn); err != nil {
		return err
	}
	resp, err := io.ReadAll(io.LimitReader(tlsConn, 2048))
	if err != nil && !isEOFLike(err) {
		return err
	}
	statusLine := firstLine(resp)
	log.Infof("hy2-phone-sim probe.ok target=%s status_line=%s elapsed_ms=%d bytes=%d", targetURL, statusLine, time.Since(start).Milliseconds(), len(resp))
	return nil
}

func firstLine(data []byte) string {
	text := string(data)
	if idx := strings.Index(text, "\r\n"); idx >= 0 {
		return text[:idx]
	}
	if idx := strings.Index(text, "\n"); idx >= 0 {
		return text[:idx]
	}
	if len(text) > 120 {
		return text[:120]
	}
	return text
}

func isEOFLike(err error) bool {
	if err == nil {
		return false
	}
	s := strings.ToLower(err.Error())
	return strings.Contains(s, "eof") || strings.Contains(s, "closed")
}
