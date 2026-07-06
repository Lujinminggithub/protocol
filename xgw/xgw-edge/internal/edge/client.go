package edge

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/compat"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/pool"
	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"go.uber.org/zap"
)

type Client struct {
	cfg    config.ClientConfig
	logger *zap.SugaredLogger
	pool   *pool.Manager
}

type ClientSession struct {
	Resp       control.AuthResponse
	Conn       *quic.Conn
	HTTPClient *http.Client
	ClientConn *http3.ClientConn

	mu             sync.RWMutex
	reconnectHint  string
	selectedRoute  control.RouteCandidate
	selectedCC     string
	createdAt      time.Time
	lastActivatedAt time.Time
	controlServerURL string
	controlHost      string
	controlPath      string
}

func NewClient(cfg config.ClientConfig, logger *zap.SugaredLogger) *Client {
	return &Client{
		cfg:    cfg,
		logger: logger,
		pool:   pool.NewManager(cfg.Pool),
	}
}

func (c *Client) Connect(ctx context.Context) (*ClientSession, error) {
	selectedServer := c.cfg.ServerURL
	if len(c.cfg.Pool.Nodes) > 0 {
		if c.cfg.BootstrapNode != "" {
			for _, node := range c.cfg.Pool.Nodes {
				if node.Name == c.cfg.BootstrapNode && node.Address != "" {
					selectedServer = node.Address
					goto selected
				}
			}
		}
		best, _ := c.pool.Select(control.AuthRequest{
			Token:                c.cfg.Control.Token,
			RequestedNode:        c.cfg.BootstrapNode,
			RequestedCongestion:  c.cfg.Control.SupportedCongestion,
			RequestedServerNames: c.cfg.Masquerade.ServerNames,
		})
		if best.Address != "" {
			selectedServer = best.Address
		}
	}
selected:

	serverURL, err := url.Parse(selectedServer)
	if err != nil {
		return nil, err
	}
	addr := serverURL.Host
	if _, _, splitErr := net.SplitHostPort(addr); splitErr != nil {
		if serverURL.Scheme == "https" {
			addr = net.JoinHostPort(addr, "443")
		}
	}

	tlsConf := &tls.Config{
		NextProtos:         c.cfg.TLS.ALPN,
		ServerName:         c.cfg.ServerName,
		InsecureSkipVerify: c.cfg.TLS.InsecureSkipVerify,
		MinVersion:         tls.VersionTLS13,
	}
	quicConf := &quic.Config{
		EnableDatagrams:                true,
		KeepAlivePeriod:                c.cfg.Control.Keepalive.Std(),
		MaxIdleTimeout:                 c.cfg.Control.IdleTimeout.Std(),
		HandshakeIdleTimeout:           c.cfg.Control.IdleTimeout.Std(),
		InitialStreamReceiveWindow:     8 << 20,
		MaxStreamReceiveWindow:         16 << 20,
		InitialConnectionReceiveWindow: 20 << 20,
		MaxConnectionReceiveWindow:     32 << 20,
	}
	conn, err := quic.DialAddr(ctx, addr, tlsConf, quicConf)
	if err != nil {
		return nil, err
	}

	roundTripper := &http3.Transport{
		TLSClientConfig: tlsConf,
		QUICConfig:      quicConf,
		EnableDatagrams: true,
	}
	clientConn := roundTripper.NewClientConn(conn)
	httpClient := &http.Client{
		Transport: roundTripper,
	}

	var req *http.Request
	if c.cfg.Compatibility.EnableHysteriaAuth {
		req, err = http.NewRequestWithContext(ctx, http.MethodPost, selectedServer+c.cfg.Control.AuthPath, http.NoBody)
		if err != nil {
			return nil, err
		}
		req.Header.Set(compat.RequestHeaderAuth, c.cfg.Control.Token)
		if c.cfg.Control.AdvertisedRxMbps > 0 {
			req.Header.Set(compat.CommonHeaderCCRX, strconv.FormatUint(c.cfg.Control.AdvertisedRxMbps*125000, 10))
		} else {
			req.Header.Set(compat.CommonHeaderCCRX, "0")
		}
	} else {
		reqBody, _ := json.Marshal(control.AuthRequest{
			Token:                c.cfg.Control.Token,
			NodeID:               "client-default",
			RequestedNode:        c.cfg.BootstrapNode,
			RequestedCongestion:  c.cfg.Control.SupportedCongestion,
			AdvertisedRxMbps:     c.cfg.Control.AdvertisedRxMbps,
			AdvertisedTxMbps:     c.cfg.Control.AdvertisedTxMbps,
			RequestedServerNames: c.cfg.Masquerade.ServerNames,
			RequestedTags:        c.cfg.Pool.RequiredTags,
		})
		req, err = http.NewRequestWithContext(ctx, http.MethodPost, selectedServer+c.cfg.Control.AuthPath, bytes.NewReader(reqBody))
		if err != nil {
			return nil, err
		}
		req.Header.Set("Content-Type", "application/json")
	}
	if err != nil {
		return nil, err
	}
	req.Host = c.cfg.Control.AuthHost
	resp, err := clientConn.RoundTrip(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	var authResp control.AuthResponse
	if c.cfg.Compatibility.EnableHysteriaAuth {
		rxStr := resp.Header.Get(compat.CommonHeaderCCRX)
		var rxMbps uint64
		if rxStr != "" && rxStr != "auto" {
			if rxBytes, err := strconv.ParseUint(rxStr, 10, 64); err == nil {
				rxMbps = rxBytes / 125000
			}
		}
		authResp = control.AuthResponse{
			OK:                 true,
			SessionID:          "",
			SelectedCongestion: "bbr",
			AdvertisedRxMbps:   rxMbps,
			AdvertisedTxMbps:   0,
			SelectedRoute: control.RouteCandidate{
				Name:    c.cfg.Compatibility.ShadowrocketName,
				Address: selectedServer,
			},
			AllowDatagrams: true,
		}
	} else {
		if err := json.Unmarshal(data, &authResp); err != nil {
			return nil, err
		}
	}
	c.logger.Infof("xgw-edge client connected route=%s cc=%s", authResp.SelectedRoute.Name, authResp.SelectedCongestion)
	session := &ClientSession{
		Resp:          authResp,
		Conn:          conn,
		HTTPClient:    httpClient,
		ClientConn:    clientConn,
		reconnectHint: authResp.SelectedRoute.BackendUDPAddr,
		selectedRoute: authResp.SelectedRoute,
		selectedCC:    authResp.SelectedCongestion,
		createdAt:     time.Now(),
		lastActivatedAt: time.Now(),
		controlServerURL: selectedServer,
		controlHost:      c.cfg.Control.AuthHost,
		controlPath:      c.cfg.Control.AuthPath,
	}
	StartClientKeepalive(ctx, c.logger, httpClient, selectedServer, c.cfg.Control.AuthHost, c.cfg.Control.AuthPath, authResp.SessionID, c.cfg.Control.Keepalive.Std())
	return session, nil
}

func (s *ClientSession) SendDatagram(ctx context.Context, payload []byte) error {
	_ = ctx
	return s.Conn.SendDatagram(payload)
}

func (s *ClientSession) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	return s.Conn.ReceiveDatagram(ctx)
}

func (s *ClientSession) StartRouteUpdates(ctx context.Context, logger *zap.SugaredLogger, serverURL string, host string, path string, interval time.Duration) {
	if interval <= 0 {
		return
	}
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				update, err := s.FetchRouteUpdate(ctx, serverURL, host, path)
				if err != nil {
					logger.Warnf("route update failed: %v", err)
					continue
				}
				s.mu.Lock()
				s.reconnectHint = update.ReconnectHint
				s.selectedRoute = update.SelectedRoute
				s.selectedCC = update.SelectedCongestion
				s.mu.Unlock()
				logger.Infof("route updated route=%s hint=%s reason=%s", update.SelectedRoute.Name, update.ReconnectHint, update.Reason)
			}
		}
	}()
}

func (s *ClientSession) FetchRouteUpdate(ctx context.Context, serverURL string, host string, path string) (*control.RouteUpdate, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, serverURL+path+"/route?session_id="+url.QueryEscape(s.Resp.SessionID), nil)
	if err != nil {
		return nil, err
	}
	req.Host = host
	resp, err := s.ClientConn.RoundTrip(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	var update control.RouteUpdate
	if err := json.Unmarshal(data, &update); err != nil {
		return nil, err
	}
	return &update, nil
}

func (s *ClientSession) CurrentReconnectHint() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.reconnectHint
}

func (s *ClientSession) CurrentRoute() control.RouteCandidate {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.selectedRoute
}

func (s *ClientSession) MarkActivated() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lastActivatedAt = time.Now()
}

func (s *ClientSession) FetchRouteUpdateForSession(ctx context.Context) (*control.RouteUpdate, error) {
	s.mu.RLock()
	serverURL := s.controlServerURL
	host := s.controlHost
	path := s.controlPath
	s.mu.RUnlock()
	return s.FetchRouteUpdate(ctx, serverURL, host, path)
}

func (s *ClientSession) LocalUDPProxy(ctx context.Context, listenAddr string) error {
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
			msg, err := s.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			peerMu.RLock()
			dst := peer
			peerMu.RUnlock()
			if dst != nil {
				_, _ = conn.WriteToUDP(msg, dst)
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
		peerMu.Lock()
		peer = addr
		peerMu.Unlock()
		if err := s.SendDatagram(ctx, buf[:n]); err != nil {
			return err
		}
	}
}

func (s *ClientSession) Close() error {
	if s.Conn != nil {
		return s.Conn.CloseWithError(0, "client closing")
	}
	return nil
}
