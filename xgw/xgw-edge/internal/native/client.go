package native

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/url"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/proxy"
	"github.com/quic-go/quic-go"
	"go.uber.org/zap"
)

type Client struct {
	cfg    config.ClientConfig
	logger *zap.SugaredLogger
}

type Session struct {
	Conn *quic.Conn
	Resp control.AuthResponse

	mu              sync.RWMutex
	lastKeepalive   control.Keepalive
	lastRouteUpdate control.RouteUpdate
}

func NewClient(cfg config.ClientConfig, logger *zap.SugaredLogger) *Client {
	cfg.Normalize()
	return &Client{cfg: cfg, logger: logger}
}

func (c *Client) Connect(ctx context.Context) (*Session, error) {
	serverURL, err := url.Parse(c.cfg.ServerURL)
	if err != nil {
		return nil, err
	}
	addr := serverURL.Host
	if _, _, splitErr := net.SplitHostPort(addr); splitErr != nil {
		addr = net.JoinHostPort(addr, "443")
	}
	tlsConf := &tls.Config{
		NextProtos:         []string{c.cfg.Native.ALPN},
		ServerName:         c.cfg.ServerName,
		InsecureSkipVerify: c.cfg.TLS.InsecureSkipVerify,
		MinVersion:         tls.VersionTLS13,
	}
	quicConf := &quic.Config{
		EnableDatagrams:                c.cfg.Native.EnableUDP,
		KeepAlivePeriod:                effectiveDuration(c.cfg.Control.Keepalive.Std(), 10*time.Second),
		MaxIdleTimeout:                 effectiveDuration(c.cfg.Control.IdleTimeout.Std(), 30*time.Second),
		HandshakeIdleTimeout:           effectiveDuration(c.cfg.Control.IdleTimeout.Std(), 30*time.Second),
		InitialStreamReceiveWindow:     8 << 20,
		MaxStreamReceiveWindow:         16 << 20,
		InitialConnectionReceiveWindow: 20 << 20,
		MaxConnectionReceiveWindow:     32 << 20,
	}
	conn, err := quic.DialAddr(ctx, addr, tlsConf, quicConf)
	if err != nil {
		return nil, err
	}
	stream, err := conn.OpenStreamSync(ctx)
	if err != nil {
		_ = conn.CloseWithError(1, "auth stream failed")
		return nil, err
	}
	req := control.AuthRequest{
		Token:                c.cfg.Control.Token,
		NodeID:               "xgw-native-client",
		RequestedNode:        c.cfg.BootstrapNode,
		RequestedCongestion:  c.cfg.Control.SupportedCongestion,
		AdvertisedRxMbps:     c.cfg.Control.AdvertisedRxMbps,
		AdvertisedTxMbps:     c.cfg.Control.AdvertisedTxMbps,
		RequestedServerNames: c.cfg.Masquerade.ServerNames,
		RequestedTags:        c.cfg.Pool.RequiredTags,
	}
	if err := writeJSONMessage(stream, streamKindAuth, req); err != nil {
		_ = stream.Close()
		_ = conn.CloseWithError(1, "auth write failed")
		return nil, err
	}
	var resp control.AuthResponse
	if err := readJSONMessage(stream, streamKindAuth, &resp); err != nil {
		_ = stream.Close()
		_ = conn.CloseWithError(1, "auth read failed")
		return nil, err
	}
	_ = stream.Close()
	if !resp.OK {
		_ = conn.CloseWithError(2, "auth rejected")
		return nil, errors.New("native auth rejected")
	}
	if c.logger != nil {
		c.logger.Infof("xgw-native client connected session=%s route=%s cc=%s scheduler=%s budget_class=%s budget_priority=%s copies=%d reduced_fec=%t fast_ack=%t",
			resp.SessionID,
			resp.SelectedRoute.Name,
			resp.SelectedCongestion,
			resp.Scheduler.Mode,
			resp.DefaultBudget.FlowClass,
			resp.DefaultBudget.Priority,
			resp.DefaultBudget.PreferredCopies,
			resp.DefaultBudget.ReducedFEC,
			resp.DefaultBudget.FastACK)
	}
	return &Session{Conn: conn, Resp: resp}, nil
}

func (s *Session) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	stream, err := s.Conn.OpenStreamSync(ctx)
	if err != nil {
		return nil, err
	}
	if err := writeStreamKind(stream, streamKindTCP); err != nil {
		_ = stream.Close()
		return nil, err
	}
	if err := proxy.WriteTCPRequest(stream, target); err != nil {
		_ = stream.Close()
		return nil, err
	}
	ok, msg, err := proxy.ReadTCPResponse(stream)
	if err != nil {
		_ = stream.Close()
		return nil, err
	}
	if !ok {
		_ = stream.Close()
		if msg == "" {
			msg = "tcp proxy rejected"
		}
		return nil, errors.New(msg)
	}
	return &tcpConn{stream: stream, local: s.Conn.LocalAddr(), remote: s.Conn.RemoteAddr()}, nil
}

func (s *Session) SendDatagramTo(ctx context.Context, target string, payload []byte) error {
	_ = ctx
	packet, err := encodeDatagram(target, payload)
	if err != nil {
		return err
	}
	return s.Conn.SendDatagram(packet)
}

func (s *Session) ReceiveDatagramFrom(ctx context.Context) (Datagram, error) {
	packet, err := s.Conn.ReceiveDatagram(ctx)
	if err != nil {
		return Datagram{}, err
	}
	return decodeDatagram(packet)
}

func (s *Session) FetchRouteUpdate(ctx context.Context) (*control.RouteUpdate, error) {
	stream, err := s.Conn.OpenStreamSync(ctx)
	if err != nil {
		return nil, err
	}
	defer stream.Close()
	if err := writeStreamKind(stream, streamKindRoute); err != nil {
		return nil, err
	}
	var update control.RouteUpdate
	if err := readJSONMessage(stream, streamKindRoute, &update); err != nil {
		return nil, err
	}
	s.mu.Lock()
	s.lastRouteUpdate = update
	s.mu.Unlock()
	return &update, nil
}

func (s *Session) Keepalive(ctx context.Context, ka control.Keepalive) error {
	stream, err := s.Conn.OpenStreamSync(ctx)
	if err != nil {
		return err
	}
	defer stream.Close()
	ka.SessionID = s.Resp.SessionID
	if err := writeStreamKind(stream, streamKindKeepalive); err != nil {
		return err
	}
	if err := writeJSONMessage(stream, streamKindKeepalive, ka); err != nil {
		return err
	}
	var ack control.Keepalive
	if err := readJSONMessage(stream, streamKindKeepalive, &ack); err != nil {
		return err
	}
	s.mu.Lock()
	s.lastKeepalive = ka
	s.mu.Unlock()
	return nil
}

func (s *Session) Close() error {
	if s == nil || s.Conn == nil {
		return nil
	}
	return s.Conn.CloseWithError(0, "client closing")
}

func (s *Session) MarshalSummary() []byte {
	data, _ := json.MarshalIndent(s.Resp, "", "  ")
	return data
}

func (s *Session) LastKeepalive() control.Keepalive {
	if s == nil {
		return control.Keepalive{}
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.lastKeepalive
}

func (s *Session) LastRouteReason() string {
	if s == nil {
		return ""
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.lastRouteUpdate.Reason
}

type tcpConn struct {
	stream *quic.Stream
	local  net.Addr
	remote net.Addr
}

func (c *tcpConn) Read(p []byte) (int, error)  { return c.stream.Read(p) }
func (c *tcpConn) Write(p []byte) (int, error) { return c.stream.Write(p) }
func (c *tcpConn) Close() error {
	c.stream.CancelRead(0)
	return c.stream.Close()
}
func (c *tcpConn) LocalAddr() net.Addr                { return c.local }
func (c *tcpConn) RemoteAddr() net.Addr               { return c.remote }
func (c *tcpConn) SetDeadline(t time.Time) error      { return c.stream.SetDeadline(t) }
func (c *tcpConn) SetReadDeadline(t time.Time) error  { return c.stream.SetReadDeadline(t) }
func (c *tcpConn) SetWriteDeadline(t time.Time) error { return c.stream.SetWriteDeadline(t) }

var _ io.ReadWriteCloser = (*tcpConn)(nil)
