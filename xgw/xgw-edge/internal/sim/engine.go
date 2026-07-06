package sim

import (
	"context"
	"encoding/binary"
	"io"
	"math"
	"math/rand"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"go.uber.org/zap"
)

const (
	controlMagic = "XGWCTRL1"
	mediaMagic   = "XGWMEDIA"
)

type Options struct {
	ConfigPath         string
	ConnectTimeout     time.Duration
	Scenario           string
	MediaTarget        string
	MediaRateMbps      float64
	MediaPayloadBytes  int
	ControlUDPTarget   string
	ControlUDPInterval time.Duration
	ControlUDPTimeout  time.Duration
	ControlTCPEnabled  bool
	ControlTCPTarget   string
	ControlTCPInterval time.Duration
	ControlTCPTimeout  time.Duration
	ControlTCPPayload  string
	ControlTCPRead     int
}

type Engine struct {
	options Options
	cfg     config.ClientConfig
	logger  *zap.SugaredLogger

	client simClient

	ctx    context.Context
	cancel context.CancelFunc
	start  time.Time

	mediaSentPackets atomic.Uint64
	mediaSentBytes   byteRate
	mediaSendErrors  atomic.Uint64
	udpRecvBytes     byteRate
	udpRecvPackets   atomic.Uint64
	unknownRecv      atomic.Uint64

	controlUDPStats *probeStats
	controlTCPStats *probeStats
	routeInfo       *routeState
	udpTracker      *udpControlTracker
	mediaRateCtrl   *mediaRateController
	burstInfo       *burstState
	autoInfo        *autoScenarioState
}

type Snapshot struct {
	Now              time.Time
	Start            time.Time
	Scenario         string
	Route            RouteSnapshot
	MediaTarget      string
	MediaRateMbps    float64
	MediaPayload     int
	MediaPackets     uint64
	MediaBytes       uint64
	MediaErrors      uint64
	ControlUDPTarget string
	ControlUDP       ProbeSnapshot
	ControlTCPTarget string
	ControlTCP       ProbeSnapshot
	UDPRecvPackets   uint64
	UDPRecvBytes     uint64
	UnknownRecv      uint64
	Burst            BurstSnapshot
	AutoScenario     AutoScenarioSnapshot
}

type DeltaSnapshot struct {
	Total      Snapshot
	MediaBps   float64
	UDPRecvBps float64
}

type ProbeSnapshot struct {
	OK         uint64
	Fail       uint64
	AvgRTT     time.Duration
	MinRTT     time.Duration
	MaxRTT     time.Duration
	LastRTT    time.Duration
	LastErr    string
	LastOKAt   time.Time
	LastFailAt time.Time
}

type RouteSnapshot struct {
	SessionID       string
	RouteName       string
	LineID          string
	Address         string
	Congestion      string
	SwitchCount     uint64
	LastSwitchAt    time.Time
	LastRouteReason string
	DefaultBudget   control.StreamBudgetSnapshot
	Scheduler       control.SchedulerSnapshot
	Keepalive       control.Keepalive
}

type BurstSnapshot struct {
	Active    bool
	Label     string
	RateMbps  float64
	EndsAt    time.Time
	LastBurst time.Time
}

type AutoScenarioSnapshot struct {
	Enabled         bool
	CurrentEvent    string
	NextEvent       string
	NextEventAt     time.Time
	LastTriggeredAt time.Time
}

func NewEngine(options Options, logger *zap.SugaredLogger) (*Engine, error) {
	cfg, err := config.LoadClient(options.ConfigPath)
	if err != nil {
		return nil, err
	}
	if logger == nil {
		zl, lerr := zap.NewDevelopment()
		if lerr != nil {
			return nil, lerr
		}
		logger = zl.Sugar()
	}
	return &Engine{
		options:         options,
		cfg:             cfg,
		logger:          logger,
		controlUDPStats: &probeStats{},
		controlTCPStats: &probeStats{},
		routeInfo:       &routeState{},
		udpTracker:      newUDPControlTracker(),
		mediaRateCtrl:   &mediaRateController{baseRateMbps: options.MediaRateMbps},
		burstInfo:       &burstState{},
		autoInfo:        &autoScenarioState{},
	}, nil
}

func (e *Engine) Start() error {
	if e.cancel != nil {
		return nil
	}
	e.ctx, e.cancel = context.WithCancel(context.Background())
	e.start = time.Now()
	e.client = newSimClient(e.cfg, e.logger)
	connectTimeout := e.options.ConnectTimeout
	if connectTimeout <= 0 {
		connectTimeout = 12 * time.Second
	}
	connectCtx, connectCancel := context.WithTimeout(e.ctx, connectTimeout)
	defer connectCancel()
	session, err := e.client.Connect(connectCtx)
	if err != nil {
		e.cancel()
		e.cancel = nil
		return err
	}
	e.logger.Infof("xgw sim engine connected session=%s route=%s line=%s frontend=%s", session.SessionID, session.RouteName, session.LineID, e.cfg.FrontendMode)
	e.routeInfo.update(session)
	e.client.StartAutoReconnect(e.ctx)
	if strings.EqualFold(e.cfg.FrontendMode, "hy2-official-bridge") &&
		(strings.EqualFold(e.options.Scenario, "media") ||
			strings.EqualFold(e.options.Scenario, "control-udp") ||
			strings.EqualFold(e.options.Scenario, "mixed")) {
		e.logger.Warnf("sim engine frontend=%s currently provides full TCP tests but only limited UDP target support", e.cfg.FrontendMode)
	}

	go recvLoop(e.ctx, e.client, e.udpTracker, e.controlUDPStats, &e.udpRecvBytes, &e.udpRecvPackets, &e.unknownRecv)
	go routeLoop(e.ctx, e.client, e.routeInfo)
	go udpTimeoutLoop(e.ctx, e.udpTracker, e.controlUDPStats)
	if strings.EqualFold(e.options.Scenario, "mixed") {
		e.autoInfo.setEnabled(true)
		go autoScenarioLoop(e.ctx, e)
	}

	switch strings.ToLower(e.options.Scenario) {
	case "media":
		if e.options.MediaTarget != "" {
			go mediaLoop(e.ctx, e.client, e.options.MediaTarget, e.mediaRateCtrl, e.options.MediaPayloadBytes, &e.mediaSentPackets, &e.mediaSentBytes, &e.mediaSendErrors)
		}
	case "control-udp":
		if e.options.ControlUDPTarget != "" {
			go controlUDPLoop(e.ctx, e.client, e.options.ControlUDPTarget, e.options.ControlUDPInterval, e.options.ControlUDPTimeout, e.udpTracker)
		}
	case "control-tcp":
		if e.options.ControlTCPEnabled && e.options.ControlTCPTarget != "" {
			go controlTCPLoop(e.ctx, e.client, e.options.ControlTCPTarget, e.options.ControlTCPInterval, e.options.ControlTCPTimeout, e.options.ControlTCPPayload, e.options.ControlTCPRead, e.controlTCPStats)
		}
	default:
		if e.options.MediaTarget != "" {
			go mediaLoop(e.ctx, e.client, e.options.MediaTarget, e.mediaRateCtrl, e.options.MediaPayloadBytes, &e.mediaSentPackets, &e.mediaSentBytes, &e.mediaSendErrors)
		}
		if e.options.ControlUDPTarget != "" {
			go controlUDPLoop(e.ctx, e.client, e.options.ControlUDPTarget, e.options.ControlUDPInterval, e.options.ControlUDPTimeout, e.udpTracker)
		}
		if e.options.ControlTCPEnabled && e.options.ControlTCPTarget != "" {
			go controlTCPLoop(e.ctx, e.client, e.options.ControlTCPTarget, e.options.ControlTCPInterval, e.options.ControlTCPTimeout, e.options.ControlTCPPayload, e.options.ControlTCPRead, e.controlTCPStats)
		}
	}
	return nil
}

func (e *Engine) Stop() {
	if e.cancel != nil {
		e.cancel()
		e.cancel = nil
	}
}

func (e *Engine) Snapshot() Snapshot {
	return Snapshot{
		Now:              time.Now(),
		Start:            e.start,
		Scenario:         e.options.Scenario,
		Route:            e.routeInfo.snapshot(),
		MediaTarget:      e.options.MediaTarget,
		MediaRateMbps:    e.options.MediaRateMbps,
		MediaPayload:     e.options.MediaPayloadBytes,
		MediaPackets:     e.mediaSentPackets.Load(),
		MediaBytes:       e.mediaSentBytes.total.Load(),
		MediaErrors:      e.mediaSendErrors.Load(),
		ControlUDPTarget: e.options.ControlUDPTarget,
		ControlUDP:       e.controlUDPStats.snapshot(),
		ControlTCPTarget: e.options.ControlTCPTarget,
		ControlTCP:       e.controlTCPStats.snapshot(),
		UDPRecvPackets:   e.udpRecvPackets.Load(),
		UDPRecvBytes:     e.udpRecvBytes.total.Load(),
		UnknownRecv:      e.unknownRecv.Load(),
		Burst:            e.burstInfo.snapshot(),
		AutoScenario:     e.autoInfo.snapshot(),
	}
}

func (e *Engine) DeltaSnapshot(delta time.Duration) DeltaSnapshot {
	return DeltaSnapshot{
		Total:      e.Snapshot(),
		MediaBps:   e.mediaSentBytes.snapshot(delta),
		UDPRecvBps: e.udpRecvBytes.snapshot(delta),
	}
}

func (e *Engine) RouteCommand(lineID string) bool {
	if e.client == nil {
		return false
	}
	if lineID == "" {
		return false
	}
	return e.client.RouteCommand(e.ctx, lineID)
}

func (e *Engine) TriggerMediaBurst(label string, rateMbps float64, duration time.Duration) {
	if duration <= 0 {
		duration = 3 * time.Second
	}
	if rateMbps <= 0 {
		rateMbps = 30
	}
	e.mediaRateCtrl.activate(rateMbps, duration)
	e.burstInfo.activate(label, rateMbps, duration)
	e.logger.Infof("sim burst media label=%s rate_mbps=%.2f duration=%s", label, rateMbps, duration)
}

func (e *Engine) TriggerControlBurst(label string) {
	if e.ctx == nil || e.client == nil {
		return
	}
	switch strings.ToLower(label) {
	case "live-start":
		e.burstInfo.activate("开播动作", 12, 2*time.Second)
		e.mediaRateCtrl.activate(12, 2*time.Second)
		go e.runUDPBurst(e.options.ControlUDPTarget, 8, 120*time.Millisecond, e.options.ControlUDPTimeout)
		go e.runTCPBurst(e.options.ControlTCPTarget, 2, 400*time.Millisecond, e.options.ControlTCPTimeout)
	case "auction-start":
		e.burstInfo.activate("竞拍开始", 30, 3*time.Second)
		e.mediaRateCtrl.activate(30, 3*time.Second)
		go e.runUDPBurst(e.options.ControlUDPTarget, 20, 60*time.Millisecond, e.options.ControlUDPTimeout)
		go e.runTCPBurst(e.options.ControlTCPTarget, 6, 200*time.Millisecond, e.options.ControlTCPTimeout)
	case "high-freq":
		e.burstInfo.activate("高频小包突发", 4, 2*time.Second)
		go e.runUDPBurst(e.options.ControlUDPTarget, 40, 40*time.Millisecond, e.options.ControlUDPTimeout)
		go e.runTCPBurst(e.options.ControlTCPTarget, 8, 150*time.Millisecond, e.options.ControlTCPTimeout)
	default:
		e.burstInfo.activate(label, 0, 2*time.Second)
		go e.runUDPBurst(e.options.ControlUDPTarget, 10, 100*time.Millisecond, e.options.ControlUDPTimeout)
	}
	e.logger.Infof("sim burst control label=%s", label)
}

func autoScenarioLoop(ctx context.Context, e *Engine) {
	r := rand.New(rand.NewSource(time.Now().UnixNano()))
	for {
		wait := time.Duration(8+r.Intn(11)) * time.Second
		nextPick := r.Intn(100)
		e.autoInfo.schedule(randomEventName(nextPick), time.Now().Add(wait))
		select {
		case <-ctx.Done():
			e.autoInfo.setEnabled(false)
			return
		case <-time.After(wait):
		}
		pick := r.Intn(100)
		e.autoInfo.trigger(randomEventName(pick))
		switch {
		case pick < 20:
			e.TriggerControlBurst("live-start")
		case pick < 50:
			e.TriggerControlBurst("auction-start")
		case pick < 80:
			e.TriggerControlBurst("high-freq")
		default:
			e.TriggerMediaBurst("random media peak", 30, 3*time.Second)
		}
	}
}

func randomEventName(pick int) string {
	switch {
	case pick < 20:
		return "live-start"
	case pick < 50:
		return "auction-start"
	case pick < 80:
		return "high-freq"
	default:
		return "random-media-peak"
	}
}

func (e *Engine) runUDPBurst(target string, count int, interval, timeout time.Duration) {
	if target == "" || count <= 0 {
		return
	}
	for i := 0; i < count; i++ {
		if e.ctx.Err() != nil {
			return
		}
		seq := e.udpTracker.register(timeout)
		payload := buildControlPayload(seq, time.Now())
		_ = e.client.SendDatagramTo(e.ctx, target, payload)
		time.Sleep(interval)
	}
}

func (e *Engine) runTCPBurst(target string, count int, interval, timeout time.Duration) {
	if !e.options.ControlTCPEnabled || target == "" || count <= 0 {
		return
	}
	for i := 0; i < count; i++ {
		if e.ctx.Err() != nil {
			return
		}
		begin := time.Now()
		probeCtx, cancel := context.WithTimeout(e.ctx, timeout)
		conn, err := e.client.DialTCP(probeCtx, target)
		if err != nil {
			e.controlTCPStats.recordFail(err.Error())
			cancel()
			time.Sleep(interval)
			continue
		}
		if e.options.ControlTCPPayload != "" {
			if _, err := conn.Write([]byte(e.options.ControlTCPPayload)); err != nil {
				e.controlTCPStats.recordFail(err.Error())
				_ = conn.Close()
				cancel()
				time.Sleep(interval)
				continue
			}
		}
		if e.options.ControlTCPRead > 0 {
			_ = conn.SetReadDeadline(time.Now().Add(timeout / 2))
			buf := make([]byte, e.options.ControlTCPRead)
			if _, err := conn.Read(buf); err != nil && err != io.EOF {
				e.controlTCPStats.recordFail(err.Error())
				_ = conn.Close()
				cancel()
				time.Sleep(interval)
				continue
			}
		}
		_ = conn.Close()
		cancel()
		e.controlTCPStats.recordOK(time.Since(begin))
		time.Sleep(interval)
	}
}

type probeStats struct {
	mu         sync.Mutex
	ok         uint64
	fail       uint64
	totalRTT   time.Duration
	minRTT     time.Duration
	maxRTT     time.Duration
	lastRTT    time.Duration
	lastErr    string
	lastOKAt   time.Time
	lastFailAt time.Time
}

func (p *probeStats) recordOK(rtt time.Duration) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.ok++
	p.totalRTT += rtt
	p.lastRTT = rtt
	p.lastOKAt = time.Now()
	if p.minRTT == 0 || rtt < p.minRTT {
		p.minRTT = rtt
	}
	if rtt > p.maxRTT {
		p.maxRTT = rtt
	}
}

func (p *probeStats) recordFail(err string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.fail++
	p.lastErr = err
	p.lastFailAt = time.Now()
}

func (p *probeStats) snapshot() ProbeSnapshot {
	p.mu.Lock()
	defer p.mu.Unlock()
	avg := time.Duration(0)
	if p.ok > 0 {
		avg = time.Duration(int64(p.totalRTT) / int64(p.ok))
	}
	return ProbeSnapshot{
		OK:         p.ok,
		Fail:       p.fail,
		AvgRTT:     avg,
		MinRTT:     p.minRTT,
		MaxRTT:     p.maxRTT,
		LastRTT:    p.lastRTT,
		LastErr:    p.lastErr,
		LastOKAt:   p.lastOKAt,
		LastFailAt: p.lastFailAt,
	}
}

type byteRate struct {
	total atomic.Uint64
	last  uint64
}

type mediaRateController struct {
	mu            sync.RWMutex
	baseRateMbps  float64
	burstRateMbps float64
	burstUntil    time.Time
}

func (m *mediaRateController) currentRate() float64 {
	m.mu.RLock()
	defer m.mu.RUnlock()
	if !m.burstUntil.IsZero() && time.Now().Before(m.burstUntil) && m.burstRateMbps > 0 {
		return m.burstRateMbps
	}
	return m.baseRateMbps
}

func (m *mediaRateController) activate(rateMbps float64, duration time.Duration) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.burstRateMbps = rateMbps
	m.burstUntil = time.Now().Add(duration)
}

func (b *byteRate) add(n uint64) {
	b.total.Add(n)
}

func (b *byteRate) snapshot(delta time.Duration) float64 {
	cur := b.total.Load()
	diff := cur - b.last
	b.last = cur
	if delta <= 0 {
		return 0
	}
	return float64(diff*8) / delta.Seconds()
}

type routeState struct {
	mu              sync.Mutex
	sessionID       string
	routeName       string
	lineID          string
	address         string
	congestion      string
	switchCount     uint64
	lastSwitchAt    time.Time
	lastRouteReason string
	defaultBudget   control.StreamBudgetSnapshot
	scheduler       control.SchedulerSnapshot
	keepalive       control.Keepalive
}

func (r *routeState) update(info sessionInfo) {
	r.mu.Lock()
	defer r.mu.Unlock()
	changed := r.sessionID != "" && (r.sessionID != info.SessionID || r.lineID != info.LineID || r.routeName != info.RouteName)
	r.sessionID = info.SessionID
	r.routeName = info.RouteName
	r.lineID = info.LineID
	r.address = info.Address
	r.congestion = info.Congestion
	r.lastRouteReason = info.LastRouteReason
	r.defaultBudget = info.DefaultBudget
	r.scheduler = info.Scheduler
	r.keepalive = info.Keepalive
	if changed {
		r.switchCount++
		r.lastSwitchAt = time.Now()
	}
}

func (r *routeState) snapshot() RouteSnapshot {
	r.mu.Lock()
	defer r.mu.Unlock()
	return RouteSnapshot{
		SessionID:       r.sessionID,
		RouteName:       r.routeName,
		LineID:          r.lineID,
		Address:         r.address,
		Congestion:      r.congestion,
		SwitchCount:     r.switchCount,
		LastSwitchAt:    r.lastSwitchAt,
		LastRouteReason: r.lastRouteReason,
		DefaultBudget:   r.defaultBudget,
		Scheduler:       r.scheduler,
		Keepalive:       r.keepalive,
	}
}

type udpPending struct {
	sentAt   time.Time
	deadline time.Time
}

type burstState struct {
	mu        sync.Mutex
	active    bool
	label     string
	rateMbps  float64
	endsAt    time.Time
	lastBurst time.Time
}

type autoScenarioState struct {
	mu              sync.Mutex
	enabled         bool
	currentEvent    string
	nextEvent       string
	nextEventAt     time.Time
	lastTriggeredAt time.Time
}

func (a *autoScenarioState) setEnabled(enabled bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.enabled = enabled
	if !enabled {
		a.currentEvent = ""
		a.nextEvent = ""
		a.nextEventAt = time.Time{}
	}
}

func (a *autoScenarioState) schedule(name string, when time.Time) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.nextEvent = name
	a.nextEventAt = when
}

func (a *autoScenarioState) trigger(name string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.currentEvent = name
	a.lastTriggeredAt = time.Now()
	a.nextEvent = ""
	a.nextEventAt = time.Time{}
}

func (a *autoScenarioState) snapshot() AutoScenarioSnapshot {
	a.mu.Lock()
	defer a.mu.Unlock()
	current := a.currentEvent
	if !a.lastTriggeredAt.IsZero() && time.Since(a.lastTriggeredAt) > 5*time.Second {
		current = ""
	}
	return AutoScenarioSnapshot{
		Enabled:         a.enabled,
		CurrentEvent:    current,
		NextEvent:       a.nextEvent,
		NextEventAt:     a.nextEventAt,
		LastTriggeredAt: a.lastTriggeredAt,
	}
}

func (b *burstState) activate(label string, rateMbps float64, duration time.Duration) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.active = true
	b.label = label
	b.rateMbps = rateMbps
	b.endsAt = time.Now().Add(duration)
	b.lastBurst = time.Now()
}

func (b *burstState) snapshot() BurstSnapshot {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.active && !b.endsAt.IsZero() && time.Now().After(b.endsAt) {
		b.active = false
	}
	return BurstSnapshot{
		Active:    b.active,
		Label:     b.label,
		RateMbps:  b.rateMbps,
		EndsAt:    b.endsAt,
		LastBurst: b.lastBurst,
	}
}

type udpControlTracker struct {
	mu      sync.Mutex
	nextSeq uint64
	pending map[uint64]udpPending
}

func newUDPControlTracker() *udpControlTracker {
	return &udpControlTracker{pending: make(map[uint64]udpPending)}
}

func (u *udpControlTracker) register(timeout time.Duration) uint64 {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.nextSeq++
	now := time.Now()
	u.pending[u.nextSeq] = udpPending{sentAt: now, deadline: now.Add(timeout)}
	return u.nextSeq
}

func (u *udpControlTracker) ack(seq uint64) (time.Duration, bool) {
	u.mu.Lock()
	defer u.mu.Unlock()
	p, ok := u.pending[seq]
	if !ok {
		return 0, false
	}
	delete(u.pending, seq)
	return time.Since(p.sentAt), true
}

func (u *udpControlTracker) reap(now time.Time) []uint64 {
	u.mu.Lock()
	defer u.mu.Unlock()
	var expired []uint64
	for seq, p := range u.pending {
		if !now.Before(p.deadline) {
			expired = append(expired, seq)
			delete(u.pending, seq)
		}
	}
	return expired
}

func mediaLoop(ctx context.Context, client simClient, target string, rateCtrl *mediaRateController, payloadBytes int, packets *atomic.Uint64, bytes *byteRate, errs *atomic.Uint64) {
	if payloadBytes < 64 {
		payloadBytes = 64
	}
	tick := 50 * time.Millisecond
	ticker := time.NewTicker(tick)
	defer ticker.Stop()
	seq := uint64(0)
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			rateMbps := rateCtrl.currentRate()
			if rateMbps <= 0 {
				rateMbps = 1.0
			}
			bytesPerTick := int(math.Max(1, (rateMbps*1_000_000.0/8.0)*tick.Seconds()))
			perTickPackets := int(math.Max(1, math.Round(float64(bytesPerTick)/float64(payloadBytes))))
			for i := 0; i < perTickPackets; i++ {
				seq++
				payload := buildMediaPayload(seq, now, payloadBytes)
				if err := client.SendDatagramTo(ctx, target, payload); err != nil {
					errs.Add(1)
					break
				}
				packets.Add(1)
				bytes.add(uint64(len(payload)))
			}
		}
	}
}

func controlUDPLoop(ctx context.Context, client simClient, target string, interval, timeout time.Duration, tracker *udpControlTracker) {
	if interval <= 0 {
		interval = 2 * time.Second
	}
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			seq := tracker.register(timeout)
			payload := buildControlPayload(seq, now)
			_ = client.SendDatagramTo(ctx, target, payload)
		}
	}
}

func controlTCPLoop(ctx context.Context, client simClient, target string, interval, timeout time.Duration, payload string, readBytes int, stats *probeStats) {
	if interval <= 0 {
		interval = 5 * time.Second
	}
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			begin := time.Now()
			probeCtx, cancel := context.WithTimeout(ctx, timeout)
			conn, err := client.DialTCP(probeCtx, target)
			if err != nil {
				stats.recordFail(err.Error())
				cancel()
				continue
			}
			if payload != "" {
				if _, err := conn.Write([]byte(payload)); err != nil {
					stats.recordFail(err.Error())
					_ = conn.Close()
					cancel()
					continue
				}
			}
			if readBytes > 0 {
				_ = conn.SetReadDeadline(time.Now().Add(timeout / 2))
				buf := make([]byte, readBytes)
				if _, err := conn.Read(buf); err != nil && err != io.EOF {
					stats.recordFail(err.Error())
					_ = conn.Close()
					cancel()
					continue
				}
			}
			_ = conn.Close()
			cancel()
			stats.recordOK(time.Since(begin))
		}
	}
}

func recvLoop(ctx context.Context, client simClient, tracker *udpControlTracker, udpStats *probeStats, recvBytes *byteRate, recvPackets *atomic.Uint64, unknown *atomic.Uint64) {
	for {
		dg, err := client.ReceiveDatagramFrom(ctx)
		if err != nil {
			return
		}
		recvPackets.Add(1)
		recvBytes.add(uint64(len(dg.Payload)))
		if seq, ok := parseControlPayload(dg.Payload); ok {
			if rtt, matched := tracker.ack(seq); matched {
				udpStats.recordOK(rtt)
				continue
			}
		}
		unknown.Add(1)
	}
}

func routeLoop(ctx context.Context, client simClient, routes *routeState) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			session := client.CurrentSession()
			if session.SessionID == "" && session.RouteName == "" && session.LineID == "" {
				continue
			}
			routes.update(session)
		}
	}
}

func udpTimeoutLoop(ctx context.Context, tracker *udpControlTracker, stats *probeStats) {
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			expired := tracker.reap(now)
			for range expired {
				stats.recordFail("udp echo timeout")
			}
		}
	}
}

func buildControlPayload(seq uint64, now time.Time) []byte {
	buf := make([]byte, len(controlMagic)+8+8)
	copy(buf, []byte(controlMagic))
	binary.BigEndian.PutUint64(buf[len(controlMagic):], seq)
	binary.BigEndian.PutUint64(buf[len(controlMagic)+8:], uint64(now.UnixNano()))
	return buf
}

func parseControlPayload(buf []byte) (uint64, bool) {
	if len(buf) < len(controlMagic)+16 {
		return 0, false
	}
	if string(buf[:len(controlMagic)]) != controlMagic {
		return 0, false
	}
	return binary.BigEndian.Uint64(buf[len(controlMagic):]), true
}

func buildMediaPayload(seq uint64, now time.Time, size int) []byte {
	if size < len(mediaMagic)+16 {
		size = len(mediaMagic) + 16
	}
	buf := make([]byte, size)
	copy(buf, []byte(mediaMagic))
	binary.BigEndian.PutUint64(buf[len(mediaMagic):], seq)
	binary.BigEndian.PutUint64(buf[len(mediaMagic)+8:], uint64(now.UnixNano()))
	fill := len(mediaMagic) + 16
	for i := fill; i < len(buf); i++ {
		buf[i] = byte('A' + (i % 26))
	}
	return buf
}
