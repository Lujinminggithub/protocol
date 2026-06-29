package main

import (
	"context"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"os/signal"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

const (
	controlMagic = "XGWCTRL1"
	mediaMagic   = "XGWMEDIA"
)

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

func (p *probeStats) snapshot() probeSnapshot {
	p.mu.Lock()
	defer p.mu.Unlock()
	avg := time.Duration(0)
	if p.ok > 0 {
		avg = time.Duration(int64(p.totalRTT) / int64(p.ok))
	}
	return probeSnapshot{
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

type probeSnapshot struct {
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

type byteRate struct {
	total atomic.Uint64
	last  uint64
}

func (b *byteRate) add(n uint64) {
	b.total.Add(n)
}

func (b *byteRate) snapshot(delta time.Duration) (uint64, float64) {
	cur := b.total.Load()
	diff := cur - b.last
	b.last = cur
	bps := 0.0
	if delta > 0 {
		bps = float64(diff*8) / delta.Seconds()
	}
	return cur, bps
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

func (r *routeState) update(session *native.Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if session == nil {
		return
	}
	changed := r.sessionID != "" && (r.sessionID != session.Resp.SessionID || r.lineID != session.Resp.SelectedRoute.LineID || r.routeName != session.Resp.SelectedRoute.Name)
	r.sessionID = session.Resp.SessionID
	r.routeName = session.Resp.SelectedRoute.Name
	r.lineID = session.Resp.SelectedRoute.LineID
	r.address = session.Resp.SelectedRoute.Address
	r.congestion = session.Resp.SelectedCongestion
	r.lastRouteReason = session.LastRouteReason()
	r.defaultBudget = session.Resp.DefaultBudget
	r.scheduler = session.Resp.Scheduler
	r.keepalive = session.LastKeepalive()
	if changed {
		r.switchCount++
		r.lastSwitchAt = time.Now()
	}
}

func (r *routeState) snapshot() routeSnapshot {
	r.mu.Lock()
	defer r.mu.Unlock()
	return routeSnapshot{
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

type routeSnapshot struct {
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

type udpPending struct {
	sentAt   time.Time
	deadline time.Time
}

type udpControlTracker struct {
	mu      sync.Mutex
	nextSeq uint64
	pending map[uint64]udpPending
}

func newUDPControlTracker() *udpControlTracker {
	return &udpControlTracker{pending: make(map[uint64]udpPending)}
}

func (u *udpControlTracker) register(timeout time.Duration) (uint64, time.Time) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.nextSeq++
	now := time.Now()
	u.pending[u.nextSeq] = udpPending{
		sentAt:   now,
		deadline: now.Add(timeout),
	}
	return u.nextSeq, now
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

func main() {
	var (
		configPath         string
		scenario           string
		refresh            time.Duration
		mediaTarget        string
		mediaRateMbps      float64
		mediaPayloadBytes  int
		controlUDPTarget   string
		controlUDPInterval time.Duration
		controlUDPTimeout  time.Duration
		controlTCPEnabled  bool
		controlTCPTarget   string
		controlTCPInterval time.Duration
		controlTCPTimeout  time.Duration
		controlTCPPayload  string
		controlTCPRead     int
		plain              bool
	)

	flag.StringVar(&configPath, "config", "", "client config json")
	flag.StringVar(&scenario, "scenario", "mixed", "media|control-udp|control-tcp|mixed")
	flag.DurationVar(&refresh, "refresh", 1*time.Second, "dashboard refresh interval")
	flag.StringVar(&mediaTarget, "media-target", "", "UDP target for media stream, e.g. 1.2.3.4:9000")
	flag.Float64Var(&mediaRateMbps, "media-rate-mbps", 3.0, "target media send rate in Mbps")
	flag.IntVar(&mediaPayloadBytes, "media-payload-bytes", 1100, "media UDP payload bytes")
	flag.StringVar(&controlUDPTarget, "control-udp-target", "", "UDP echo target for control probe")
	flag.DurationVar(&controlUDPInterval, "control-udp-interval", 2*time.Second, "control UDP probe interval")
	flag.DurationVar(&controlUDPTimeout, "control-udp-timeout", 3*time.Second, "control UDP probe timeout")
	flag.BoolVar(&controlTCPEnabled, "control-tcp", false, "enable TCP control probes")
	flag.StringVar(&controlTCPTarget, "control-tcp-target", "", "TCP target for control probe")
	flag.DurationVar(&controlTCPInterval, "control-tcp-interval", 5*time.Second, "control TCP probe interval")
	flag.DurationVar(&controlTCPTimeout, "control-tcp-timeout", 5*time.Second, "control TCP timeout")
	flag.StringVar(&controlTCPPayload, "control-tcp-payload", "", "optional TCP payload to write")
	flag.IntVar(&controlTCPRead, "control-tcp-read", 0, "bytes to read back after TCP write; 0 means connect-only")
	flag.BoolVar(&plain, "plain", false, "disable ANSI dashboard clear and print plain snapshots")
	flag.Parse()

	cfg, err := config.LoadClient(configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load config: %v\n", err)
		os.Exit(1)
	}
	logger, err := zap.NewDevelopment()
	if err != nil {
		fmt.Fprintf(os.Stderr, "logger: %v\n", err)
		os.Exit(1)
	}
	defer logger.Sync()

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	client := native.NewManagedClient(cfg, logger.Sugar())
	session, err := client.Connect(ctx)
	if err != nil {
		logger.Sugar().Errorf("xgw sim client connect failed: %v", err)
		os.Exit(1)
	}
	logger.Sugar().Infof("xgw sim client connected session=%s route=%s line=%s", session.Resp.SessionID, session.Resp.SelectedRoute.Name, session.Resp.SelectedRoute.LineID)
	client.StartAutoReconnect(ctx)

	var (
		mediaSentPackets atomic.Uint64
		mediaSentBytes   byteRate
		mediaSendErrors  atomic.Uint64
		udpRecvBytes     byteRate
		udpRecvPackets   atomic.Uint64
		unknownRecv      atomic.Uint64
	)
	controlUDPStats := &probeStats{}
	controlTCPStats := &probeStats{}
	routeInfo := &routeState{}
	routeInfo.update(session)
	udpTracker := newUDPControlTracker()

	go recvLoop(ctx, client, udpTracker, controlUDPStats, &udpRecvBytes, &udpRecvPackets, &unknownRecv)
	go routeLoop(ctx, client, routeInfo)
	go udpTimeoutLoop(ctx, udpTracker, controlUDPStats)

	switch strings.ToLower(scenario) {
	case "media":
		if mediaTarget != "" {
			go mediaLoop(ctx, client, mediaTarget, mediaRateMbps, mediaPayloadBytes, &mediaSentPackets, &mediaSentBytes, &mediaSendErrors)
		}
	case "control-udp":
		if controlUDPTarget != "" {
			go controlUDPLoop(ctx, client, controlUDPTarget, controlUDPInterval, controlUDPTimeout, udpTracker)
		}
	case "control-tcp":
		if controlTCPEnabled && controlTCPTarget != "" {
			go controlTCPLoop(ctx, client, controlTCPTarget, controlTCPInterval, controlTCPTimeout, controlTCPPayload, controlTCPRead, controlTCPStats)
		}
	default:
		if mediaTarget != "" {
			go mediaLoop(ctx, client, mediaTarget, mediaRateMbps, mediaPayloadBytes, &mediaSentPackets, &mediaSentBytes, &mediaSendErrors)
		}
		if controlUDPTarget != "" {
			go controlUDPLoop(ctx, client, controlUDPTarget, controlUDPInterval, controlUDPTimeout, udpTracker)
		}
		if controlTCPEnabled && controlTCPTarget != "" {
			go controlTCPLoop(ctx, client, controlTCPTarget, controlTCPInterval, controlTCPTimeout, controlTCPPayload, controlTCPRead, controlTCPStats)
		}
	}

	ticker := time.NewTicker(refresh)
	defer ticker.Stop()
	lastTick := time.Now()
	start := time.Now()

	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			delta := now.Sub(lastTick)
			lastTick = now
			routeSnap := routeInfo.snapshot()
			udpSnap := controlUDPStats.snapshot()
			tcpSnap := controlTCPStats.snapshot()
			totalMediaBytes, mediaBps := mediaSentBytes.snapshot(delta)
			totalUDPRecvBytes, udpRxBps := udpRecvBytes.snapshot(delta)
			snap := dashboardSnapshot{
				Now:              now,
				Start:            start,
				Scenario:         scenario,
				Route:            routeSnap,
				MediaTarget:      mediaTarget,
				MediaRateMbps:    mediaRateMbps,
				MediaPayload:     mediaPayloadBytes,
				MediaPackets:     mediaSentPackets.Load(),
				MediaBytes:       totalMediaBytes,
				MediaBps:         mediaBps,
				MediaErrors:      mediaSendErrors.Load(),
				ControlUDPTarget: controlUDPTarget,
				ControlUDP:       udpSnap,
				ControlTCPTarget: controlTCPTarget,
				ControlTCP:       tcpSnap,
				UDPRecvPackets:   udpRecvPackets.Load(),
				UDPRecvBytes:     totalUDPRecvBytes,
				UDPRecvBps:       udpRxBps,
				UnknownRecv:      unknownRecv.Load(),
			}
			renderDashboard(snap, plain)
		}
	}
}

type dashboardSnapshot struct {
	Now              time.Time
	Start            time.Time
	Scenario         string
	Route            routeSnapshot
	MediaTarget      string
	MediaRateMbps    float64
	MediaPayload     int
	MediaPackets     uint64
	MediaBytes       uint64
	MediaBps         float64
	MediaErrors      uint64
	ControlUDPTarget string
	ControlUDP       probeSnapshot
	ControlTCPTarget string
	ControlTCP       probeSnapshot
	UDPRecvPackets   uint64
	UDPRecvBytes     uint64
	UDPRecvBps       float64
	UnknownRecv      uint64
}

func renderDashboard(s dashboardSnapshot, plain bool) {
	if !plain {
		fmt.Print("\033[H\033[2J")
	}
	uptime := s.Now.Sub(s.Start).Round(time.Second)
	fmt.Printf("XGW Windows Local Simulator  |  scenario=%s  |  uptime=%s\n", s.Scenario, uptime)
	fmt.Printf("time=%s\n", s.Now.Format(time.RFC3339))
	fmt.Println(strings.Repeat("=", 96))
	fmt.Printf("route: session=%s  name=%s  line=%s  cc=%s\n", emptyDash(s.Route.SessionID), emptyDash(s.Route.RouteName), emptyDash(s.Route.LineID), emptyDash(s.Route.Congestion))
	fmt.Printf("route: address=%s  switches=%d  last_switch=%s  reason=%s\n", emptyDash(s.Route.Address), s.Route.SwitchCount, fmtTime(s.Route.LastSwitchAt), emptyDash(s.Route.LastRouteReason))
	fmt.Printf("budget: flow=%s  priority=%s  copies=%d  reduced_fec=%t  fast_ack=%t  independent_io=%t\n",
		emptyDash(s.Route.DefaultBudget.FlowClass),
		emptyDash(s.Route.DefaultBudget.Priority),
		s.Route.DefaultBudget.PreferredCopies,
		s.Route.DefaultBudget.ReducedFEC,
		s.Route.DefaultBudget.FastACK,
		s.Route.DefaultBudget.IndependentIO)
	fmt.Printf("budget: read_timeout=%dms  idle_after_first_byte=%dms\n",
		s.Route.DefaultBudget.ReadTimeoutMillis,
		s.Route.DefaultBudget.IdleAfterFirstByteMs)
	fmt.Printf("scheduler: mode=%s  per_stream=%t  drr=%t  starvation=%t  cc_coupled=%t\n",
		emptyDash(s.Route.Scheduler.Mode),
		s.Route.Scheduler.PerStreamAccounting,
		s.Route.Scheduler.DeficitRoundRobin,
		s.Route.Scheduler.StarvationProtection,
		s.Route.Scheduler.SessionCCCoupled)
	fmt.Printf("accounting: inflight=%dB  send_credit=%dB  ack_debt=%d\n",
		s.Route.Keepalive.InFlightBytes,
		s.Route.Keepalive.SendCreditBytes,
		s.Route.Keepalive.AckCreditFrames)
	fmt.Println(strings.Repeat("-", 96))
	fmt.Printf("media: target=%s  configured_rate=%.2f Mbps  payload=%dB\n", emptyDash(s.MediaTarget), s.MediaRateMbps, s.MediaPayload)
	fmt.Printf("media: sent_packets=%d  sent_bytes=%d  current_tx=%.2f Mbps  send_errors=%d\n", s.MediaPackets, s.MediaBytes, s.MediaBps/1_000_000.0, s.MediaErrors)
	fmt.Println(strings.Repeat("-", 96))
	fmt.Printf("control-udp: target=%s  ok=%d fail=%d avg=%s min=%s max=%s last=%s\n",
		emptyDash(s.ControlUDPTarget), s.ControlUDP.OK, s.ControlUDP.Fail, fmtDuration(s.ControlUDP.AvgRTT), fmtDuration(s.ControlUDP.MinRTT), fmtDuration(s.ControlUDP.MaxRTT), fmtDuration(s.ControlUDP.LastRTT))
	fmt.Printf("control-udp: last_ok=%s  last_fail=%s  last_err=%s\n",
		fmtTime(s.ControlUDP.LastOKAt), fmtTime(s.ControlUDP.LastFailAt), emptyDash(s.ControlUDP.LastErr))
	fmt.Println(strings.Repeat("-", 96))
	fmt.Printf("control-tcp: target=%s  ok=%d fail=%d avg=%s min=%s max=%s last=%s\n",
		emptyDash(s.ControlTCPTarget), s.ControlTCP.OK, s.ControlTCP.Fail, fmtDuration(s.ControlTCP.AvgRTT), fmtDuration(s.ControlTCP.MinRTT), fmtDuration(s.ControlTCP.MaxRTT), fmtDuration(s.ControlTCP.LastRTT))
	fmt.Printf("control-tcp: last_ok=%s  last_fail=%s  last_err=%s\n",
		fmtTime(s.ControlTCP.LastOKAt), fmtTime(s.ControlTCP.LastFailAt), emptyDash(s.ControlTCP.LastErr))
	fmt.Println(strings.Repeat("-", 96))
	fmt.Printf("udp-rx: packets=%d  bytes=%d  current_rx=%.2f Mbps  unknown_rx=%d\n", s.UDPRecvPackets, s.UDPRecvBytes, s.UDPRecvBps/1_000_000.0, s.UnknownRecv)
	fmt.Println(strings.Repeat("=", 96))
	fmt.Println("tips:")
	fmt.Println("  - control-udp fail 增长通常表示 echo 未返回、链路抖动或切线期间控制流恢复慢")
	fmt.Println("  - media current_tx 与 configured_rate 偏差大，说明本地发送节奏或链路承载需要继续观察")
	fmt.Println("  - switches 增长说明 route update/切线已经发生，可结合 ok/fail 与吞吐变化判断是否平滑")
}

func mediaLoop(ctx context.Context, client *native.ManagedClient, target string, rateMbps float64, payloadBytes int, packets *atomic.Uint64, bytes *byteRate, errs *atomic.Uint64) {
	if payloadBytes < 64 {
		payloadBytes = 64
	}
	tick := 50 * time.Millisecond
	ticker := time.NewTicker(tick)
	defer ticker.Stop()
	if rateMbps <= 0 {
		rateMbps = 1.0
	}
	bytesPerTick := int(math.Max(1, (rateMbps*1_000_000.0/8.0)*tick.Seconds()))
	payloadSize := payloadBytes
	perTickPackets := int(math.Max(1, math.Round(float64(bytesPerTick)/float64(payloadSize))))
	seq := uint64(0)

	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			for i := 0; i < perTickPackets; i++ {
				seq++
				payload := buildMediaPayload(seq, now, payloadSize)
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

func controlUDPLoop(ctx context.Context, client *native.ManagedClient, target string, interval, timeout time.Duration, tracker *udpControlTracker) {
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
			seq, sentAt := tracker.register(timeout)
			payload := buildControlPayload(seq, now)
			if err := client.SendDatagramTo(ctx, target, payload); err != nil {
				_ = sentAt
			}
		}
	}
}

func controlTCPLoop(ctx context.Context, client *native.ManagedClient, target string, interval, timeout time.Duration, payload string, readBytes int, stats *probeStats) {
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

func recvLoop(ctx context.Context, client *native.ManagedClient, tracker *udpControlTracker, udpStats *probeStats, recvBytes *byteRate, recvPackets *atomic.Uint64, unknown *atomic.Uint64) {
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

func routeLoop(ctx context.Context, client *native.ManagedClient, routes *routeState) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			session := client.CurrentSession()
			if session == nil {
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

func emptyDash(v string) string {
	if strings.TrimSpace(v) == "" {
		return "-"
	}
	return v
}

func fmtTime(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.Format("15:04:05")
}

func fmtDuration(d time.Duration) string {
	if d <= 0 {
		return "-"
	}
	return d.Round(time.Millisecond).String()
}
