package native

import (
	"context"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/coremodel"
	"github.com/local/xgw-edge/internal/pool"
	"github.com/local/xgw-edge/internal/proxy"
	"github.com/quic-go/quic-go"
	"go.uber.org/zap"
)

const defaultDialTimeout = 10 * time.Second

type Server struct {
	cfg           config.ServerConfig
	logger        *zap.SugaredLogger
	pool          *pool.Manager
	listener      *quic.Listener
	backend       *BackendAdapter
	backendRoutes *backendRouteController

	sessionsMu sync.RWMutex
	sessions   map[string]*serverSession

	routeMu         sync.Mutex
	activeRoute     control.RouteCandidate
	activeRouteTime time.Time
}

type serverSession struct {
	ID             string
	Route          control.RouteCandidate
	SelectedCC     string
	BackendUDPAddr string
	CreatedAt      time.Time
	LastSeenAt     time.Time
	udpMu          sync.Mutex
	udpConns       map[string]net.Conn
}

func NewServer(cfg config.ServerConfig, logger *zap.SugaredLogger) (*Server, error) {
	cfg.Normalize()
	if cfg.Native.Mode == "" {
		cfg.Native.Mode = ModeXGWNative
	}
	if cfg.Native.ALPN == "" {
		cfg.Native.ALPN = DefaultALPN
	}
	if cfg.ConnectType != connectTypeBridge && cfg.ConnectType != connectTypeDirect {
		return nil, errors.New("native server supports connect_type=bridge or direct")
	}
	backend, err := NewBackendAdapter(cfg)
	if err != nil {
		return nil, err
	}
	return &Server{
		cfg:           cfg,
		logger:        logger,
		pool:          pool.NewManager(cfg.Pool),
		backend:       backend,
		backendRoutes: newBackendRouteController(cfg.BackendRouteControlPath),
		sessions:      make(map[string]*serverSession),
	}, nil
}

func defaultBudgetSnapshot() control.StreamBudgetSnapshot {
	classifier := config.DefaultServer().FlowClassifier.ToCore()
	flow := coremodel.ClassifyFlowWithRules(coremodel.FlowKindTCP, "www.google.com:443", &classifier)
	return control.StreamBudgetSnapshot{
		FlowClass:            string(flow.Class),
		Priority:             flow.Priority.String(),
		PreferredCopies:      flow.Budget.PreferredCopies,
		ReadTimeoutMillis:    int(flow.Budget.ReadTimeout / time.Millisecond),
		IdleAfterFirstByteMs: int(flow.Budget.IdleAfterFirstByte / time.Millisecond),
		ReducedFEC:           flow.Budget.PreferReducedFEC,
		FastACK:              flow.Budget.PreferFastAck,
		IndependentIO:        flow.Budget.PreferIndependentIO,
	}
}

func schedulerSnapshot() control.SchedulerSnapshot {
	return control.SchedulerSnapshot{
		Mode:                 "per-stream-budget",
		PerStreamAccounting:  true,
		DeficitRoundRobin:    true,
		StarvationProtection: true,
		SessionCCCoupled:     true,
	}
}

func (s *Server) Serve() error {
	cert, err := tls.LoadX509KeyPair(s.cfg.TLS.CertFile, s.cfg.TLS.KeyFile)
	if err != nil {
		return err
	}
	tlsConf := &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{s.cfg.Native.ALPN},
		MinVersion:   tls.VersionTLS13,
	}
	quicConf := &quic.Config{
		EnableDatagrams:                s.cfg.Native.EnableUDP,
		Allow0RTT:                      true,
		HandshakeIdleTimeout:           effectiveDuration(s.cfg.Control.IdleTimeout.Std(), 30*time.Second),
		MaxIdleTimeout:                 effectiveDuration(s.cfg.Control.IdleTimeout.Std(), 30*time.Second),
		KeepAlivePeriod:                effectiveDuration(s.cfg.Control.Keepalive.Std(), 10*time.Second),
		InitialStreamReceiveWindow:     8 << 20,
		MaxStreamReceiveWindow:         16 << 20,
		InitialConnectionReceiveWindow: 20 << 20,
		MaxConnectionReceiveWindow:     32 << 20,
		DisablePathMTUDiscovery:        false,
	}
	listener, err := quic.ListenAddr(s.cfg.Listen, tlsConf, quicConf)
	if err != nil {
		return err
	}
	s.listener = listener
	s.pool.StartHealthProbes(context.Background())
	s.logger.Infof("xgw-native server listen=%s connect_type=%s bridge_transport=%s bridge_tcp=%s bridge_unix=%s bridge_ring=%s backend_route_control_mode=%s alpn=%s tcp=%t udp=%t",
		s.cfg.Listen, s.cfg.ConnectType, s.cfg.BridgeTransport, s.cfg.BridgeTCPAddr, s.cfg.BridgeUnixPath, s.cfg.BridgeRingPath, s.cfg.BackendRouteControlMode, s.cfg.Native.ALPN, s.cfg.Native.EnableTCP, s.cfg.Native.EnableUDP)
	for {
		conn, err := listener.Accept(context.Background())
		if err != nil {
			return err
		}
		go s.handleConn(conn)
	}
}

func (s *Server) handleConn(conn *quic.Conn) {
	remote := conn.RemoteAddr().String()
	s.logger.Infof("native.conn.accept remote=%s local=%s", remote, conn.LocalAddr())
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stream, err := conn.AcceptStream(ctx)
	if err != nil {
		s.logger.Warnf("native.auth.accept.fail remote=%s err=%v", remote, err)
		_ = conn.CloseWithError(1, "auth stream required")
		return
	}
	session, err := s.handleAuthStream(stream, remote)
	if err != nil {
		s.logger.Warnf("native.auth.fail remote=%s err=%v", remote, err)
		_ = conn.CloseWithError(2, "auth failed")
		return
	}
	defer s.closeSession(session)
	s.logger.Infof("native.auth.allow remote=%s session=%s route=%s line=%s cc=%s",
		remote, session.ID, session.Route.Name, session.Route.LineID, session.SelectedCC)

	if s.cfg.Native.EnableUDP {
		go s.runDatagramLoop(ctx, conn, session)
	}
	for {
		stream, err := conn.AcceptStream(ctx)
		if err != nil {
			s.logger.Infof("native.conn.close remote=%s session=%s err=%v", remote, session.ID, err)
			return
		}
		go s.handleStream(ctx, session, stream)
	}
}

func (s *Server) handleAuthStream(stream *quic.Stream, remote string) (*serverSession, error) {
	defer stream.Close()
	var req control.AuthRequest
	if err := readJSONMessage(stream, streamKindAuth, &req); err != nil {
		return nil, err
	}
	if s.cfg.Control.RequireAuth && s.cfg.Control.Token != "" && req.Token != s.cfg.Control.Token {
		return nil, errors.New("token mismatch")
	}
	best, alternates := s.pool.Select(req)
	if best.Name == "" {
		best = control.RouteCandidate{
			Name:           "default",
			Address:        s.cfg.Listen,
			Protocol:       "xgw-native",
			BackendUDPAddr: s.cfg.BackendUDPAddr,
			Score:          100,
			Reasons:        []string{"fallback-default"},
		}
	}
	best, _ = s.stabilizeBackendRoute(best, "auth")
	selectedCC := chooseCongestion(s.cfg.Control.SupportedCongestion, req.RequestedCongestion)
	resp := control.AuthResponse{
		OK:                 true,
		SessionID:          newSessionID(best.Name, time.Now()),
		SelectedCongestion: selectedCC,
		AdvertisedRxMbps:   s.cfg.Control.AdvertisedRxMbps,
		AdvertisedTxMbps:   s.cfg.Control.AdvertisedTxMbps,
		SelectedRoute:      best,
		AlternateRoutes:    alternates,
		KeepaliveSec:       int(effectiveDuration(s.cfg.Control.Keepalive.Std(), 10*time.Second) / time.Second),
		IdleTimeoutSec:     int(effectiveDuration(s.cfg.Control.IdleTimeout.Std(), 30*time.Second) / time.Second),
		AllowMigration:     s.cfg.Control.EnableMigration,
		AllowDatagrams:     s.cfg.Native.EnableUDP,
		DefaultBudget:      defaultBudgetSnapshot(),
		Scheduler:          schedulerSnapshot(),
	}
	if err := writeJSONMessage(stream, streamKindAuth, resp); err != nil {
		return nil, err
	}
	session := &serverSession{
		ID:             resp.SessionID,
		Route:          best,
		SelectedCC:     selectedCC,
		BackendUDPAddr: backendForRoute(best, s.cfg.BackendUDPAddr),
		CreatedAt:      time.Now(),
		LastSeenAt:     time.Now(),
		udpConns:       make(map[string]net.Conn),
	}
	s.sessionsMu.Lock()
	s.sessions[session.ID] = session
	s.sessionsMu.Unlock()
	s.pool.IncrementSessions(routePressureName(best), 1)
	if s.backendRouteControlActive() {
		if err := s.backendRoutes.SetActive(best); err != nil {
			s.logger.Warnf("native.backend_route.active.fail session=%s route=%s line=%s err=%v", session.ID, best.Name, best.LineID, err)
		} else {
			s.logger.Infof("native.backend_route.active session=%s route=%s line=%s", session.ID, best.Name, best.LineID)
		}
	} else {
		s.logger.Infof("native.backend_route.observe session=%s route=%s line=%s mode=%s", session.ID, best.Name, best.LineID, s.cfg.BackendRouteControlMode)
	}
	s.logger.Infof("native.route.select session=%s route=%s line=%s score=%.2f reasons=%v alternates=%v backend=%s",
		session.ID, best.Name, best.LineID, best.Score, best.Reasons, alternates, session.BackendUDPAddr)
	return session, nil
}

func (s *Server) closeSession(session *serverSession) {
	if session == nil {
		return
	}
	session.closeUDPConns()
	s.sessionsMu.Lock()
	if s.sessions[session.ID] == session {
		delete(s.sessions, session.ID)
		s.pool.IncrementSessions(routePressureName(session.Route), -1)
	}
	s.sessionsMu.Unlock()
}

func (s *Server) handleStream(ctx context.Context, session *serverSession, stream *quic.Stream) {
	kind, err := readStreamKind(stream)
	if err != nil {
		s.logger.Warnf("native.stream.kind.fail session=%s err=%v", session.ID, err)
		_ = stream.Close()
		return
	}
	switch kind {
	case streamKindTCP:
		s.handleTCPStream(ctx, session, stream)
	case streamKindKeepalive:
		s.handleKeepaliveStream(stream, session)
	case streamKindRoute:
		s.handleRouteStream(stream, session)
	default:
		s.logger.Warnf("native.stream.unknown session=%s kind=%d", session.ID, kind)
		_ = stream.Close()
	}
}

func (s *Server) handleTCPStream(ctx context.Context, session *serverSession, stream *quic.Stream) {
	_ = ctx
	if !s.cfg.Native.EnableTCP {
		_ = proxy.WriteTCPResponse(stream, false, "tcp disabled")
		_ = stream.Close()
		return
	}
	target, err := proxy.ReadTCPRequestAny(stream)
	if err != nil {
		s.logger.Warnf("native.tcp.request.fail session=%s err=%v", session.ID, err)
		_ = stream.Close()
		return
	}
	classifier := s.cfg.FlowClassifier.ToCore()
	flow := coremodel.ClassifyFlowWithRules(coremodel.FlowKindTCP, target, &classifier)
	s.logger.Infof("native.tcp.open.begin session=%s target=%s class=%s priority=%s connect_type=%s", session.ID, target, flow.Class, flow.Priority.String(), s.cfg.ConnectType)
	upstream, err := s.dialOutboundTCP(ctx, session, target)
	if err != nil {
		s.logger.Warnf("native.tcp.open.fail session=%s target=%s err=%v", session.ID, target, err)
		_ = proxy.WriteTCPResponse(stream, false, err.Error())
		_ = stream.Close()
		return
	}
	defer upstream.Close()
	if err := proxy.WriteTCPResponse(stream, true, "connected"); err != nil {
		_ = stream.Close()
		return
	}
	s.logger.Infof("native.tcp.open.ok session=%s target=%s", session.ID, target)
	proxyNativeTwoWay(stream, upstream)
	s.logger.Infof("native.tcp.close session=%s target=%s", session.ID, target)
}

func (s *Server) handleKeepaliveStream(stream *quic.Stream, session *serverSession) {
	defer stream.Close()
	var ka control.Keepalive
	if err := readJSONMessage(stream, streamKindKeepalive, &ka); err != nil {
		s.logger.Warnf("native.keepalive.decode.fail session=%s err=%v", session.ID, err)
		return
	}
	s.sessionsMu.Lock()
	if live := s.sessions[session.ID]; live != nil {
		live.LastSeenAt = time.Now()
	}
	s.sessionsMu.Unlock()
	s.logger.Infof("native.keepalive session=%s rtt_ms=%d loss_ppm=%d", session.ID, ka.LatestRTTMillis, ka.LossPPM)
	_ = writeJSONMessage(stream, streamKindKeepalive, control.Keepalive{SessionID: session.ID})
}

func (s *Server) handleRouteStream(stream *quic.Stream, session *serverSession) {
	defer stream.Close()
	best, current, alternates := s.pool.SelectWithCurrent(control.AuthRequest{
		Token:               s.cfg.Control.Token,
		RequestedCongestion: []string{session.SelectedCC},
	}, routePressureName(session.Route))
	chosen := best
	reason := "periodic_reassessment"
	if chosen.Name == "" {
		chosen = session.Route
		reason = "fallback_to_current"
	} else {
		stickinessLeft := session.CreatedAt.Add(s.cfg.Control.RouteStickinessMin.Std()).After(time.Now())
		scoreDelta := chosen.Score - current.Score
		healthEscaped := current.Name != "" && current.Score < s.cfg.Control.RouteEscapeMinScore
		if current.Name != "" && !healthEscaped && (stickinessLeft || scoreDelta < s.cfg.Control.RouteSwitchDelta) {
			chosen = current
			if stickinessLeft {
				reason = "sticky_current"
			} else {
				reason = "delta_too_small"
			}
		}
		if healthEscaped {
			reason = "health_escape"
		}
	}
	update := control.RouteUpdate{
		SessionID:          session.ID,
		SelectedRoute:      chosen,
		AlternateRoutes:    alternates,
		SelectedCongestion: session.SelectedCC,
		ReconnectHint:      backendForRoute(chosen, s.cfg.BackendUDPAddr),
		Reason:             reason,
		DefaultBudget:      defaultBudgetSnapshot(),
		Scheduler:          schedulerSnapshot(),
	}
	chosen, reason = s.stabilizeBackendRoute(chosen, reason)
	update.SelectedRoute = chosen
	update.Reason = reason
	update.ReconnectHint = backendForRoute(chosen, s.cfg.BackendUDPAddr)
	if !sameRouteCandidate(session.Route, chosen) {
		if !s.backendRouteControlActive() {
			s.logger.Infof("native.backend_route.observe_update session=%s chosen=%s line=%s current=%s current_line=%s mode=%s reason=%s",
				session.ID, chosen.Name, chosen.LineID, session.Route.Name, session.Route.LineID, s.cfg.BackendRouteControlMode, reason)
			s.logger.Infof("native.route.update session=%s chosen=%s line=%s score=%.2f reason=%s reasons=%v hint=%s current=%s current_score=%.2f",
				session.ID, chosen.Name, chosen.LineID, chosen.Score, reason, chosen.Reasons, update.ReconnectHint, current.Name, current.Score)
			_ = writeJSONMessage(stream, streamKindRoute, update)
			return
		}
		if err := s.backendRoutes.Prewarm(chosen); err != nil {
			s.logger.Warnf("native.backend_route.prewarm.fail session=%s route=%s line=%s err=%v", session.ID, chosen.Name, chosen.LineID, err)
		} else {
			s.logger.Infof("native.backend_route.prewarm session=%s route=%s line=%s", session.ID, chosen.Name, chosen.LineID)
		}
		if err := s.backendRoutes.SetActive(chosen); err != nil {
			s.logger.Warnf("native.backend_route.active.fail session=%s route=%s line=%s err=%v", session.ID, chosen.Name, chosen.LineID, err)
		} else {
			oldRoute := session.Route
			session.Route = chosen
			session.BackendUDPAddr = backendForRoute(chosen, s.cfg.BackendUDPAddr)
			s.logger.Infof("native.backend_route.active.request session=%s route=%s line=%s old_route=%s old_line=%s drain_owner=c-backend",
				session.ID, chosen.Name, chosen.LineID, oldRoute.Name, oldRoute.LineID)
		}
	}
	s.logger.Infof("native.route.update session=%s chosen=%s line=%s score=%.2f reason=%s reasons=%v hint=%s current=%s current_score=%.2f",
		session.ID, chosen.Name, chosen.LineID, chosen.Score, reason, chosen.Reasons, update.ReconnectHint, current.Name, current.Score)
	_ = writeJSONMessage(stream, streamKindRoute, update)
}

func (s *Server) stabilizeBackendRoute(chosen control.RouteCandidate, reason string) (control.RouteCandidate, string) {
	if chosen.Name == "" && chosen.LineID == "" {
		return chosen, reason
	}
	s.routeMu.Lock()
	defer s.routeMu.Unlock()
	now := time.Now()
	if s.activeRoute.Name == "" && s.activeRoute.LineID == "" {
		s.activeRoute = chosen
		s.activeRouteTime = now
		return chosen, reason
	}
	if sameRouteCandidate(s.activeRoute, chosen) {
		return chosen, reason
	}
	minStickiness := s.cfg.Control.RouteStickinessMin.Std()
	if minStickiness <= 0 {
		minStickiness = 5 * time.Second
	}
	if reason != "health_escape" && now.Sub(s.activeRouteTime) < minStickiness {
		return s.activeRoute, "backend_sticky"
	}
	s.activeRoute = chosen
	s.activeRouteTime = now
	return chosen, reason
}

func (s *Server) backendRouteControlActive() bool {
	switch strings.ToLower(strings.TrimSpace(s.cfg.BackendRouteControlMode)) {
	case "active", "write", "enabled", "on":
		return true
	default:
		return false
	}
}

func (s *Server) runDatagramLoop(ctx context.Context, conn *quic.Conn, session *serverSession) {
	for {
		msg, err := conn.ReceiveDatagram(ctx)
		if err != nil {
			s.logger.Infof("native.udp.close session=%s err=%v", session.ID, err)
			session.closeUDPConns()
			return
		}
		dg, err := decodeDatagram(msg)
		if err != nil {
			s.logger.Warnf("native.udp.decode.fail session=%s bytes=%d err=%v", session.ID, len(msg), err)
			continue
		}
		s.logger.Infof("native.udp.recv session=%s target=%s bytes=%d", session.ID, dg.Target, len(dg.Payload))
		if err := s.forwardDatagram(ctx, conn, session, dg); err != nil {
			s.logger.Warnf("native.udp.forward.fail session=%s target=%s err=%v", session.ID, dg.Target, err)
		}
	}
}

func (s *Server) dialOutboundTCP(ctx context.Context, session *serverSession, target string) (net.Conn, error) {
	if s.backend == nil {
		return nil, errors.New("backend adapter is not initialized")
	}
	semantic := coremodel.DefaultSessionSemantic("xgw-native")
	if session != nil {
		semantic.SessionID = session.ID
		semantic.RouteName = session.Route.Name
		semantic.LineID = session.Route.LineID
	}
	classifier := s.cfg.FlowClassifier.ToCore()
	req := coremodel.OpenRequest{
		Flow: coremodel.ClassifyFlowWithRules(coremodel.FlowKindTCP, target, &classifier),
		Session: semantic,
	}
	conn, _, err := s.backend.OpenTCP(ctx, req)
	return conn, err
}

func (s *Server) forwardDatagram(ctx context.Context, conn *quic.Conn, session *serverSession, dg Datagram) error {
	if s.cfg.ConnectType != connectTypeBridge {
		return s.forwardDatagramDirect(conn, session, dg)
	}
	bridgeConn, err := session.getUDPBridgeConn(dg.Target, func() (net.Conn, error) {
		if s.backend == nil {
			return nil, errors.New("backend adapter is not initialized")
		}
		semantic := coremodel.DefaultSessionSemantic("xgw-native")
		semantic.SessionID = session.ID
		semantic.RouteName = session.Route.Name
		semantic.LineID = session.Route.LineID
		classifier := s.cfg.FlowClassifier.ToCore()
		req := coremodel.OpenRequest{
			Flow:    coremodel.ClassifyFlowWithRules(coremodel.FlowKindUDP, dg.Target, &classifier),
			Session: semantic,
		}
		conn, _, err := s.backend.OpenUDP(ctx, req)
		return conn, err
	}, func(target string, c net.Conn) {
		go s.pumpBridgeUDP(ctx, conn, session, target, c)
	})
	if err != nil {
		return err
	}
	return writeBridgeDatagram(bridgeConn, dg.Target, dg.Payload)
}

func (s *Server) forwardDatagramDirect(conn *quic.Conn, session *serverSession, dg Datagram) error {
	udpAddr, err := net.ResolveUDPAddr("udp", dg.Target)
	if err != nil {
		return err
	}
	udpConn, err := net.DialUDP("udp", nil, udpAddr)
	if err != nil {
		return err
	}
	defer udpConn.Close()
	if _, err := udpConn.Write(dg.Payload); err != nil {
		return err
	}
	_ = udpConn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 64<<10)
	n, err := udpConn.Read(buf)
	if err != nil {
		return err
	}
	resp, err := encodeDatagram(dg.Target, buf[:n])
	if err != nil {
		return err
	}
	s.logger.Infof("native.udp.direct.reply session=%s target=%s bytes=%d", session.ID, dg.Target, n)
	return conn.SendDatagram(resp)
}

func (s *Server) pumpBridgeUDP(ctx context.Context, conn *quic.Conn, session *serverSession, target string, bridgeConn net.Conn) {
	for {
		addr, data, err := readBridgeDatagram(bridgeConn)
		if err != nil {
			session.removeUDPBridgeConn(target, bridgeConn)
			s.logger.Infof("native.udp.bridge.close session=%s target=%s err=%v", session.ID, target, err)
			return
		}
		resp, err := encodeDatagram(addr, data)
		if err != nil {
			s.logger.Warnf("native.udp.encode.fail session=%s target=%s addr=%s err=%v", session.ID, target, addr, err)
			continue
		}
		select {
		case <-ctx.Done():
			return
		default:
		}
		if err := conn.SendDatagram(resp); err != nil {
			session.removeUDPBridgeConn(target, bridgeConn)
			s.logger.Warnf("native.udp.bridge.reply.fail session=%s target=%s addr=%s err=%v", session.ID, target, addr, err)
			return
		}
		s.logger.Infof("native.udp.bridge.reply session=%s target=%s addr=%s bytes=%d", session.ID, target, addr, len(data))
	}
}

func (s *serverSession) getUDPBridgeConn(target string, dial func() (net.Conn, error), onCreate func(string, net.Conn)) (net.Conn, error) {
	s.udpMu.Lock()
	defer s.udpMu.Unlock()
	if s.udpConns == nil {
		s.udpConns = make(map[string]net.Conn)
	}
	if conn := s.udpConns[target]; conn != nil {
		return conn, nil
	}
	conn, err := dial()
	if err != nil {
		return nil, err
	}
	s.udpConns[target] = conn
	onCreate(target, conn)
	return conn, nil
}

func (s *serverSession) removeUDPBridgeConn(target string, conn net.Conn) {
	s.udpMu.Lock()
	defer s.udpMu.Unlock()
	if s.udpConns[target] == conn {
		delete(s.udpConns, target)
	}
	_ = conn.Close()
}

func (s *serverSession) closeUDPConns() {
	s.udpMu.Lock()
	defer s.udpMu.Unlock()
	for target, conn := range s.udpConns {
		_ = conn.Close()
		delete(s.udpConns, target)
	}
}

func proxyNativeTwoWay(stream *quic.Stream, upstream net.Conn) {
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		_, _ = copyWithBridgePool(upstream, stream)
		_ = upstream.SetDeadline(time.Now())
	}()
	go func() {
		defer wg.Done()
		_, _ = copyWithBridgePool(stream, upstream)
		_ = stream.SetDeadline(time.Now())
	}()
	wg.Wait()
	stream.CancelRead(0)
	_ = stream.Close()
}

func chooseCongestion(supported []string, requested []string) string {
	if len(requested) == 0 {
		if len(supported) == 0 {
			return "bbr"
		}
		return supported[0]
	}
	for _, want := range requested {
		for _, have := range supported {
			if want == have {
				return want
			}
		}
	}
	if len(supported) > 0 {
		return supported[0]
	}
	return "bbr"
}

func backendForRoute(route control.RouteCandidate, fallback string) string {
	if route.BackendUDPAddr != "" {
		return route.BackendUDPAddr
	}
	return fallback
}

func routePressureName(route control.RouteCandidate) string {
	if route.LineID != "" {
		return route.LineID
	}
	return route.Name
}

func sameRouteCandidate(a control.RouteCandidate, b control.RouteCandidate) bool {
	if a.LineID != "" || b.LineID != "" {
		return a.LineID != "" && a.LineID == b.LineID
	}
	return a.Name == b.Name && a.Address == b.Address
}

func newSessionID(seed string, now time.Time) string {
	if seed == "" {
		seed = "native"
	}
	return seed + "-" + now.UTC().Format("20060102T150405.000000000")
}

func effectiveDuration(value, fallback time.Duration) time.Duration {
	if value > 0 {
		return value
	}
	return fallback
}

var _ io.Reader = (*quic.Stream)(nil)
