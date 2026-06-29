package pool

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
)

type Telemetry struct {
	LatencyMillis    float64   `json:"latency_millis"`
	JitterMillis     float64   `json:"jitter_millis"`
	LossPPM          float64   `json:"loss_ppm"`
	AvailableMbps    float64   `json:"available_mbps"`
	CurrentSessions  int       `json:"current_sessions"`
	LastUpdated      time.Time `json:"last_updated"`
	HealthPenalty    float64   `json:"health_penalty"`
	BandwidthPenalty float64   `json:"bandwidth_penalty"`
	ProbeOK          bool      `json:"probe_ok"`
	ProbeError       string    `json:"probe_error"`
	ProbeSamples     int       `json:"probe_samples"`
	State            string    `json:"state,omitempty"`
	Priority         int       `json:"priority,omitempty"`
	Active           bool      `json:"active,omitempty"`
	Candidate        bool      `json:"candidate,omitempty"`
	Draining         bool      `json:"draining,omitempty"`
	DrainUntil       int64     `json:"drain_until,omitempty"`
}

type Node struct {
	Config    config.PoolNode
	Line      config.LineConfig
	IsLine    bool
	Telemetry Telemetry
}

type Manager struct {
	cfg   config.PoolConfig
	mu    sync.RWMutex
	nodes map[string]*Node
}

func NewManager(cfg config.PoolConfig) *Manager {
	m := &Manager{
		cfg:   cfg,
		nodes: make(map[string]*Node, len(cfg.Nodes)),
	}
	for _, n := range cfg.Nodes {
		node := n
		m.nodes[n.Name] = &Node{
			Config: node,
			Telemetry: Telemetry{
				LatencyMillis: float64(maxInt(node.Priority, 1) * 10),
				LossPPM:       0,
				AvailableMbps: float64(maxInt(node.CapacityMbps, 100)),
				LastUpdated:   time.Now(),
			},
		}
	}
	for _, line := range cfg.Lines {
		normalized := normalizeLine(line)
		if normalized.ID == "" {
			continue
		}
		key := "line:" + normalized.ID
		m.nodes[key] = &Node{
			Config: lineAsNode(normalized),
			Line:   normalized,
			IsLine: true,
			Telemetry: Telemetry{
				LatencyMillis: float64(maxInt(normalized.Priority, 1) * 10),
				LossPPM:       0,
				AvailableMbps: float64(maxInt(normalized.CapacityMbps, 100)),
				LastUpdated:   time.Now(),
			},
		}
	}
	return m
}

func (m *Manager) StartHealthProbes(ctx context.Context) {
	interval := m.cfg.ProbeInterval.Std()
	if interval <= 0 {
		interval = 10 * time.Second
	}
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		m.ProbeOnce(ctx)
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				m.ProbeOnce(ctx)
			}
		}
	}()
}

func (m *Manager) ProbeOnce(ctx context.Context) {
	m.RefreshBackendMetrics()
	snapshot := m.snapshotNodes()
	for name, node := range snapshot {
		target := probeTarget(node)
		if target.Addr == "" && target.HealthURL == "" {
			continue
		}
		result := runProbeSeries(ctx, m.cfg.ProbeTimeout.Std(), probeSamples(m.cfg, node), target)
		m.applyProbeResult(name, result)
	}
}

func (m *Manager) RefreshBackendMetrics() {
	path := strings.TrimSpace(m.cfg.BackendMetricsPath)
	if path == "" {
		return
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var metrics map[string]Telemetry
	if err := json.Unmarshal(data, &metrics); err != nil {
		return
	}
	for name, telemetry := range metrics {
		if telemetry.State != "" {
			switch telemetry.State {
			case "active":
				telemetry.Active = true
			case "candidate":
				telemetry.Candidate = true
			case "draining":
				telemetry.Draining = true
			}
		}
		m.UpdateTelemetry(name, telemetry)
	}
}

func (m *Manager) UpdateTelemetry(name string, telemetry Telemetry) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, key := range candidateKeys(name) {
		if node, ok := m.nodes[key]; ok {
			node.Telemetry = telemetry
			if node.Telemetry.LastUpdated.IsZero() {
				node.Telemetry.LastUpdated = time.Now()
			}
			return
		}
	}
}

func (m *Manager) Snapshot() map[string]Telemetry {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make(map[string]Telemetry, len(m.nodes))
	for name, node := range m.nodes {
		out[name] = node.Telemetry
	}
	return out
}

func (m *Manager) IncrementSessions(name string, delta int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, key := range candidateKeys(name) {
		if node, ok := m.nodes[key]; ok {
			node.Telemetry.CurrentSessions += delta
			if node.Telemetry.CurrentSessions < 0 {
				node.Telemetry.CurrentSessions = 0
			}
			node.Telemetry.LastUpdated = time.Now()
			return
		}
	}
}

func (m *Manager) Select(req control.AuthRequest) (control.RouteCandidate, []control.RouteCandidate) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	candidates := make([]control.RouteCandidate, 0, len(m.nodes))
	for _, node := range m.nodes {
		score, reasons, ok := scoreNode(m.cfg, node, req)
		if !ok {
			continue
		}
		candidates = append(candidates, control.RouteCandidate{
			Name:           node.Config.Name,
			LineID:         routeLineID(node),
			Address:        node.Config.Address,
			Region:         node.Config.Region,
			Protocol:       node.Config.Protocol,
			Score:          score,
			Reasons:        reasons,
			BackendUDPAddr: node.Config.BackendUDPAddr,
			Hops:           routeHops(node),
		})
	}

	slices.SortFunc(candidates, func(a, b control.RouteCandidate) int {
		if a.Score == b.Score {
			return strings.Compare(a.Name, b.Name)
		}
		if a.Score > b.Score {
			return -1
		}
		return 1
	})

	if len(candidates) == 0 {
		return control.RouteCandidate{}, nil
	}
	best := candidates[0]
	rest := []control.RouteCandidate(nil)
	if len(candidates) > 1 {
		rest = append(rest, candidates[1:]...)
	}
	return best, rest
}

func (m *Manager) SelectWithCurrent(req control.AuthRequest, currentName string) (control.RouteCandidate, control.RouteCandidate, []control.RouteCandidate) {
	best, alternates := m.Select(req)
	var current control.RouteCandidate
	if currentName == "" {
		return best, current, alternates
	}
	for _, candidate := range append([]control.RouteCandidate{best}, alternates...) {
		if candidate.Name == currentName || candidate.LineID == currentName || "line:"+candidate.LineID == currentName {
			current = candidate
			break
		}
	}
	return best, current, alternates
}

func scoreNode(cfg config.PoolConfig, node *Node, req control.AuthRequest) (float64, []string, bool) {
	reasons := make([]string, 0, 8)
	if len(cfg.RequiredTags) > 0 && !containsAll(node.Config.Tags, cfg.RequiredTags) {
		return 0, nil, false
	}
	if len(req.RequestedTags) > 0 && !containsAll(node.Config.Tags, req.RequestedTags) {
		return 0, nil, false
	}

	score := float64(node.Config.Weight*10 + node.Config.Priority*5 - node.Config.BasePenalty*4)
	reasons = append(reasons, "base-weight")

	if cfg.EnableLatencyAware {
		score -= math.Min(node.Telemetry.LatencyMillis/5, 200)
		reasons = append(reasons, "latency-aware")
	}
	if cfg.EnableJitterAware {
		score -= math.Min(node.Telemetry.JitterMillis/2, 120)
		reasons = append(reasons, "jitter-aware")
	}
	if cfg.EnableLossAware {
		score -= math.Min(node.Telemetry.LossPPM/1000, 200)
		reasons = append(reasons, "loss-aware")
	}
	if cfg.EnableBandwidthAware {
		score += math.Min(node.Telemetry.AvailableMbps/5, 200)
		reasons = append(reasons, "bandwidth-aware")
	}
	if cfg.EnableSessionPressure && node.Config.MaxSessions > 0 {
		ratio := float64(node.Telemetry.CurrentSessions) / float64(node.Config.MaxSessions)
		score -= ratio * 100
		reasons = append(reasons, "session-pressure")
	}
	if node.Telemetry.Draining {
		score -= 80
		reasons = append(reasons, "backend-draining")
	}
	if node.Telemetry.Candidate {
		score += 5
		reasons = append(reasons, "backend-prewarmed")
	}
	if node.Telemetry.Active {
		score += 2
		reasons = append(reasons, "backend-active")
	}
	if cfg.EnableCapacityAware && node.Config.CapacityMbps > 0 {
		score += math.Min(float64(node.Config.CapacityMbps)/10, 100)
		reasons = append(reasons, "capacity-aware")
	}
	if len(cfg.PreferRegions) > 0 && slices.Contains(cfg.PreferRegions, node.Config.Region) {
		score += 40
		reasons = append(reasons, "preferred-region")
	}
	score -= node.Telemetry.HealthPenalty
	score -= node.Telemetry.BandwidthPenalty
	if node.Telemetry.ProbeOK {
		reasons = append(reasons, "probe-ok")
	}
	if node.Telemetry.ProbeError != "" {
		reasons = append(reasons, "probe-degraded")
	}
	return score, reasons, true
}

func normalizeLine(line config.LineConfig) config.LineConfig {
	if line.ID == "" {
		line.ID = line.Name
	}
	if line.Name == "" {
		line.Name = line.ID
	}
	if line.Protocol == "" {
		line.Protocol = "xgw-native"
	}
	if line.Address == "" {
		for _, hop := range line.Hops {
			if hop.Role == "ingress" {
				line.Address = firstNonEmpty(hop.PublicAddr, hop.PrivateAddr, hop.BackendUDPAddr)
				break
			}
		}
	}
	if line.BackendUDPAddr == "" {
		for _, hop := range line.Hops {
			if hop.Role == "ingress" && hop.BackendUDPAddr != "" {
				line.BackendUDPAddr = hop.BackendUDPAddr
				break
			}
		}
	}
	return line
}

func lineAsNode(line config.LineConfig) config.PoolNode {
	return config.PoolNode{
		Name:           line.Name,
		Address:        line.Address,
		Region:         line.Region,
		Role:           "line",
		Protocol:       line.Protocol,
		ProbeProtocol:  line.ProbeProtocol,
		ProbeAddr:      line.ProbeAddr,
		ProbePayload:   line.ProbePayload,
		Priority:       line.Priority,
		Weight:         line.Weight,
		BasePenalty:    line.BasePenalty,
		CapacityMbps:   line.CapacityMbps,
		MaxSessions:    line.MaxSessions,
		Tags:           line.Tags,
		HealthURL:      line.HealthURL,
		BackendUDPAddr: line.BackendUDPAddr,
	}
}

func (m *Manager) snapshotNodes() map[string]Node {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := make(map[string]Node, len(m.nodes))
	for name, node := range m.nodes {
		out[name] = *node
	}
	return out
}

func (m *Manager) applyProbeResult(name string, result probeResult) {
	m.mu.Lock()
	defer m.mu.Unlock()
	node := m.nodes[name]
	if node == nil {
		return
	}
	previous := node.Telemetry
	if result.Attempts == 0 || result.Successes == 0 {
		previous.LossPPM = ewma(previous.LossPPM, 1000000, 0.35)
		previous.HealthPenalty = math.Min(previous.HealthPenalty+40, 300)
		previous.ProbeOK = false
		previous.ProbeError = result.Error
		previous.ProbeSamples = result.Attempts
		previous.LastUpdated = time.Now()
		node.Telemetry = previous
		return
	}
	latencyMs := result.AvgMillis
	if previous.LatencyMillis <= 0 {
		previous.LatencyMillis = latencyMs
		previous.JitterMillis = result.JitterMillis
	} else {
		previous.JitterMillis = ewma(previous.JitterMillis, result.JitterMillis+math.Abs(latencyMs-previous.LatencyMillis), 0.35)
		previous.LatencyMillis = ewma(previous.LatencyMillis, latencyMs, 0.35)
	}
	lossPPM := float64(result.Attempts-result.Successes) / float64(result.Attempts) * 1000000
	previous.LossPPM = ewma(previous.LossPPM, lossPPM, 0.35)
	previous.HealthPenalty = math.Max(previous.HealthPenalty-25, 0)
	previous.ProbeOK = result.Successes == result.Attempts
	previous.ProbeError = result.Error
	previous.ProbeSamples = result.Attempts
	previous.LastUpdated = time.Now()
	node.Telemetry = previous
}

type probeSpec struct {
	Protocol  string
	Addr      string
	HealthURL string
	Payload   string
}

func probeTarget(node Node) probeSpec {
	if node.IsLine {
		if node.Line.ProbeAddr != "" || node.Line.HealthURL != "" {
			return probeSpec{
				Protocol:  node.Line.ProbeProtocol,
				Addr:      node.Line.ProbeAddr,
				HealthURL: node.Line.HealthURL,
				Payload:   node.Line.ProbePayload,
			}
		}
		return probeSpec{Protocol: node.Line.ProbeProtocol, Addr: addressToProbeAddr(node.Config.Address), Payload: node.Line.ProbePayload}
	}
	if node.Config.HealthURL != "" {
		return probeSpec{Protocol: node.Config.ProbeProtocol, HealthURL: node.Config.HealthURL, Payload: node.Config.ProbePayload}
	}
	return probeSpec{Protocol: node.Config.ProbeProtocol, Addr: firstNonEmpty(node.Config.ProbeAddr, addressToProbeAddr(node.Config.Address)), Payload: node.Config.ProbePayload}
}

func addressToProbeAddr(address string) string {
	if address == "" {
		return ""
	}
	if parsed, err := url.Parse(address); err == nil && parsed.Host != "" {
		return parsed.Host
	}
	return address
}

func runProbe(ctx context.Context, timeout time.Duration, target probeSpec) (time.Duration, error) {
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	probeCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	start := time.Now()
	if target.HealthURL != "" {
		req, err := http.NewRequestWithContext(probeCtx, http.MethodGet, target.HealthURL, nil)
		if err != nil {
			return 0, err
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return 0, err
		}
		defer resp.Body.Close()
		if resp.StatusCode >= 500 {
			return 0, &probeStatusError{status: resp.StatusCode}
		}
		return time.Since(start), nil
	}
	if strings.EqualFold(target.Protocol, "udp") || strings.EqualFold(target.Protocol, "quic") {
		return runUDPProbe(probeCtx, target.Addr, target.Payload, start)
	}
	dialer := &net.Dialer{Timeout: timeout}
	conn, err := dialer.DialContext(probeCtx, "tcp", target.Addr)
	if err != nil {
		return 0, err
	}
	_ = conn.Close()
	return time.Since(start), nil
}

type probeResult struct {
	Attempts     int
	Successes    int
	AvgMillis    float64
	JitterMillis float64
	Error        string
}

func runProbeSeries(ctx context.Context, timeout time.Duration, samples int, target probeSpec) probeResult {
	if samples <= 0 {
		samples = 1
	}
	result := probeResult{Attempts: samples}
	latencies := make([]float64, 0, samples)
	for i := 0; i < samples; i++ {
		latency, err := runProbe(ctx, timeout, target)
		if err != nil {
			result.Error = err.Error()
			continue
		}
		result.Successes++
		latencies = append(latencies, float64(latency.Microseconds())/1000)
	}
	if len(latencies) == 0 {
		if result.Error == "" {
			result.Error = "all probes failed"
		}
		return result
	}
	var sum float64
	for _, value := range latencies {
		sum += value
	}
	result.AvgMillis = sum / float64(len(latencies))
	var variance float64
	for _, value := range latencies {
		diff := value - result.AvgMillis
		variance += diff * diff
	}
	result.JitterMillis = math.Sqrt(variance / float64(len(latencies)))
	if result.Successes == result.Attempts {
		result.Error = ""
	}
	return result
}

func probeSamples(cfg config.PoolConfig, node Node) int {
	if node.IsLine && node.Line.ProbeSamples > 0 {
		return node.Line.ProbeSamples
	}
	if node.Config.ProbeSamples > 0 {
		return node.Config.ProbeSamples
	}
	if cfg.ProbeSamples > 0 {
		return cfg.ProbeSamples
	}
	return 3
}

func runUDPProbe(ctx context.Context, addr string, payload string, start time.Time) (time.Duration, error) {
	if strings.TrimSpace(addr) == "" {
		return 0, errors.New("empty udp probe addr")
	}
	if payload == "" {
		payload = "XGW-UDP-PROBE"
	}
	dialer := &net.Dialer{}
	conn, err := dialer.DialContext(ctx, "udp", addr)
	if err != nil {
		return 0, err
	}
	defer conn.Close()
	deadline, ok := ctx.Deadline()
	if ok {
		_ = conn.SetDeadline(deadline)
	}
	if _, err := conn.Write([]byte(payload)); err != nil {
		return 0, err
	}
	buf := make([]byte, 2048)
	n, err := conn.Read(buf)
	if err != nil {
		return 0, err
	}
	if n == 0 {
		return 0, errors.New("empty udp probe reply")
	}
	return time.Since(start), nil
}

type probeStatusError struct {
	status int
}

func (e *probeStatusError) Error() string {
	return "health status " + strconv.Itoa(e.status)
}

func ewma(old float64, sample float64, alpha float64) float64 {
	if old == 0 {
		return sample
	}
	return old*(1-alpha) + sample*alpha
}

func routeLineID(node *Node) string {
	if node == nil || !node.IsLine {
		return ""
	}
	return node.Line.ID
}

func routeHops(node *Node) []control.RouteHop {
	if node == nil || !node.IsLine || len(node.Line.Hops) == 0 {
		return nil
	}
	hops := make([]control.RouteHop, 0, len(node.Line.Hops))
	for _, hop := range node.Line.Hops {
		hops = append(hops, control.RouteHop{
			Name:           hop.Name,
			Role:           hop.Role,
			PublicAddr:     hop.PublicAddr,
			PrivateAddr:    hop.PrivateAddr,
			BackendUDPAddr: hop.BackendUDPAddr,
		})
	}
	return hops
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}

func candidateKeys(name string) []string {
	if name == "" {
		return nil
	}
	return []string{name, "line:" + name}
}

func containsAll(tags []string, required []string) bool {
	for _, req := range required {
		if !slices.Contains(tags, req) {
			return false
		}
	}
	return true
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
