package officialbridge

import (
	"context"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	coreServer "github.com/apernet/hysteria/core/v2/server"
	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/coremodel"
	"github.com/local/xgw-edge/internal/masq"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

const (
	connectTypeBridge = "bridge"
	connectTypeDirect = "direct"
	bridgeProtoUDP    = 2
)

var bridgeCopyBufPool = sync.Pool{
	New: func() any {
		buf := make([]byte, 64*1024)
		return &buf
	},
}

type OfficialBridgeServer struct {
	cfg        config.ServerConfig
	logger     *zap.SugaredLogger
	server     coreServer.Server
	packetConn net.PacketConn
	backend    *native.BackendAdapter
}

type officialBridgeAuthenticator struct {
	token  string
	logger *zap.SugaredLogger
}

type officialBridgeOutbound struct {
	cfg       config.ServerConfig
	logger    *zap.SugaredLogger
	backend   *native.BackendAdapter
}

type officialBridgeSessionContext struct {
	sessionID string
	frontend  string
	routeName string
	lineID    string
	remote    string
	authID    string
	connID    uint32
	udpID     uint32
}

type officialBridgeConn struct {
	net.Conn
	id       uint64
	target   string
	mode     string
	logger   *zap.SugaredLogger
	readOnce sync.Once
	closeOnce sync.Once
	stats    *officialBridgeFlowStats
}

type officialBridgeEventLogger struct {
	logger *zap.SugaredLogger
}

type officialUDPConn struct {
	*net.UDPConn
}

type officialBridgeUDPConn struct {
	conn    net.Conn
	id      uint64
	target  string
	logger  *zap.SugaredLogger
	readMu  sync.Mutex
	writeMu sync.Mutex
	stats   *officialBridgeFlowStats
}

var officialBridgeConnSeq uint64
var officialBridgeUDPSeq uint64

func isTikTokTraceTarget(target string) bool {
	target = strings.ToLower(strings.TrimSpace(target))
	return strings.Contains(target, "mon-boot.tiktokv.com:443") ||
		strings.Contains(target, "api-boot.tiktokv.com:443") ||
		strings.Contains(target, "tnc-boot.tiktokv.com:443") ||
		strings.Contains(target, "frontier.tiktokv.com:443") ||
		strings.Contains(target, "log-boot.tiktokv.com:443") ||
		strings.Contains(target, "gecko-boot.tiktokv.com:443") ||
		strings.Contains(target, "jsb-boot.tiktokv.com:443")
}

type officialBridgeFlowStats struct {
	proto            string
	id               uint64
	target           string
	logger           *zap.SugaredLogger
	startedAt        time.Time
	probeLike        bool
	model            coremodel.FlowRequest
	openDoneUnixNano atomic.Int64
	firstPacketOnce  sync.Once
	firstPacketMs    atomic.Int64
	firstByteOnce    sync.Once
	firstByteMs      atomic.Int64
	frontToBridgeBytes  atomic.Int64
	frontToBridgeChunks atomic.Int64
	bridgeToFrontBytes  atomic.Int64
	bridgeToFrontChunks atomic.Int64
	lastFrontToBridgeNs atomic.Int64
	lastBridgeToFrontNs atomic.Int64
	lastFrontChunkLogNs atomic.Int64
	lastBackChunkLogNs  atomic.Int64
	closeOnce        sync.Once
}

func classifierForServerConfig(cfg config.ServerConfig) coremodel.FlowClassifier {
	return cfg.FlowClassifier.ToCore()
}

type flowStatsReader struct {
	io.Reader
	onRead func(int)
}

func (r *flowStatsReader) Read(p []byte) (int, error) {
	n, err := r.Reader.Read(p)
	if n > 0 && r.onRead != nil {
		r.onRead(n)
	}
	return n, err
}

type flowStatsWriter struct {
	io.Writer
	onWrite func(int)
}

func (w *flowStatsWriter) Write(p []byte) (int, error) {
	n, err := w.Writer.Write(p)
	if n > 0 && w.onWrite != nil {
		w.onWrite(n)
	}
	return n, err
}

func newOfficialBridgeFlowStats(proto string, id uint64, target string, logger *zap.SugaredLogger) *officialBridgeFlowStats {
	classifier := config.DefaultServer().FlowClassifier.ToCore()
	return newOfficialBridgeFlowStatsWithConfig(proto, id, target, classifier, logger)
}

func newOfficialBridgeFlowStatsWithConfig(proto string, id uint64, target string, classifier coremodel.FlowClassifier, logger *zap.SugaredLogger) *officialBridgeFlowStats {
	kind := coremodel.FlowKindTCP
	if strings.EqualFold(proto, "udp") {
		kind = coremodel.FlowKindUDP
	}
	model := coremodel.ClassifyFlowWithRules(kind, target, &classifier)
	stats := &officialBridgeFlowStats{
		proto:     proto,
		id:        id,
		target:    target,
		logger:    logger,
		startedAt: time.Now(),
		probeLike: false,
		model:     model,
	}
	stats.firstPacketMs.Store(-1)
	stats.firstByteMs.Store(-1)
	return stats
}

func (s *officialBridgeFlowStats) markOpen() {
	if s == nil {
		return
	}
	now := time.Now()
	s.openDoneUnixNano.Store(now.UnixNano())
	if s.probeLike {
		s.logger.Infof("hy2front.flow.probe_like proto=%s conn=%d target=%s class=%s priority=%s idle_after_first_byte_ms=%d read_timeout_ms=%d",
			s.proto,
			s.id,
			s.target,
			s.model.Class,
			s.model.Priority.String(),
			s.idleAfterFirstByte().Milliseconds(),
			s.readFromTimeout().Milliseconds())
	}
	s.logger.Infof("hy2front.flow.open proto=%s conn=%d target=%s open_ms=%d",
		s.proto,
		s.id,
		s.target,
		durationMillis(s.startedAt, now))
	if strings.EqualFold(s.proto, "tcp") && isTikTokTraceTarget(s.target) {
		s.logger.Infof("tiktok.trace.front.open conn=%d target=%s open_ms=%d", s.id, s.target, durationMillis(s.startedAt, now))
	}
}

func (s *officialBridgeFlowStats) noteFirstPacket(bytes int) {
	if s == nil || bytes <= 0 {
		return
	}
	s.firstPacketOnce.Do(func() {
		ms := durationMillis(s.startedAt, time.Now())
		s.firstPacketMs.Store(ms)
		s.logger.Infof("hy2front.flow.first_packet proto=%s conn=%d target=%s elapsed_ms=%d bytes=%d",
			s.proto,
			s.id,
			s.target,
			ms,
			bytes)
		if strings.EqualFold(s.proto, "tcp") && isTikTokTraceTarget(s.target) {
			s.logger.Infof("tiktok.trace.front.first_packet conn=%d target=%s elapsed_ms=%d bytes=%d", s.id, s.target, ms, bytes)
		}
	})
}

func (s *officialBridgeFlowStats) noteFirstByte(bytes int) {
	if s == nil || bytes <= 0 {
		return
	}
	s.firstByteOnce.Do(func() {
		ms := durationMillis(s.startedAt, time.Now())
		s.firstByteMs.Store(ms)
		s.logger.Infof("hy2front.flow.first_byte proto=%s conn=%d target=%s elapsed_ms=%d bytes=%d",
			s.proto,
			s.id,
			s.target,
			ms,
			bytes)
		if strings.EqualFold(s.proto, "tcp") && isTikTokTraceTarget(s.target) {
			s.logger.Infof("tiktok.trace.front.first_byte conn=%d target=%s elapsed_ms=%d bytes=%d", s.id, s.target, ms, bytes)
		}
	})
}

func (s *officialBridgeFlowStats) noteChunk(direction string, bytes int) {
	if s == nil || bytes <= 0 {
		return
	}
	now := time.Now()
	nowNs := now.UnixNano()
	var total int64
	var chunks int64
	var gapMs int64 = -1
	var shouldLog bool
	var lastLogNs int64
	switch direction {
	case "front_to_bridge":
		total = s.frontToBridgeBytes.Add(int64(bytes))
		chunks = s.frontToBridgeChunks.Add(1)
		lastNs := s.lastFrontToBridgeNs.Swap(nowNs)
		if lastNs > 0 {
			gapMs = now.Sub(time.Unix(0, lastNs)).Milliseconds()
		}
		lastLogNs = s.lastFrontChunkLogNs.Load()
	case "bridge_to_front":
		total = s.bridgeToFrontBytes.Add(int64(bytes))
		chunks = s.bridgeToFrontChunks.Add(1)
		lastNs := s.lastBridgeToFrontNs.Swap(nowNs)
		if lastNs > 0 {
			gapMs = now.Sub(time.Unix(0, lastNs)).Milliseconds()
		}
		lastLogNs = s.lastBackChunkLogNs.Load()
	default:
		return
	}
	shouldLog = chunks == 1 || bytes >= 2048 || gapMs >= 500 || (lastLogNs > 0 && now.Sub(time.Unix(0, lastLogNs)) >= 2*time.Second)
	if !shouldLog {
		return
	}
	if direction == "front_to_bridge" {
		s.lastFrontChunkLogNs.Store(nowNs)
	} else {
		s.lastBackChunkLogNs.Store(nowNs)
	}
	s.logger.Infof("hy2front.flow.chunk proto=%s conn=%d target=%s direction=%s chunk_bytes=%d total_bytes=%d chunks=%d gap_ms=%d",
		s.proto,
		s.id,
		s.target,
		direction,
		bytes,
		total,
		chunks,
		gapMs)
}

func (s *officialBridgeFlowStats) idleAfterFirstByte() time.Duration {
	if s == nil {
		return 0
	}
	if s.model.Budget.IdleAfterFirstByte > 0 {
		return s.model.Budget.IdleAfterFirstByte
	}
	return 15 * time.Minute
}

func (s *officialBridgeFlowStats) readFromTimeout() time.Duration {
	if s == nil {
		return 0
	}
	if s.model.Budget.ReadTimeout > 0 {
		return s.model.Budget.ReadTimeout
	}
	return 10 * time.Minute
}

func (s *officialBridgeFlowStats) finish(reason string) {
	if s == nil {
		return
	}
	s.closeOnce.Do(func() {
		now := time.Now()
		openMs := int64(-1)
		if ns := s.openDoneUnixNano.Load(); ns > 0 {
			openMs = durationMillis(s.startedAt, time.Unix(0, ns))
		}
		s.logger.Infof("hy2front.flow.close proto=%s conn=%d target=%s lifetime_ms=%d open_ms=%d first_packet_ms=%d first_byte_ms=%d reason=%s",
			s.proto,
			s.id,
			s.target,
			durationMillis(s.startedAt, now),
			openMs,
			s.firstPacketMs.Load(),
			s.firstByteMs.Load(),
			compactReason(reason))
		if strings.EqualFold(s.proto, "tcp") && isTikTokTraceTarget(s.target) {
			s.logger.Infof("tiktok.trace.front.close conn=%d target=%s lifetime_ms=%d open_ms=%d first_packet_ms=%d first_byte_ms=%d reason=%s",
				s.id,
				s.target,
				durationMillis(s.startedAt, now),
				openMs,
				s.firstPacketMs.Load(),
				s.firstByteMs.Load(),
				compactReason(reason))
		}
		s.logger.Infof("hy2front.flow.accounting proto=%s conn=%d target=%s front_to_bridge_bytes=%d front_to_bridge_chunks=%d bridge_to_front_bytes=%d bridge_to_front_chunks=%d",
			s.proto,
			s.id,
			s.target,
			s.frontToBridgeBytes.Load(),
			s.frontToBridgeChunks.Load(),
			s.bridgeToFrontBytes.Load(),
			s.bridgeToFrontChunks.Load())
	})
}

func durationMillis(start time.Time, end time.Time) int64 {
	if end.Before(start) {
		return 0
	}
	return end.Sub(start).Milliseconds()
}

func compactReason(reason string) string {
	reason = strings.TrimSpace(reason)
	if reason == "" {
		return "unknown"
	}
	reason = strings.NewReplacer("\r", "_", "\n", "_", "\t", "_", " ", "_").Replace(reason)
	return reason
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func NewOfficialBridgeServer(cfg config.ServerConfig, logger *zap.SugaredLogger) (*OfficialBridgeServer, error) {
	var err error
	cfg.Normalize()
	if cfg.ConnectType != connectTypeBridge && cfg.ConnectType != connectTypeDirect {
		return nil, fmt.Errorf("unsupported connect_type %q", cfg.ConnectType)
	}
	backend, err := native.NewBackendAdapter(cfg)
	if err != nil {
		return nil, err
	}

	cert, err := tls.LoadX509KeyPair(cfg.TLS.CertFile, cfg.TLS.KeyFile)
	if err != nil {
		_ = backend.Close()
		return nil, fmt.Errorf("load tls key pair: %w", err)
	}
	pc, err := net.ListenPacket("udp", cfg.Listen)
	if err != nil {
		_ = backend.Close()
		return nil, fmt.Errorf("listen udp %s: %w", cfg.Listen, err)
	}

	masqHandler, err := masq.New(cfg.Masquerade)
	if err != nil {
		_ = backend.Close()
		_ = pc.Close()
		return nil, err
	}
	disableUDP := !cfg.Compatibility.EnableHysteriaUDP
	idleTimeout := effectiveDuration(cfg.Control.IdleTimeout.Std(), 120*time.Second)
	coreCfg := &coreServer.Config{
		TLSConfig: coreServer.TLSConfig{
			Certificates: []tls.Certificate{cert},
		},
		QUICConfig: coreServer.QUICConfig{
			MaxIdleTimeout:          idleTimeout,
			KeepAlivePeriod:         15 * time.Second,
			DisablePathMTUDiscovery: false,
		},
		Conn:                  pc,
		Outbound:              &officialBridgeOutbound{cfg: cfg, logger: logger, backend: backend},
		CongestionConfig:      coreServer.CongestionConfig{Type: chooseOfficialCongestion(cfg.Control.SupportedCongestion)},
		BandwidthConfig:       coreServer.BandwidthConfig{MaxRx: cfg.Control.AdvertisedRxMbps * 125000, MaxTx: cfg.Control.AdvertisedTxMbps * 125000},
		IgnoreClientBandwidth: true,
		DisableUDP:            disableUDP,
		UDPIdleTimeout:        effectiveDuration(idleTimeout, 300*time.Second),
		Authenticator:         officialBridgeAuthenticator{token: cfg.Control.Token, logger: logger},
		EventLogger:           officialBridgeEventLogger{logger: logger},
		MasqHandler:           http.HandlerFunc(masqHandler.ServeFallback),
	}
	server, err := coreServer.NewServer(coreCfg)
	if err != nil {
		_ = backend.Close()
		_ = pc.Close()
		return nil, err
	}
	return &OfficialBridgeServer{
		cfg:        cfg,
		logger:     logger,
		server:     server,
		packetConn: pc,
		backend:    backend,
	}, nil
}

func (s *OfficialBridgeServer) Serve() error {
	s.logger.Infof("xgw-edge official hy2 front listen=%s connect_type=%s bridge_transport=%s bridge_tcp=%s bridge_ring=%s udp_enabled=%t alpn=%v",
		s.cfg.Listen,
		s.cfg.ConnectType,
		s.cfg.BridgeTransport,
		s.cfg.BridgeTCPAddr,
		s.cfg.BridgeRingPath,
		s.cfg.Compatibility.EnableHysteriaUDP,
		s.cfg.TLS.ALPN,
	)
	return s.server.Serve()
}

func (s *OfficialBridgeServer) Close() error {
	if s.backend != nil {
		defer s.backend.Close()
	}
	if s.server != nil {
		return s.server.Close()
	}
	if s.packetConn != nil {
		return s.packetConn.Close()
	}
	return nil
}

func (a officialBridgeAuthenticator) Authenticate(addr net.Addr, auth string, tx uint64) (bool, string) {
	remote := ""
	if addr != nil {
		remote = addr.String()
	}
	if a.token == "" {
		a.logger.Infof("auth allow remote=%s tx=%d mode=anonymous", remote, tx)
		return true, "anonymous"
	}
	if auth == a.token {
		a.logger.Infof("auth allow remote=%s tx=%d auth_len=%d", remote, tx, len(auth))
		return true, auth
	}
	a.logger.Warnf("auth deny remote=%s tx=%d auth_len=%d", remote, tx, len(auth))
	return false, ""
}

func (o *officialBridgeOutbound) TCP(reqAddr string) (net.Conn, error) {
	connID := atomic.AddUint64(&officialBridgeConnSeq, 1)
	if o.cfg.ConnectType == connectTypeDirect {
		o.logger.Infof("tcp.open.direct.begin conn=%d target=%s", connID, reqAddr)
		conn, err := net.DialTimeout("tcp", reqAddr, 10*time.Second)
		if err != nil {
			o.logger.Warnf("tcp.open.direct.fail conn=%d target=%s err=%v", connID, reqAddr, err)
			return nil, err
		}
		o.logger.Infof("tcp.open.direct.ok conn=%d target=%s", connID, reqAddr)
		stats := newOfficialBridgeFlowStats("tcp", connID, reqAddr, o.logger)
		stats.markOpen()
		return &officialBridgeConn{Conn: conn, id: connID, target: reqAddr, mode: o.cfg.BridgeCopyMode, logger: o.logger, stats: stats}, nil
	}

	o.logger.Infof("tcp.open.bridge.begin conn=%d target=%s bridge_tcp=%s", connID, reqAddr, o.cfg.BridgeTCPAddr)
	if o.backend == nil {
		return nil, errors.New("backend adapter is not initialized")
	}
	ctx, cancel := contextWithBridgeTimeout()
	defer cancel()
	classifier := classifierForServerConfig(o.cfg)
	openReq := coremodel.OpenRequest{
		Flow: coremodel.ClassifyFlowWithRules(coremodel.FlowKindTCP, reqAddr, &classifier),
		Session: coremodel.DefaultSessionSemantic("hy2-compat"),
	}
	o.logger.Debugf("tcp.classify target=%s class=%s priority=%s session_id=%s route=%s line=%s",
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID)
	o.logger.Infof("tcp.classify conn=%d target=%s class=%s priority=%s frontend=%s session_id=%s route=%s line=%s",
		connID,
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.Frontend,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID)
	o.logger.Infof("tcp.open.bridge.meta conn=%d target=%s frontend=%s session_id=%s route=%s line=%s",
		connID,
		reqAddr,
		openReq.Session.Frontend,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID)
	conn, _, err := o.backend.OpenTCP(ctx, openReq)
	if err != nil {
		o.logger.Warnf("tcp.open.bridge_dial.fail conn=%d target=%s transport=%s err=%v", connID, reqAddr, o.cfg.BridgeTransport, err)
		return nil, err
	}
	o.logger.Infof("tcp.open.bridge.ok conn=%d target=%s", connID, reqAddr)
	stats := newOfficialBridgeFlowStatsWithConfig("tcp", connID, reqAddr, classifierForServerConfig(o.cfg), o.logger)
	stats.markOpen()
	return &officialBridgeConn{Conn: conn, id: connID, target: reqAddr, mode: o.cfg.BridgeCopyMode, logger: o.logger, stats: stats}, nil
}

func (o *officialBridgeOutbound) TCPContext(ctx coreServer.OutboundContext, reqAddr string) (net.Conn, error) {
	connID := atomic.AddUint64(&officialBridgeConnSeq, 1)
	if o.cfg.ConnectType == connectTypeDirect {
		o.logger.Infof("tcp.open.direct.begin conn=%d target=%s remote=%s auth=%s", connID, reqAddr, addrString(ctx.RemoteAddr), ctx.AuthID)
		conn, err := net.DialTimeout("tcp", reqAddr, 10*time.Second)
		if err != nil {
			o.logger.Warnf("tcp.open.direct.fail conn=%d target=%s err=%v", connID, reqAddr, err)
			return nil, err
		}
		o.logger.Infof("tcp.open.direct.ok conn=%d target=%s", connID, reqAddr)
		stats := newOfficialBridgeFlowStats("tcp", connID, reqAddr, o.logger)
		stats.markOpen()
		return &officialBridgeConn{Conn: conn, id: connID, target: reqAddr, mode: o.cfg.BridgeCopyMode, logger: o.logger, stats: stats}, nil
	}
	if o.backend == nil {
		return nil, errors.New("backend adapter is not initialized")
	}
	ctxDial, cancel := contextWithBridgeTimeout()
	defer cancel()
	classifier := classifierForServerConfig(o.cfg)
	openReq := coremodel.OpenRequest{
		Flow:    coremodel.ClassifyFlowWithRules(coremodel.FlowKindTCP, reqAddr, &classifier),
		Session: o.semanticFromContext(ctx, reqAddr, 0),
	}
	o.logger.Debugf("tcp.classify target=%s class=%s priority=%s session_id=%s route=%s line=%s",
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID)
	o.logger.Infof("tcp.classify conn=%d target=%s class=%s priority=%s frontend=%s session_id=%s route=%s line=%s remote=%s auth=%s hy2_conn=%d",
		connID,
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.Frontend,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID,
		addrString(ctx.RemoteAddr),
		ctx.AuthID,
		ctx.ConnID)
	o.logger.Infof("tcp.open.bridge.begin conn=%d target=%s frontend=%s session_id=%s route=%s line=%s remote=%s auth=%s hy2_conn=%d",
		connID,
		reqAddr,
		openReq.Session.Frontend,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID,
		addrString(ctx.RemoteAddr),
		ctx.AuthID,
		ctx.ConnID)
	conn, _, err := o.backend.OpenTCP(ctxDial, openReq)
	if err != nil {
		o.logger.Warnf("tcp.open.bridge_dial.fail conn=%d target=%s transport=%s err=%v", connID, reqAddr, o.cfg.BridgeTransport, err)
		return nil, err
	}
	o.logger.Infof("tcp.open.bridge.ok conn=%d target=%s session_id=%s route=%s line=%s",
		connID,
		reqAddr,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID)
	stats := newOfficialBridgeFlowStatsWithConfig("tcp", connID, reqAddr, classifierForServerConfig(o.cfg), o.logger)
	stats.markOpen()
	return &officialBridgeConn{Conn: conn, id: connID, target: reqAddr, mode: o.cfg.BridgeCopyMode, logger: o.logger, stats: stats}, nil
}

func (o *officialBridgeOutbound) UDP(reqAddr string) (coreServer.UDPConn, error) {
	if o.cfg.ConnectType == connectTypeBridge {
		connID := atomic.AddUint64(&officialBridgeUDPSeq, 1)
		o.logger.Infof("udp.open.bridge.begin conn=%d target=%s bridge_tcp=%s", connID, reqAddr, o.cfg.BridgeTCPAddr)
		if o.backend == nil {
			return nil, errors.New("backend adapter is not initialized")
		}
		ctx, cancel := contextWithBridgeTimeout()
		defer cancel()
		classifier := classifierForServerConfig(o.cfg)
		openReq := coremodel.OpenRequest{
			Flow: coremodel.ClassifyFlowWithRules(coremodel.FlowKindUDP, reqAddr, &classifier),
			Session: coremodel.DefaultSessionSemantic("hy2-compat"),
		}
		o.logger.Debugf("udp.classify target=%s class=%s priority=%s session_id=%s route=%s line=%s udp_session=%d",
			reqAddr,
			openReq.Flow.Class,
			openReq.Flow.Priority.String(),
			openReq.Session.SessionID,
			openReq.Session.RouteName,
			openReq.Session.LineID,
			0)
		o.logger.Infof("udp.classify conn=%d target=%s class=%s priority=%s frontend=%s session_id=%s route=%s line=%s udp_session=%d",
			connID,
			reqAddr,
			openReq.Flow.Class,
			openReq.Flow.Priority.String(),
			openReq.Session.Frontend,
			openReq.Session.SessionID,
			openReq.Session.RouteName,
			openReq.Session.LineID,
			0)
		o.logger.Infof("udp.open.bridge.meta conn=%d target=%s frontend=%s session_id=%s route=%s line=%s",
			connID,
			reqAddr,
			openReq.Session.Frontend,
			openReq.Session.SessionID,
			openReq.Session.RouteName,
			openReq.Session.LineID)
		conn, _, err := o.backend.OpenUDP(ctx, openReq)
		if err != nil {
			o.logger.Warnf("udp.open.bridge_dial.fail conn=%d target=%s transport=%s err=%v", connID, reqAddr, o.cfg.BridgeTransport, err)
			return nil, err
		}
		o.logger.Infof("udp.open.bridge.ok conn=%d target=%s", connID, reqAddr)
		stats := newOfficialBridgeFlowStatsWithConfig("udp", connID, reqAddr, classifierForServerConfig(o.cfg), o.logger)
		stats.markOpen()
		o.logger.Infof("udp.bridge.session conn=%d target=%s class=%s priority=%s idle_after_first_byte_ms=%d read_timeout_ms=%d",
			connID,
			reqAddr,
			stats.model.Class,
			stats.model.Priority.String(),
			stats.idleAfterFirstByte().Milliseconds(),
			stats.readFromTimeout().Milliseconds())
		return &officialBridgeUDPConn{conn: conn, id: connID, target: reqAddr, logger: o.logger, stats: stats}, nil
	}
	conn, err := net.ListenUDP("udp", nil)
	if err != nil {
		o.logger.Warnf("udp.open.fail target=%s err=%v", reqAddr, err)
		return nil, err
	}
	o.logger.Infof("udp.open.direct.ok target=%s local=%s", reqAddr, conn.LocalAddr())
	return &officialUDPConn{UDPConn: conn}, nil
}

func (o *officialBridgeOutbound) UDPContext(ctx coreServer.OutboundContext, reqAddr string) (coreServer.UDPConn, error) {
	if o.cfg.ConnectType != connectTypeBridge {
		return o.UDP(reqAddr)
	}
	connID := atomic.AddUint64(&officialBridgeUDPSeq, 1)
	o.logger.Infof("udp.open.bridge.begin conn=%d target=%s remote=%s auth=%s udp_session=%d", connID, reqAddr, addrString(ctx.RemoteAddr), ctx.AuthID, ctx.UDPSessionID)
	if o.backend == nil {
		return nil, errors.New("backend adapter is not initialized")
	}
	ctxDial, cancel := contextWithBridgeTimeout()
	defer cancel()
	classifier := classifierForServerConfig(o.cfg)
	openReq := coremodel.OpenRequest{
		Flow:    coremodel.ClassifyFlowWithRules(coremodel.FlowKindUDP, reqAddr, &classifier),
		Session: o.semanticFromContext(ctx, reqAddr, ctx.UDPSessionID),
	}
	fmt.Printf("udp.classify target=%s class=%s priority=%s session_id=%s route=%s line=%s udp_session=%d\n",
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID,
		ctx.UDPSessionID)
	o.logger.Infof("udp.classify conn=%d target=%s class=%s priority=%s frontend=%s session_id=%s route=%s line=%s remote=%s auth=%s udp_session=%d",
		connID,
		reqAddr,
		openReq.Flow.Class,
		openReq.Flow.Priority.String(),
		openReq.Session.Frontend,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID,
		addrString(ctx.RemoteAddr),
		ctx.AuthID,
		ctx.UDPSessionID)
	conn, _, err := o.backend.OpenUDP(ctxDial, openReq)
	if err != nil {
		o.logger.Warnf("udp.open.bridge_dial.fail conn=%d target=%s transport=%s err=%v", connID, reqAddr, o.cfg.BridgeTransport, err)
		return nil, err
	}
	o.logger.Infof("udp.open.bridge.ok conn=%d target=%s session_id=%s route=%s line=%s udp_session=%d",
		connID,
		reqAddr,
		openReq.Session.SessionID,
		openReq.Session.RouteName,
		openReq.Session.LineID,
		ctx.UDPSessionID)
	stats := newOfficialBridgeFlowStatsWithConfig("udp", connID, reqAddr, classifierForServerConfig(o.cfg), o.logger)
	stats.markOpen()
	o.logger.Infof("udp.bridge.session conn=%d target=%s class=%s priority=%s idle_after_first_byte_ms=%d read_timeout_ms=%d",
		connID,
		reqAddr,
		stats.model.Class,
		stats.model.Priority.String(),
		stats.idleAfterFirstByte().Milliseconds(),
		stats.readFromTimeout().Milliseconds())
	return &officialBridgeUDPConn{conn: conn, id: connID, target: reqAddr, logger: o.logger, stats: stats}, nil
}

func (o *officialBridgeOutbound) CheckUDP(reqAddr string) error {
	o.logger.Infof("udp.check.ok target=%s", reqAddr)
	return nil
}

func (o *officialBridgeOutbound) semanticFromContext(ctx coreServer.OutboundContext, reqAddr string, udpSessionID uint32) coremodel.SessionSemantic {
	semantic := coremodel.DefaultSessionSemantic("hy2-compat")
	semantic.Frontend = "hy2-compat"
	semantic.RouteName = o.defaultRouteName()
	semantic.LineID = o.defaultLineID()
	semantic.SessionID = officialBridgeCanonicalSessionID(ctx, reqAddr, udpSessionID)
	return semantic
}

func (o *officialBridgeOutbound) defaultRouteName() string {
	if len(o.cfg.Pool.Lines) > 0 {
		line := o.cfg.Pool.Lines[0]
		if strings.TrimSpace(line.Name) != "" {
			return strings.TrimSpace(line.Name)
		}
		if strings.TrimSpace(line.ID) != "" {
			return strings.TrimSpace(line.ID)
		}
	}
	return "hy2-ingress-route"
}

func (o *officialBridgeOutbound) defaultLineID() string {
	if len(o.cfg.Pool.Lines) > 0 {
		line := o.cfg.Pool.Lines[0]
		if strings.TrimSpace(line.ID) != "" {
			return strings.TrimSpace(line.ID)
		}
		if strings.TrimSpace(line.Name) != "" {
			return strings.TrimSpace(line.Name)
		}
	}
	return "primary"
}

func officialBridgeCanonicalSessionID(ctx coreServer.OutboundContext, reqAddr string, udpSessionID uint32) string {
	remote := addrString(ctx.RemoteAddr)
	targetHost, _, err := net.SplitHostPort(reqAddr)
	if err != nil {
		targetHost = reqAddr
	}
	targetHost = strings.TrimSpace(targetHost)
	if targetHost == "" {
		targetHost = reqAddr
	}
	if udpSessionID != 0 {
		return fmt.Sprintf("hy2-%s-%d-%d-%s",
			compactSessionPart(ctx.AuthID, "anon"),
			ctx.ConnID,
			udpSessionID,
			compactSessionPart(targetHost, "udp"))
	}
	return fmt.Sprintf("hy2-%s-%d-%s", compactSessionPart(ctx.AuthID, "anon"), ctx.ConnID, compactSessionPart(targetHost+"-"+remote, "flow"))
}

func compactSessionPart(v string, fallback string) string {
	v = strings.TrimSpace(v)
	if v == "" {
		return fallback
	}
	v = strings.NewReplacer(":", "-", ".", "-", "[", "", "]", "", "/", "-", "\\", "-", " ", "-").Replace(v)
	if len(v) > 20 {
		return v[:20]
	}
	return v
}

func contextWithBridgeTimeout() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), 10*time.Second)
}

func (c *officialUDPConn) ReadFrom(b []byte) (int, string, error) {
	n, addr, err := c.UDPConn.ReadFrom(b)
	if addr != nil {
		return n, addr.String(), err
	}
	return n, "", err
}

func (c *officialUDPConn) WriteTo(b []byte, addr string) (int, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return 0, err
	}
	return c.UDPConn.WriteTo(b, udpAddr)
}

func (c *officialBridgeUDPConn) ReadFrom(b []byte) (int, string, error) {
	c.readMu.Lock()
	defer c.readMu.Unlock()

	addr, data, err := readOfficialBridgeDatagram(c.conn)
	if err != nil {
		if c.stats != nil {
			c.stats.finish("udp_read_" + err.Error())
		}
		c.logger.Infof("udp.read.close conn=%d target=%s err=%v", c.id, c.target, err)
		return 0, "", err
	}
	if len(data) > len(b) {
		c.logger.Warnf("udp.read.truncate conn=%d target=%s addr=%s len=%d cap=%d", c.id, c.target, addr, len(data), len(b))
		data = data[:len(b)]
	}
	n := copy(b, data)
	if c.stats != nil {
		c.stats.noteFirstByte(n)
	}
	c.logger.Debugf("udp.read.ok conn=%d target=%s addr=%s bytes=%d", c.id, c.target, addr, n)
	return n, addr, nil
}

func (c *officialBridgeUDPConn) WriteTo(b []byte, addr string) (int, error) {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	if err := writeOfficialBridgeDatagram(c.conn, addr, b); err != nil {
		if c.stats != nil {
			c.stats.finish("udp_write_" + err.Error())
		}
		c.logger.Warnf("udp.write.fail conn=%d target=%s addr=%s bytes=%d err=%v", c.id, c.target, addr, len(b), err)
		return 0, err
	}
	if c.stats != nil {
		c.stats.noteFirstPacket(len(b))
	}
	c.logger.Debugf("udp.write.ok conn=%d target=%s addr=%s bytes=%d", c.id, c.target, addr, len(b))
	return len(b), nil
}

func (c *officialBridgeUDPConn) Close() error {
	if c.stats != nil {
		c.stats.finish("udp_close_called")
	}
	c.logger.Infof("udp.close conn=%d target=%s", c.id, c.target)
	return c.conn.Close()
}

func (c *officialBridgeConn) Read(p []byte) (int, error) {
	for {
		n, err := c.Conn.Read(p)
		if n > 0 {
			if c.stats != nil {
				c.stats.noteFirstByte(n)
				c.stats.noteChunk("bridge_to_front", n)
			}
		}
		if err != nil {
			// 同 copyFrontToBridge：不再对超时 continue 自旋。本侧不设 read deadline，
			// 超时即 QUIC 连接级终止错误，传播让连接关闭，避免死连接忙等打满 CPU。
			if c.stats != nil {
				c.stats.finish("tcp_read_" + err.Error())
			}
			c.logger.Infof("tcp.read.close conn=%d target=%s err=%v", c.id, c.target, err)
		}
		return n, err
	}
}

func (c *officialBridgeConn) Write(p []byte) (int, error) {
	n, err := c.writeChunked(p)
	if err != nil {
		if c.stats != nil {
			c.stats.finish("tcp_write_" + err.Error())
		}
		c.logger.Warnf("tcp.write.fail conn=%d target=%s want=%d sent=%d err=%v", c.id, c.target, len(p), n, err)
		return n, err
	}
	if n > 0 {
		if c.stats != nil {
			c.stats.noteFirstPacket(n)
			c.stats.noteChunk("front_to_bridge", n)
		}
	}
	return n, nil
}

func (c *officialBridgeConn) Close() error {
	var closeErr error
	c.closeOnce.Do(func() {
		if c.stats != nil {
			c.stats.finish("tcp_close_called")
		}
		c.logger.Infof("tcp.close.full conn=%d target=%s", c.id, c.target)
		closeErr = c.Conn.Close()
	})
	return closeErr
}

func (c *officialBridgeConn) CloseWrite() error {
	type closeWriter interface {
		CloseWrite() error
	}
	if cw, ok := c.Conn.(closeWriter); ok {
		err := cw.CloseWrite()
		c.logger.Infof("tcp.close.write conn=%d target=%s err=%v", c.id, c.target, err)
		return err
	}
	c.logger.Infof("tcp.close.write conn=%d target=%s err=<nil>", c.id, c.target)
	return nil
}

func (c *officialBridgeConn) ReadFrom(r io.Reader) (int64, error) {
	n, err := c.copyFrontToBridge(r)
	if n > 0 {
		c.logger.Infof("tcp.copy.buffered conn=%d target=%s direction=front_to_bridge bytes=%d", c.id, c.target, n)
	}
	if err == io.EOF {
		c.logger.Infof("tcp.copy.front_to_bridge.eof conn=%d target=%s bytes=%d", c.id, c.target, n)
		_ = c.CloseWrite()
		return n, nil
	}
	if err != nil && c.stats != nil {
		c.stats.finish("front_to_bridge_" + err.Error())
	}
	return n, err
}

func (c *officialBridgeConn) WriteTo(w io.Writer) (int64, error) {
	writer := &flowStatsWriter{
		Writer: w,
		onWrite: func(n int) {
			if c.stats != nil {
				c.stats.noteFirstByte(n)
				c.stats.noteChunk("bridge_to_front", n)
			}
		},
	}
	n, err := copyWithBridgePool(writer, c.Conn, func(n int64) {
		if n > 0 {
			c.logger.Infof("tcp.copy.buffered conn=%d target=%s direction=bridge_to_front bytes=%d", c.id, c.target, n)
		}
	})
	if err != nil && c.stats != nil {
		c.stats.finish("bridge_to_front_" + err.Error())
	}
	return n, err
}

func (l officialBridgeEventLogger) Connect(addr net.Addr, id string, tx uint64) {
	l.logger.Infof("event.connect remote=%s id=%s tx=%d", addrString(addr), id, tx)
}

func (l officialBridgeEventLogger) Disconnect(addr net.Addr, id string, err error) {
	l.logger.Infof("event.disconnect remote=%s id=%s err=%v", addrString(addr), id, err)
}

func (l officialBridgeEventLogger) TCPRequest(addr net.Addr, id, reqAddr string) {
	l.logger.Infof("event.tcp.request remote=%s id=%s target=%s", addrString(addr), id, reqAddr)
	if isTikTokTraceTarget(reqAddr) {
		l.logger.Infof("tiktok.trace.front.request remote=%s id=%s target=%s", addrString(addr), id, reqAddr)
	}
}

func (l officialBridgeEventLogger) TCPError(addr net.Addr, id, reqAddr string, err error) {
	l.logger.Infof("event.tcp.close remote=%s id=%s target=%s err=%v", addrString(addr), id, reqAddr, err)
}

func (l officialBridgeEventLogger) UDPRequest(addr net.Addr, id string, sessionID uint32, reqAddr string) {
	l.logger.Infof("event.udp.request remote=%s id=%s session=%d target=%s", addrString(addr), id, sessionID, reqAddr)
}

func (l officialBridgeEventLogger) UDPError(addr net.Addr, id string, sessionID uint32, err error) {
	l.logger.Infof("event.udp.close remote=%s id=%s session=%d err=%v", addrString(addr), id, sessionID, err)
}

func writeOfficialBridgeRequest(conn net.Conn, target string) error {
	host, portText, err := net.SplitHostPort(target)
	if err != nil {
		return err
	}
	portAddr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort("127.0.0.1", portText))
	if err != nil || portAddr == nil {
		return fmt.Errorf("resolve target port: %w", err)
	}
	if len(host) == 0 || len(host) > 65535 {
		return errors.New("invalid target host length")
	}
	head := make([]byte, 2+len(host)+2)
	binary.BigEndian.PutUint16(head[:2], uint16(len(host)))
	copy(head[2:], host)
	binary.BigEndian.PutUint16(head[2+len(host):], uint16(portAddr.Port))
	_, err = conn.Write(head)
	return err
}

func writeOfficialBridgeExtendedRequest(conn net.Conn, proto byte, target string) error {
	host, portText, err := net.SplitHostPort(target)
	if err != nil {
		return err
	}
	portAddr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort("127.0.0.1", portText))
	if err != nil || portAddr == nil {
		return fmt.Errorf("resolve target port: %w", err)
	}
	if len(host) == 0 || len(host) > 65535 {
		return errors.New("invalid target host length")
	}
	head := make([]byte, 2+1+2+len(host)+2)
	binary.BigEndian.PutUint16(head[:2], 0)
	head[2] = proto
	binary.BigEndian.PutUint16(head[3:5], uint16(len(host)))
	copy(head[5:], host)
	binary.BigEndian.PutUint16(head[5+len(host):], uint16(portAddr.Port))
	_, err = conn.Write(head)
	return err
}

func readOfficialBridgeStatus(conn net.Conn) error {
	head := make([]byte, 3)
	if _, err := io.ReadFull(conn, head); err != nil {
		return err
	}
	msgLen := int(binary.BigEndian.Uint16(head[1:3]))
	msg := make([]byte, msgLen)
	if msgLen > 0 {
		if _, err := io.ReadFull(conn, msg); err != nil {
			return err
		}
	}
	if head[0] == 0 {
		if msgLen == 0 {
			return errors.New("xgw bridge rejected")
		}
		return errors.New(string(msg))
	}
	return nil
}

func writeOfficialBridgeDatagram(conn net.Conn, addr string, data []byte) error {
	if len(addr) == 0 || len(addr) > 65535 {
		return errors.New("invalid udp bridge addr length")
	}
	if len(data) > 65535 {
		return errors.New("udp bridge datagram too large")
	}
	frame := make([]byte, 2+len(addr)+2+len(data))
	binary.BigEndian.PutUint16(frame[:2], uint16(len(addr)))
	copy(frame[2:], addr)
	binary.BigEndian.PutUint16(frame[2+len(addr):], uint16(len(data)))
	copy(frame[4+len(addr):], data)
	_, err := conn.Write(frame)
	return err
}

func readOfficialBridgeDatagram(conn net.Conn) (string, []byte, error) {
	head := make([]byte, 2)
	if _, err := io.ReadFull(conn, head); err != nil {
		return "", nil, err
	}
	addrLen := int(binary.BigEndian.Uint16(head))
	if addrLen == 0 || addrLen > 65535 {
		return "", nil, errors.New("invalid udp bridge addr length")
	}
	addr := make([]byte, addrLen)
	if _, err := io.ReadFull(conn, addr); err != nil {
		return "", nil, err
	}
	lenBuf := make([]byte, 2)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return "", nil, err
	}
	dataLen := int(binary.BigEndian.Uint16(lenBuf))
	data := make([]byte, dataLen)
	if dataLen > 0 {
		if _, err := io.ReadFull(conn, data); err != nil {
			return "", nil, err
		}
	}
	return string(addr), data, nil
}

func copyWithBridgePool(dst io.Writer, src io.Reader, done func(int64)) (int64, error) {
	bufp := bridgeCopyBufPool.Get().(*[]byte)
	defer bridgeCopyBufPool.Put(bufp)
	n, err := io.CopyBuffer(dst, src, *bufp)
	done(n)
	return n, err
}

func (c *officialBridgeConn) bridgeChunkSize() int {
	// The bridge transport (shared-ring) already splits payloads into
	// max-sized ring frames internally, so a small 4096 cut here only
	// multiplies frame/syscall count and adds latency to downstream
	// responses (e.g. TikTok bootstrap bodies). Hand it a full 64KiB buffer.
	return 64 * 1024
}

func (c *officialBridgeConn) writeChunked(p []byte) (int, error) {
	total := 0
	chunkSize := c.bridgeChunkSize()
	for total < len(p) {
		end := total + chunkSize
		if end > len(p) {
			end = len(p)
		}
		n, err := c.Conn.Write(p[total:end])
		total += n
		if err != nil {
			return total, err
		}
		if n <= 0 {
			return total, io.ErrShortWrite
		}
	}
	return total, nil
}

func (c *officialBridgeConn) copyFrontToBridge(r io.Reader) (int64, error) {
	buf := make([]byte, c.bridgeChunkSize())
	var total int64
	for {
		nr, er := r.Read(buf)
		if nr > 0 {
			if c.stats != nil {
				c.stats.noteFirstPacket(nr)
				c.stats.noteChunk("front_to_bridge", nr)
			}
			nw, ew := c.writeChunked(buf[:nr])
			total += int64(nw)
			if ew != nil {
				return total, ew
			}
			if nw != nr {
				return total, io.ErrShortWrite
			}
		}
		if er != nil {
			if er == io.EOF {
				return total, io.EOF
			}
			// 不再对超时 continue 重试：本侧从不设 read deadline，任何超时只能是
			// QUIC 连接级 idle/终止错误（终态）。早期在此忙等重读，会对已 idle 的死
			// 连接以 CPU 极限自旋（每次还做 errors.As 反射），perf 实证 98.6% CPU 落在
			// 本函数→isTimeoutErr→errors.As。直接传播让连接干净关闭。
			return total, er
		}
	}
}

func chooseOfficialCongestion(supported []string) string {
	for _, cc := range supported {
		switch strings.ToLower(cc) {
		case "bbr", "reno":
			return strings.ToLower(cc)
		}
	}
	return "bbr"
}

func effectiveDuration(value, fallback time.Duration) time.Duration {
	if value > 0 {
		return value
	}
	return fallback
}

func addrString(addr net.Addr) string {
	if addr == nil {
		return ""
	}
	return addr.String()
}
