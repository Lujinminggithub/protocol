package edge

import (
	"context"
	"net"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"go.uber.org/zap"
)

type managedPhase int

const (
	phaseStable managedPhase = iota
	phasePrewarm
	phaseDrain
)

type inboundMessage struct {
	payload []byte
	source  string
}

type ManagedClient struct {
	cfg          config.ClientConfig
	logger       *zap.SugaredLogger
	client       *Client
	mu           sync.RWMutex
	session      *ClientSession
	candidate    *ClientSession
	phase        managedPhase
	drainFor     time.Duration
	prewarmFor   time.Duration
	inboundCh    chan inboundMessage
	bootstrapped bool
}

func NewManagedClient(cfg config.ClientConfig, logger *zap.SugaredLogger) *ManagedClient {
	return &ManagedClient{
		cfg:        cfg,
		logger:     logger,
		client:     NewClient(cfg, logger),
		drainFor:   5 * time.Second,
		prewarmFor: 3 * time.Second,
		inboundCh:  make(chan inboundMessage, 4096),
	}
}

func (m *ManagedClient) Connect(ctx context.Context) (*ClientSession, error) {
	session, err := m.client.Connect(ctx)
	if err != nil {
		return nil, err
	}
	session.MarkActivated()
	m.mu.Lock()
	m.session = session
	m.phase = phaseStable
	m.bootstrapped = true
	m.mu.Unlock()
	m.attachSessionPump(ctx, session, "active")
	return session, nil
}

func (m *ManagedClient) attachSessionPump(ctx context.Context, session *ClientSession, source string) {
	go func() {
		for {
			msg, err := session.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			select {
			case m.inboundCh <- inboundMessage{payload: append([]byte(nil), msg...), source: source}:
			case <-ctx.Done():
				return
			}
		}
	}()
}

func (m *ManagedClient) CurrentSession() *ClientSession {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.session
}

func (m *ManagedClient) StartAutoReconnect(ctx context.Context) {
	go func() {
		interval := m.cfg.Control.RouteUpdateInterval.Std()
		if interval <= 0 {
			interval = 2 * time.Second
		}
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				session := m.CurrentSession()
				if session == nil {
					continue
				}
				update, err := session.FetchRouteUpdateForSession(ctx)
				if err != nil {
					m.logger.Warnf("managed-client route update failed: %v", err)
					continue
				}
				m.logger.Infof("route updated route=%s hint=%s reason=%s", update.SelectedRoute.Name, update.ReconnectHint, update.Reason)
				hint := update.ReconnectHint
				route := update.SelectedRoute
				if hint == "" && route.Address == "" {
					continue
				}
				if !m.shouldSwitchTarget(route.Name, route.Address, hint) {
					continue
				}
				if err := m.switchSession(ctx, route.Name, route.Address, hint); err != nil {
					m.logger.Warnf("managed-client switch failed: %v", err)
				}
			}
		}
	}()
}

func (m *ManagedClient) shouldSwitchTarget(name string, address string, hint string) bool {
	m.mu.RLock()
	active := m.session
	candidate := m.candidate
	phase := m.phase
	currentURL := m.cfg.ServerURL
	m.mu.RUnlock()

	targetSig := routeSignature(name, address, hint)
	if targetSig == "" {
		return false
	}

	activeSig := ""
	if active != nil {
		activeSig = routeSignature(active.CurrentRoute().Name, active.CurrentRoute().Address, active.CurrentReconnectHint())
	}
	candidateSig := ""
	if candidate != nil {
		candidateSig = routeSignature(candidate.CurrentRoute().Name, candidate.CurrentRoute().Address, candidate.CurrentReconnectHint())
	}

	if targetSig == activeSig || targetSig == candidateSig {
		return false
	}
	if phase == phaseDrain && targetSig == candidateSig {
		return false
	}
	if address != "" && address == currentURL && hint == "" {
		return false
	}
	return true
}

func (m *ManagedClient) switchSession(ctx context.Context, routeName string, newAddress string, hint string) error {
	m.mu.Lock()
	if m.phase == phasePrewarm {
		m.mu.Unlock()
		return nil
	}
	m.phase = phasePrewarm
	m.mu.Unlock()

	m.logger.Infof("managed-client prewarm route=%s address=%s hint=%s", routeName, newAddress, hint)
	newCfg := m.cfg
	if newAddress != "" {
		newCfg.ServerURL = newAddress
	}
	newCfg.BootstrapNode = ""
	newClient := NewClient(newCfg, m.logger)
	newSession, err := newClient.Connect(ctx)
	if err != nil {
		m.mu.Lock()
		m.phase = phaseStable
		m.mu.Unlock()
		return err
	}
	newSession.MarkActivated()
	m.attachSessionPump(ctx, newSession, "candidate")

	m.mu.Lock()
	old := m.session
	m.candidate = newSession
	m.client = newClient
	m.cfg = newCfg
	m.mu.Unlock()

	time.Sleep(m.prewarmFor)

	m.mu.Lock()
	m.session = newSession
	m.phase = phaseDrain
	m.mu.Unlock()

	m.logger.Infof("managed-client activate new route=%s", newSession.CurrentRoute().Name)
	if old != nil {
		go m.drainAndCloseOld(old)
	}
	return nil
}

func (m *ManagedClient) drainAndCloseOld(old *ClientSession) {
	m.logger.Infof("managed-client draining old session route=%s", old.CurrentRoute().Name)
	time.Sleep(m.drainFor)
	_ = old.Close()
	m.mu.Lock()
	if m.phase == phaseDrain {
		m.phase = phaseStable
		m.candidate = nil
	}
	m.mu.Unlock()
}

func (m *ManagedClient) SendDatagram(ctx context.Context, payload []byte) error {
	m.mu.RLock()
	active := m.session
	candidate := m.candidate
	phase := m.phase
	m.mu.RUnlock()

	if active == nil {
		return context.Canceled
	}
	if phase == phasePrewarm && candidate != nil {
		err1 := active.SendDatagram(ctx, payload)
		err2 := candidate.SendDatagram(ctx, payload)
		if err1 != nil {
			return err1
		}
		return err2
	}
	return active.SendDatagram(ctx, payload)
}

func (m *ManagedClient) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	session := m.CurrentSession()
	if session == nil {
		return nil, context.Canceled
	}
	return session.DialTCP(ctx, target)
}

func (m *ManagedClient) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	select {
	case msg := <-m.inboundCh:
		return msg.payload, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (m *ManagedClient) LocalUDPProxy(ctx context.Context, listenAddr string) error {
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
			msg, err := m.ReceiveDatagram(ctx)
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
		if err := m.SendDatagram(ctx, buf[:n]); err != nil {
			return err
		}
	}
}

func (m *ManagedClient) LocalTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	return m.localTUNProxy(ctx, tunName, tunCIDR)
}

func routeSignature(name string, address string, hint string) string {
	if name == "" && address == "" && hint == "" {
		return ""
	}
	return name + "|" + address + "|" + hint
}
