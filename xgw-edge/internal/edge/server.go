//go:build ignore

package edge

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/compat"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/masq"
	"github.com/local/xgw-edge/internal/pool"
	"github.com/local/xgw-edge/internal/proxy"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/quic-go/logging"
	"go.uber.org/zap"
)

type Server struct {
	cfg         config.ServerConfig
	logger      *zap.SugaredLogger
	pool        *pool.Manager
	masq        *masq.Handler
	httpServer  *http3.Server
	sessionsMu  sync.Mutex
	sessions    map[string]*Session
	authMu      sync.RWMutex
	authByRemote map[string]*authState
	authByTrace map[quic.ConnectionTracingID]*authState
}

type Session struct {
	ID              string
	Route           control.RouteCandidate
	StartedAt       time.Time
	RouteChosenAt   time.Time
	RouteScore      float64
	SelectedCC      string
	BackendUDPAddr  string
	LastKeepaliveAt time.Time
}

type authState struct {
	authenticated bool
	authID        string
	sessionID     string
}

func NewServer(cfg config.ServerConfig, logger *zap.SugaredLogger) (*Server, error) {
	masqHandler, err := masq.New(cfg.Masquerade)
	if err != nil {
		return nil, err
	}
	s := &Server{
		cfg:         cfg,
		logger:      logger,
		pool:        pool.NewManager(cfg.Pool),
		masq:        masqHandler,
		sessions:    make(map[string]*Session),
		authByRemote: make(map[string]*authState),
		authByTrace: make(map[quic.ConnectionTracingID]*authState),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleHTTP)
	if cfg.MetricsListen == "" {
		mux.Handle("/metrics", promhttp.Handler())
	}
	tlsConf := &tls.Config{
		NextProtos: cfg.TLS.ALPN,
		MinVersion: tls.VersionTLS13,
	}
	s.httpServer = &http3.Server{
		Addr:            cfg.Listen,
		Handler:         mux,
		TLSConfig:       tlsConf,
		QUICConfig:      s.buildQUICConfig(),
		EnableDatagrams: true,
		MaxHeaderBytes:  64 << 10,
		StreamHijacker:  s.handleStreamHijack,
		ConnContext:     s.handleConnContext,
	}
	return s, nil
}

func (s *Server) buildQUICConfig() *quic.Config {
	return &quic.Config{
		EnableDatagrams:                true,
		Allow0RTT:                      true,
		HandshakeIdleTimeout:           s.cfg.Control.IdleTimeout.Std(),
		MaxIdleTimeout:                 s.cfg.Control.IdleTimeout.Std(),
		KeepAlivePeriod:                s.cfg.Control.Keepalive.Std(),
		InitialStreamReceiveWindow:     8 << 20,
		MaxStreamReceiveWindow:         16 << 20,
		InitialConnectionReceiveWindow: 20 << 20,
		MaxConnectionReceiveWindow:     32 << 20,
		DisablePathMTUDiscovery:        false,
		Tracer:                         s.newConnTracer,
	}
}

func (s *Server) ListenAndServe() error {
	s.logger.Infof("xgw-edge server listen=%s backend=%s compatibility=%s auth_host=%s auth_path=%s alpn=%v",
		s.cfg.Listen,
		s.cfg.BackendUDPAddr,
		s.cfg.Compatibility.Mode,
		s.cfg.Control.AuthHost,
		s.cfg.Control.AuthPath,
		s.cfg.TLS.ALPN,
	)
	return s.httpServer.ListenAndServeTLS(s.cfg.TLS.CertFile, s.cfg.TLS.KeyFile)
}

func (s *Server) Serve() error {
	return s.ListenAndServe()
}

func (s *Server) handleConnContext(ctx context.Context, c *quic.Conn) context.Context {
	s.logger.Infof("quic conn context remote=%s local=%s", c.RemoteAddr(), c.LocalAddr())
	return ctx
}

func (s *Server) newConnTracer(ctx context.Context, p logging.Perspective, connID quic.ConnectionID) *logging.ConnectionTracer {
	var remote string
	tracingID, _ := ctx.Value(quic.ConnectionTracingKey).(quic.ConnectionTracingID)
	return &logging.ConnectionTracer{
		StartedConnection: func(local, remoteAddr net.Addr, srcConnID, destConnID logging.ConnectionID) {
			remote = remoteAddr.String()
			state := &authState{}
			s.authMu.Lock()
			s.authByRemote[remote] = state
			if tracingID != 0 {
				s.authByTrace[tracingID] = state
			}
			s.authMu.Unlock()
			s.logger.Infof("quic started remote=%s local=%s perspective=%v conn_id=%s tracing_id=%d", remoteAddr, local, p, connID, tracingID)
		},
		NegotiatedVersion: func(chosen logging.Version, clientVersions, serverVersions []logging.Version) {
			s.logger.Infof("quic version negotiated remote=%s version=%v", remote, chosen)
		},
		SentTransportParameters: func(parameters *logging.TransportParameters) {
			s.logger.Infof("quic sent transport params remote=%s max_idle=%s datagram=%t", remote, parameters.MaxIdleTimeout, parameters.MaxDatagramFrameSize > 0)
		},
		ReceivedTransportParameters: func(parameters *logging.TransportParameters) {
			s.logger.Infof("quic recv transport params remote=%s max_idle=%s datagram=%t", remote, parameters.MaxIdleTimeout, parameters.MaxDatagramFrameSize > 0)
		},
		ReceivedLongHeaderPacket: func(hdr *logging.ExtendedHeader, size logging.ByteCount, ecn logging.ECN, frames []logging.Frame) {
			s.logger.Infof("quic recv long packet remote=%s type=%s size=%d frames=%d", remote, hdr.Type, size, len(frames))
		},
		ReceivedShortHeaderPacket: func(hdr *logging.ShortHeader, size logging.ByteCount, ecn logging.ECN, frames []logging.Frame) {
			s.logger.Infof("quic recv short packet remote=%s key_phase=%d size=%d frames=%d", remote, hdr.KeyPhase, size, len(frames))
		},
		DroppedPacket: func(packetType logging.PacketType, pn logging.PacketNumber, size logging.ByteCount, reason logging.PacketDropReason) {
			s.logger.Warnf("quic dropped packet remote=%s type=%s pn=%d size=%d reason=%s", remote, packetType, pn, size, reason)
		},
		ChoseALPN: func(protocol string) {
			s.logger.Infof("quic alpn chosen remote=%s alpn=%s", remote, protocol)
		},
		UpdatedKeyFromTLS: func(encLevel logging.EncryptionLevel, p logging.Perspective) {
			s.logger.Infof("quic tls keys updated remote=%s enc_level=%v perspective=%v", remote, encLevel, p)
		},
		Debug: func(name, msg string) {
			s.logger.Infof("quic debug remote=%s name=%s msg=%s", remote, name, msg)
		},
		ClosedConnection: func(err error) {
			s.authMu.Lock()
			if remote != "" {
				delete(s.authByRemote, remote)
			}
			if tracingID != 0 {
				delete(s.authByTrace, tracingID)
			}
			s.authMu.Unlock()
			if err != nil {
				s.logger.Warnf("quic closed remote=%s err=%v", remote, err)
			} else {
				s.logger.Infof("quic closed remote=%s", remote)
			}
		},
	}
}

func (s *Server) handleHTTP(w http.ResponseWriter, r *http.Request) {
	s.logger.Infof("http3 request remote=%s method=%s host=%s path=%s ua=%q",
		r.RemoteAddr, r.Method, r.Host, r.URL.Path, r.UserAgent())

	if s.cfg.Compatibility.EnableHysteriaAuth && r.Header.Get(compat.RequestHeaderAuth) != "" {
		s.logger.Infof("http3 compat auth candidate remote=%s method=%s host=%s path=%s", r.RemoteAddr, r.Method, r.Host, r.URL.Path)
		s.handleAuth(w, r)
		return
	}
	if r.URL.Path == "/metrics" && s.cfg.MetricsListen == "" {
		promhttp.Handler().ServeHTTP(w, r)
		return
	}
	if r.Method == http.MethodGet && r.Host == s.cfg.Control.AuthHost && r.URL.Path == s.cfg.Control.AuthPath+"/route" {
		s.handleRoute(w, r)
		return
	}
	if r.Method == http.MethodPost && r.Host == s.cfg.Control.AuthHost && r.URL.Path == s.cfg.Control.AuthPath+"/keepalive" {
		s.handleKeepalive(w, r)
		return
	}
	if r.Method == http.MethodPost && r.Host == s.cfg.Control.AuthHost && r.URL.Path == s.cfg.Control.AuthPath {
		s.handleAuth(w, r)
		return
	}

	s.logger.Infof("http3 fallback remote=%s method=%s host=%s path=%s", r.RemoteAddr, r.Method, r.Host, r.URL.Path)
	s.masq.ServeFallback(w, r)
}

func (s *Server) handleAuth(w http.ResponseWriter, r *http.Request) {
	state := s.lookupAuthStateByRemote(r.RemoteAddr)
	var req control.AuthRequest
	if s.cfg.Compatibility.EnableHysteriaAuth {
		hyReq := compat.AuthRequestFromHeader(r.Header)
		req.Token = hyReq.Auth
		req.AdvertisedRxMbps = hyReq.Rx / 125000
		s.logger.Infof("auth request compat=hysteria remote=%s host=%s path=%s token_set=%t token=%q rx_bps=%d headers=%v",
			r.RemoteAddr, r.Host, r.URL.Path, req.Token != "", req.Token, hyReq.Rx, r.Header)
	} else {
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			s.logger.Warnf("invalid auth payload remote=%s err=%v", r.RemoteAddr, err)
			s.masq.ServeUnauthorized(w, r)
			return
		}
		s.logger.Infof("auth request compat=native remote=%s host=%s path=%s token_set=%t requested_node=%s",
			r.RemoteAddr, r.Host, r.URL.Path, req.Token != "", req.RequestedNode)
	}

	if state != nil && state.authenticated {
		s.logger.Infof("auth request on authenticated conn remote=%s session=%s", r.RemoteAddr, state.sessionID)
		w.Header().Set("Content-Type", "application/json")
		compat.AuthResponseToHeader(w.Header(), compat.AuthResponse{
			UDPEnabled: true,
			Rx:         s.cfg.Control.AdvertisedRxMbps * 125000,
			RxAuto:     s.cfg.Control.AdvertisedRxMbps == 0,
		})
		w.WriteHeader(compat.StatusAuthOK)
		return
	}
	if s.cfg.Control.RequireAuth && s.cfg.Control.Token != "" && req.Token != s.cfg.Control.Token {
		s.logger.Warnf("auth token mismatch remote=%s host=%s", r.RemoteAddr, r.Host)
		s.masq.ServeUnauthorized(w, r)
		return
	}

	best, alternates := s.pool.Select(req)
	if req.RequestedNode != "" {
		for _, alt := range append([]control.RouteCandidate{best}, alternates...) {
			if alt.Name == req.RequestedNode {
				best = alt
				break
			}
		}
	}
	if best.Name == "" {
		http.Error(w, "no route available", http.StatusServiceUnavailable)
		return
	}
	selectedCC := chooseCongestion(s.cfg.Control.SupportedCongestion, req.RequestedCongestion)
	resp := control.AuthResponse{
		OK:                 true,
		SessionID:          newSessionID(best.Name, time.Now()),
		SelectedCongestion: selectedCC,
		AdvertisedRxMbps:   s.cfg.Control.AdvertisedRxMbps,
		AdvertisedTxMbps:   s.cfg.Control.AdvertisedTxMbps,
		SelectedRoute:      best,
		AlternateRoutes:    alternates,
		KeepaliveSec:       int(s.cfg.Control.Keepalive.Std() / time.Second),
		IdleTimeoutSec:     int(s.cfg.Control.IdleTimeout.Std() / time.Second),
		AllowMigration:     s.cfg.Control.EnableMigration,
		AllowDatagrams:     true,
	}

	s.sessionsMu.Lock()
	s.sessions[resp.SessionID] = &Session{
		ID:              resp.SessionID,
		Route:           best,
		StartedAt:       time.Now(),
		RouteChosenAt:   time.Now(),
		RouteScore:      best.Score,
		SelectedCC:      selectedCC,
		BackendUDPAddr:  backendForRoute(best, s.cfg.BackendUDPAddr),
		LastKeepaliveAt: time.Now(),
	}
	s.sessionsMu.Unlock()

	if state != nil {
		state.authenticated = true
		state.authID = req.Token
		state.sessionID = resp.SessionID
	}
	s.logger.Infof("auth ok remote=%s session=%s route=%s cc=%s udp=%t",
		r.RemoteAddr, resp.SessionID, resp.SelectedRoute.Name, resp.SelectedCongestion, resp.AllowDatagrams)

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("X-Edge-Authenticated", "true")
	if s.cfg.Compatibility.EnableHysteriaAuth {
		compat.AuthResponseToHeader(w.Header(), compat.AuthResponse{
			UDPEnabled: resp.AllowDatagrams,
			Rx:         resp.AdvertisedRxMbps * 125000,
			RxAuto:     resp.AdvertisedRxMbps == 0,
		})
		s.logger.Infof("auth response compat=hysteria remote=%s status=%d udp=%t rx_mbps=%d",
			r.RemoteAddr, compat.StatusAuthOK, resp.AllowDatagrams, resp.AdvertisedRxMbps)
		w.WriteHeader(compat.StatusAuthOK)
	} else {
		s.logger.Infof("auth response compat=native remote=%s status=%d", r.RemoteAddr, http.StatusOK)
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(resp)
	}

	if connVal := r.Context().Value(http3.ServerContextKey); connVal != nil {
		if conn, ok := connVal.(*quic.Conn); ok {
			go s.runDatagramSession(r.Context(), conn, resp)
		}
	}
}

func (s *Server) handleKeepalive(w http.ResponseWriter, r *http.Request) {
	var ka control.Keepalive
	if err := json.NewDecoder(r.Body).Decode(&ka); err != nil {
		http.Error(w, "invalid keepalive", http.StatusBadRequest)
		return
	}
	s.logger.Infof("keepalive remote=%s session=%s rtt_ms=%d loss_ppm=%d rx_mbps=%d tx_mbps=%d",
		r.RemoteAddr, ka.SessionID, ka.LatestRTTMillis, ka.LossPPM, ka.RxMbps, ka.TxMbps)
	s.sessionsMu.Lock()
	if sess, ok := s.sessions[ka.SessionID]; ok {
		sess.LastKeepaliveAt = time.Now()
	}
	s.sessionsMu.Unlock()
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleRoute(w http.ResponseWriter, r *http.Request) {
	sessionID := r.URL.Query().Get("session_id")
	if sessionID == "" {
		http.Error(w, "missing session_id", http.StatusBadRequest)
		return
	}
	s.logger.Infof("route update request remote=%s session=%s", r.RemoteAddr, sessionID)
	s.sessionsMu.Lock()
	sess, ok := s.sessions[sessionID]
	s.sessionsMu.Unlock()
	if !ok {
		http.Error(w, "session not found", http.StatusNotFound)
		return
	}
	best, current, alternates := s.pool.SelectWithCurrent(control.AuthRequest{
		Token:               s.cfg.Control.Token,
		RequestedCongestion: []string{sess.SelectedCC},
	}, sess.Route.Name)

	chosen := best
	reason := "periodic_reassessment"
	if chosen.Name == "" {
		chosen = sess.Route
		reason = "fallback_to_current"
	} else {
		stickinessLeft := sess.RouteChosenAt.Add(s.cfg.Control.RouteStickinessMin.Std()).After(time.Now())
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

	if chosen.Name != "" && chosen.Name != sess.Route.Name {
		s.sessionsMu.Lock()
		if live, ok := s.sessions[sessionID]; ok {
			live.Route = chosen
			live.BackendUDPAddr = backendForRoute(chosen, s.cfg.BackendUDPAddr)
			live.RouteChosenAt = time.Now()
			live.RouteScore = chosen.Score
		}
		s.sessionsMu.Unlock()
	}

	update := control.RouteUpdate{
		SessionID:          sessionID,
		SelectedRoute:      chosen,
		AlternateRoutes:    alternates,
		SelectedCongestion: sess.SelectedCC,
		ReconnectHint:      backendForRoute(chosen, s.cfg.BackendUDPAddr),
		Reason:             reason,
	}
	s.logger.Infof("route update response remote=%s session=%s chosen=%s reason=%s reconnect_hint=%s",
		r.RemoteAddr, sessionID, update.SelectedRoute.Name, update.Reason, update.ReconnectHint)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(update)
}

func (s *Server) runDatagramSession(ctx context.Context, conn *quic.Conn, resp control.AuthResponse) {
	state := s.lookupAuthStateByRemote(conn.RemoteAddr().String())
	if state == nil || !state.authenticated {
		s.logger.Warnf("hy2 udp rejected: unauthenticated remote=%s", conn.RemoteAddr())
		return
	}
	if s.cfg.Compatibility.EnableHysteriaUDP {
		s.logger.Infof("hy2 udp compatibility session start session=%s remote=%s", resp.SessionID, conn.RemoteAddr())
		srv := newHY2UDPServer(conn, s.logger, s.cfg.Control.IdleTimeout.Std())
		srv.run(ctx)
		return
	}
	backendAddr := backendForRoute(resp.SelectedRoute, s.cfg.BackendUDPAddr)
	if backendAddr == "" {
		s.logger.Warnf("session=%s missing backend addr", resp.SessionID)
		return
	}
	udpRemote, err := net.ResolveUDPAddr("udp", backendAddr)
	if err != nil {
		s.logger.Errorf("session=%s resolve backend failed: %v", resp.SessionID, err)
		return
	}
	udpConn, err := net.DialUDP("udp", nil, udpRemote)
	if err != nil {
		s.logger.Errorf("session=%s dial backend failed: %v", resp.SessionID, err)
		return
	}
	defer udpConn.Close()

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	go func() {
		buf := make([]byte, 64<<10)
		for {
			n, err := udpConn.Read(buf)
			if err != nil {
				cancel()
				return
			}
			if err := conn.SendDatagram(buf[:n]); err != nil {
				cancel()
				return
			}
		}
	}()

	for {
		msg, err := conn.ReceiveDatagram(ctx)
		if err != nil {
			if !errors.Is(err, context.Canceled) {
				s.logger.Warnf("session=%s receive datagram end: %v", resp.SessionID, err)
			}
			return
		}
		if _, err := udpConn.Write(msg); err != nil {
			s.logger.Warnf("session=%s udp write failed: %v", resp.SessionID, err)
			return
		}
	}
}

func (s *Server) handleStreamHijack(ft http3.FrameType, connTracingID quic.ConnectionTracingID, stream *quic.Stream, err error) (bool, error) {
	if err != nil {
		return false, err
	}
	if uint64(ft) != proxy.FrameTypeTCPRequest {
		return false, nil
	}
	state := s.lookupAuthStateByConnID(connTracingID)
	if state == nil || !state.authenticated {
		s.logger.Warnf("tcp stream rejected before auth frame_type=%d conn_tracing_id=%d", uint64(ft), connTracingID)
		(*stream).CancelRead(0)
		_ = stream.Close()
		return true, nil
	}
	s.logger.Infof("tcp stream hijacked frame_type=%d auth_session=%s", uint64(ft), state.sessionID)
	go s.handleTCPStream(stream)
	return true, nil
}

func (s *Server) lookupAuthStateByRemote(remote string) *authState {
	s.authMu.RLock()
	defer s.authMu.RUnlock()
	return s.authByRemote[remote]
}

func (s *Server) lookupAuthStateByConnID(id quic.ConnectionTracingID) *authState {
	s.authMu.RLock()
	defer s.authMu.RUnlock()
	return s.authByTrace[id]
}

func (s *Server) handleTCPStream(stream *quic.Stream) {
	target, err := proxy.ReadTCPRequestAny(stream)
	if err != nil {
		s.logger.Warnf("tcp stream request decode failed: %v", err)
		_ = stream.Close()
		return
	}
	backend, err := net.DialTimeout("tcp", target, 10*time.Second)
	if err != nil {
		s.logger.Warnf("tcp stream dial target=%s failed: %v", target, err)
		_ = proxy.WriteTCPResponse(stream, false, err.Error())
		_ = stream.Close()
		return
	}
	s.logger.Infof("tcp stream connected target=%s", target)
	if err := proxy.WriteTCPResponse(stream, true, "connected"); err != nil {
		_ = backend.Close()
		_ = stream.Close()
		return
	}
	s.logger.Infof("tcp stream proxy start target=%s", target)
	var wg sync.WaitGroup
	copyHalf := func(dst io.Writer, src io.Reader) {
		defer wg.Done()
		_, _ = io.Copy(dst, src)
	}
	wg.Add(2)
	go copyHalf(backend, stream)
	go copyHalf(stream, backend)
	wg.Wait()
	_ = backend.Close()
	(*stream).CancelRead(0)
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

func newSessionID(seed string, now time.Time) string {
	return seed + "-" + now.UTC().Format("20060102T150405.000000000")
}
