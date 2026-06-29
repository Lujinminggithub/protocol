package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/url"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

type result struct {
	OK      bool
	Kind    string
	Latency time.Duration
	Err     string
}

type loadOptions struct {
	TCPConnectTimeout time.Duration
	TCPReadTimeout    time.Duration
	UDPTimeout        time.Duration
	StopMargin        time.Duration
}

type summary struct {
	OK              bool              `json:"ok"`
	Profile         string            `json:"profile"`
	TargetTCP       string            `json:"target_tcp,omitempty"`
	TargetUDP       string            `json:"target_udp,omitempty"`
	TargetURLs      []string          `json:"target_urls,omitempty"`
	ModeDescription string            `json:"mode_description"`
	StartedAt       string            `json:"started_at"`
	FinishedAt      string            `json:"finished_at"`
	Concurrency     int               `json:"concurrency"`
	DurationSeconds float64           `json:"duration_seconds"`
	IntervalSeconds float64           `json:"interval_seconds"`
	StopMarginMs    float64           `json:"stop_margin_ms"`
	TCPConnectMs    float64           `json:"tcp_connect_timeout_ms"`
	TCPReadMs       float64           `json:"tcp_read_timeout_ms"`
	UDPTimeoutMs    float64           `json:"udp_timeout_ms"`
	MinSuccessRate  float64           `json:"min_success_rate"`
	Total           uint64            `json:"total"`
	Success         uint64            `json:"success"`
	Failed          uint64            `json:"failed"`
	SuccessRate     float64           `json:"success_rate"`
	P50Millis       float64           `json:"p50_ms"`
	P75Millis       float64           `json:"p75_ms"`
	P90Millis       float64           `json:"p90_ms"`
	P95Millis       float64           `json:"p95_ms"`
	P99Millis       float64           `json:"p99_ms"`
	SelectedRoutes  map[string]uint64 `json:"selected_routes"`
	SelectedLines   map[string]uint64 `json:"selected_lines"`
	Errors          map[string]uint64 `json:"errors"`
	RouteUpdates    routeUpdateStats  `json:"route_updates"`
}

type routeUpdateStats struct {
	Enabled        bool              `json:"enabled"`
	IntervalMs     float64           `json:"interval_ms"`
	Total          uint64            `json:"total"`
	Success        uint64            `json:"success"`
	Failed         uint64            `json:"failed"`
	SelectedRoutes map[string]uint64 `json:"selected_routes,omitempty"`
	SelectedLines  map[string]uint64 `json:"selected_lines,omitempty"`
	Reasons        map[string]uint64 `json:"reasons,omitempty"`
	Errors         map[string]uint64 `json:"errors,omitempty"`
}

type routeUpdateRecorder struct {
	enabled  bool
	interval time.Duration
	mu       sync.Mutex
	stats    routeUpdateStats
}

func newRouteUpdateRecorder(enabled bool, interval time.Duration) *routeUpdateRecorder {
	return &routeUpdateRecorder{
		enabled:  enabled,
		interval: interval,
		stats: routeUpdateStats{
			Enabled:        enabled,
			IntervalMs:     durationMillis(interval),
			SelectedRoutes: map[string]uint64{},
			SelectedLines:  map[string]uint64{},
			Reasons:        map[string]uint64{},
			Errors:         map[string]uint64{},
		},
	}
}

func (r *routeUpdateRecorder) recordOK(update *control.RouteUpdate) {
	if r == nil || update == nil {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.stats.Total++
	r.stats.Success++
	if update.SelectedRoute.Name != "" {
		r.stats.SelectedRoutes[update.SelectedRoute.Name]++
	}
	if update.SelectedRoute.LineID != "" {
		r.stats.SelectedLines[update.SelectedRoute.LineID]++
	}
	if update.Reason != "" {
		r.stats.Reasons[update.Reason]++
	}
}

func (r *routeUpdateRecorder) recordError(err error) {
	if r == nil {
		return
	}
	key := "unknown"
	if err != nil {
		key = err.Error()
		if len(key) > 160 {
			key = key[:160]
		}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.stats.Total++
	r.stats.Failed++
	r.stats.Errors[key]++
}

func (r *routeUpdateRecorder) snapshot() routeUpdateStats {
	if r == nil {
		return routeUpdateStats{}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	out := r.stats
	out.SelectedRoutes = cloneUintMap(r.stats.SelectedRoutes)
	out.SelectedLines = cloneUintMap(r.stats.SelectedLines)
	out.Reasons = cloneUintMap(r.stats.Reasons)
	out.Errors = cloneUintMap(r.stats.Errors)
	return out
}

func main() {
	var configPath string
	var profile string
	var targetTCP string
	var targetUDP string
	var targetURLsText string
	var duration time.Duration
	var concurrency int
	var payloadSize int
	var interval time.Duration
	var minSuccessRate float64
	var summaryPath string
	var routeUpdateInterval time.Duration
	var opts loadOptions
	flag.StringVar(&configPath, "config", "", "client config file")
	flag.StringVar(&profile, "profile", "mixed", "load profile: live, auction, browser, web, mixed")
	flag.StringVar(&targetTCP, "target-tcp", "ip.sb:443", "browser-like TCP target")
	flag.StringVar(&targetUDP, "target-udp", "udp.example:443", "live-like UDP target")
	flag.StringVar(&targetURLsText, "target-urls", "", "comma separated HTTPS URLs for web profile, for example https://www.tiktok.com/,https://www.youtube.com/")
	flag.DurationVar(&duration, "duration", 30*time.Second, "test duration")
	flag.IntVar(&concurrency, "concurrency", 4, "worker count")
	flag.IntVar(&payloadSize, "payload-size", 1200, "payload bytes per operation")
	flag.DurationVar(&interval, "interval", 100*time.Millisecond, "per-worker send interval")
	flag.DurationVar(&opts.StopMargin, "stop-margin", time.Second, "do not start a new operation in the final margin of the schedule window")
	flag.DurationVar(&opts.TCPConnectTimeout, "tcp-connect-timeout", 6*time.Second, "timeout for opening a TCP proxy stream")
	flag.DurationVar(&opts.TCPReadTimeout, "tcp-read-timeout", 6*time.Second, "timeout for reading the TCP echo response")
	flag.DurationVar(&opts.UDPTimeout, "udp-timeout", 4*time.Second, "timeout for one UDP echo operation")
	flag.DurationVar(&routeUpdateInterval, "route-update-interval", 0, "periodically fetch route updates without counting them as workload operations")
	flag.Float64Var(&minSuccessRate, "min-success-rate", 0.99, "minimum success rate required for ok=true")
	flag.StringVar(&summaryPath, "summary", "", "write JSON summary to this path")
	flag.Parse()

	if concurrency <= 0 {
		concurrency = 1
	}
	opts.normalize()
	targetURLs := parseTargetURLs(targetURLsText)
	if len(targetURLs) == 0 && strings.EqualFold(profile, "web") {
		targetURLs = []string{"https://www.tiktok.com/", "https://www.youtube.com/"}
	}
	cfg, err := config.LoadClient(configPath)
	if err != nil {
		fail("load config", err)
	}
	logger, _ := zap.NewDevelopment()
	defer logger.Sync()

	connectCtx, connectCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer connectCancel()
	scheduleCtx, scheduleCancel := context.WithTimeout(context.Background(), duration)
	defer scheduleCancel()
	startedAt := time.Now()

	results := make(chan result, concurrency*16)
	var wg sync.WaitGroup
	var routeMu sync.Mutex
	routes := map[string]uint64{}
	lines := map[string]uint64{}
	var total atomic.Uint64
	routeUpdates := newRouteUpdateRecorder(routeUpdateInterval > 0, routeUpdateInterval)

	for i := 0; i < concurrency; i++ {
		workerID := i
		wg.Add(1)
		go func() {
			defer wg.Done()
			client := native.NewClient(cfg, logger.Sugar())
			session, err := client.Connect(connectCtx)
			if err != nil {
				results <- result{Kind: "connect", Err: err.Error()}
				return
			}
			defer session.Close()
			routeMu.Lock()
			routes[session.Resp.SelectedRoute.Name]++
			if session.Resp.SelectedRoute.LineID != "" {
				lines[session.Resp.SelectedRoute.LineID]++
			}
			routeMu.Unlock()
			var stopRouteUpdates chan struct{}
			var routeWG sync.WaitGroup
			if routeUpdateInterval > 0 {
				stopRouteUpdates = make(chan struct{})
				routeWG.Add(1)
				go func() {
					defer routeWG.Done()
					runRouteUpdates(scheduleCtx, session, routeUpdateInterval, stopRouteUpdates, routeUpdates)
				}()
			}
			runWorker(scheduleCtx, session, profile, targetTCP, targetUDP, targetURLs, payload(workerID, payloadSize), interval, opts, results, &total)
			if stopRouteUpdates != nil {
				close(stopRouteUpdates)
				routeWG.Wait()
			}
		}()
	}

	go func() {
		wg.Wait()
		close(results)
	}()

	sum := collect(profile, targetTCP, targetUDP, targetURLs, concurrency, duration, interval, opts, minSuccessRate, startedAt, results)
	sum.FinishedAt = time.Now().UTC().Format(time.RFC3339Nano)
	sum.SelectedRoutes = routes
	sum.SelectedLines = lines
	sum.RouteUpdates = routeUpdates.snapshot()
	if summaryPath != "" {
		data, _ := json.MarshalIndent(sum, "", "  ")
		if err := os.WriteFile(summaryPath, data, 0o644); err != nil {
			fail("write summary", err)
		}
	}
	data, _ := json.MarshalIndent(sum, "", "  ")
	fmt.Println(string(data))
	if !sum.OK {
		os.Exit(1)
	}
}

func runRouteUpdates(ctx context.Context,
	session *native.Session,
	interval time.Duration,
	stop <-chan struct{},
	recorder *routeUpdateRecorder) {
	if interval <= 0 {
		return
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-stop:
			return
		case <-ticker.C:
			opCtx, cancel := context.WithTimeout(ctx, minDuration(interval, 5*time.Second))
			update, err := session.FetchRouteUpdate(opCtx)
			cancel()
			if err != nil {
				recorder.recordError(err)
				continue
			}
			recorder.recordOK(update)
		}
	}
}

func runWorker(ctx context.Context,
	session *native.Session,
	profile string,
	targetTCP string,
	targetUDP string,
	targetURLs []string,
	payload []byte,
	interval time.Duration,
	opts loadOptions,
	results chan<- result,
	total *atomic.Uint64) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if !hasOperationBudget(ctx, opts.StopMargin) {
				return
			}
			switch strings.ToLower(profile) {
			case "live":
				total.Add(1)
				results <- udpOnce(session, targetUDP, payload, opts)
			case "auction":
				for i := 0; i < 3; i++ {
					total.Add(1)
					results <- tcpOnce(session, targetTCP, payload[:minInt(len(payload), 64)], opts)
				}
			case "browser":
				total.Add(1)
				results <- tcpOnce(session, targetTCP, payload, opts)
			case "web":
				total.Add(1)
				results <- webOnce(session, pickTargetURL(targetURLs, int(total.Load())), opts)
			default:
				total.Add(2)
				results <- udpOnce(session, targetUDP, payload, opts)
				results <- tcpOnce(session, targetTCP, payload[:minInt(len(payload), 256)], opts)
			}
		}
	}
}

func hasOperationBudget(ctx context.Context, minLeft time.Duration) bool {
	deadline, ok := ctx.Deadline()
	if !ok {
		return true
	}
	return time.Until(deadline) >= minLeft
}

func udpOnce(session *native.Session, target string, payload []byte, opts loadOptions) result {
	start := time.Now()
	opCtx, cancel := context.WithTimeout(context.Background(), opts.UDPTimeout)
	defer cancel()
	if err := session.SendDatagramTo(opCtx, target, payload); err != nil {
		return result{Kind: "udp", Err: err.Error()}
	}
	if _, err := session.ReceiveDatagramFrom(opCtx); err != nil {
		return result{Kind: "udp", Err: err.Error()}
	}
	return result{OK: true, Kind: "udp", Latency: time.Since(start)}
}

func tcpOnce(session *native.Session, target string, payload []byte, opts loadOptions) result {
	start := time.Now()
	opCtx, cancel := context.WithTimeout(context.Background(), opts.TCPConnectTimeout)
	defer cancel()
	conn, err := session.DialTCP(opCtx, target)
	if err != nil {
		return result{Kind: "tcp", Err: err.Error()}
	}
	defer conn.Close()
	if len(payload) > 0 {
		if _, err := conn.Write(payload); err != nil {
			return result{Kind: "tcp", Err: err.Error()}
		}
	}
	_ = conn.SetReadDeadline(time.Now().Add(opts.TCPReadTimeout))
	buf := make([]byte, minInt(len(payload)+512, 4096))
	if _, err := conn.Read(buf); err != nil && err != io.EOF {
		return result{Kind: "tcp", Err: err.Error()}
	}
	return result{OK: true, Kind: "tcp", Latency: time.Since(start)}
}

func webOnce(session *native.Session, rawURL string, opts loadOptions) result {
	start := time.Now()
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return result{Kind: "web", Err: err.Error()}
	}
	if parsed.Scheme != "https" || parsed.Host == "" {
		return result{Kind: "web", Err: "target URL must be https"}
	}
	host := parsed.Host
	serverName := parsed.Hostname()
	target := host
	if _, _, err := net.SplitHostPort(host); err != nil {
		target = net.JoinHostPort(host, "443")
	}
	path := parsed.RequestURI()
	if path == "" {
		path = "/"
	}
	opCtx, cancel := context.WithTimeout(context.Background(), opts.TCPConnectTimeout)
	defer cancel()
	conn, err := session.DialTCP(opCtx, target)
	if err != nil {
		return result{Kind: "web", Err: err.Error()}
	}
	defer conn.Close()
	tlsConn := tls.Client(conn, &tls.Config{ServerName: serverName, MinVersion: tls.VersionTLS12})
	if err := tlsConn.SetDeadline(time.Now().Add(opts.TCPReadTimeout)); err != nil {
		return result{Kind: "web", Err: err.Error()}
	}
	if err := tlsConn.Handshake(); err != nil {
		return result{Kind: "web", Err: err.Error()}
	}
	req := fmt.Sprintf("HEAD %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: xgw-loadgen/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, parsed.Host)
	if _, err := io.WriteString(tlsConn, req); err != nil {
		return result{Kind: "web", Err: err.Error()}
	}
	line, err := bufio.NewReader(tlsConn).ReadString('\n')
	if err != nil && err != io.EOF {
		return result{Kind: "web", Err: err.Error()}
	}
	if !strings.HasPrefix(line, "HTTP/") {
		return result{Kind: "web", Err: "invalid HTTP response"}
	}
	return result{OK: true, Kind: "web", Latency: time.Since(start)}
}

func collect(profile, targetTCP, targetUDP string,
	targetURLs []string,
	concurrency int,
	duration time.Duration,
	interval time.Duration,
	opts loadOptions,
	minSuccessRate float64,
	startedAt time.Time,
	results <-chan result) summary {
	sum := summary{
		Profile:         profile,
		TargetTCP:       targetTCP,
		TargetUDP:       targetUDP,
		TargetURLs:      targetURLs,
		ModeDescription: profileDescription(profile),
		StartedAt:       startedAt.UTC().Format(time.RFC3339Nano),
		Concurrency:     concurrency,
		DurationSeconds: duration.Seconds(),
		IntervalSeconds: interval.Seconds(),
		StopMarginMs:    durationMillis(opts.StopMargin),
		TCPConnectMs:    durationMillis(opts.TCPConnectTimeout),
		TCPReadMs:       durationMillis(opts.TCPReadTimeout),
		UDPTimeoutMs:    durationMillis(opts.UDPTimeout),
		MinSuccessRate:  minSuccessRate,
		Errors:          map[string]uint64{},
		SelectedRoutes:  map[string]uint64{},
		SelectedLines:   map[string]uint64{},
	}
	latencies := make([]float64, 0)
	for r := range results {
		sum.Total++
		if r.OK {
			sum.Success++
			latencies = append(latencies, float64(r.Latency.Microseconds())/1000)
			continue
		}
		sum.Failed++
		key := r.Kind + ":" + r.Err
		if len(key) > 160 {
			key = key[:160]
		}
		sum.Errors[key]++
	}
	if sum.Total > 0 {
		sum.SuccessRate = float64(sum.Success) / float64(sum.Total)
	}
	sort.Float64s(latencies)
	sum.P50Millis = percentile(latencies, 50)
	sum.P75Millis = percentile(latencies, 75)
	sum.P90Millis = percentile(latencies, 90)
	sum.P95Millis = percentile(latencies, 95)
	sum.P99Millis = percentile(latencies, 99)
	if minSuccessRate <= 0 || minSuccessRate > 1 {
		minSuccessRate = 0.95
	}
	sum.OK = sum.Total > 0 && sum.SuccessRate >= minSuccessRate
	return sum
}

func (o *loadOptions) normalize() {
	if o.StopMargin <= 0 {
		o.StopMargin = time.Second
	}
	if o.TCPConnectTimeout <= 0 {
		o.TCPConnectTimeout = 6 * time.Second
	}
	if o.TCPReadTimeout <= 0 {
		o.TCPReadTimeout = 6 * time.Second
	}
	if o.UDPTimeout <= 0 {
		o.UDPTimeout = 4 * time.Second
	}
}

func durationMillis(d time.Duration) float64 {
	return float64(d.Microseconds()) / 1000
}

func profileDescription(profile string) string {
	switch strings.ToLower(profile) {
	case "live":
		return "continuous UDP echo workload for live-stream uplink stability"
	case "auction":
		return "small TCP burst workload for latency-sensitive bidding actions"
	case "browser":
		return "TCP connect/read/write workload for fingerprint browser sessions"
	case "web":
		return "real HTTPS HEAD workload for TikTok/YouTube-style browser reachability and tail latency"
	default:
		return "mixed live UDP and browser TCP workload"
	}
}

func parseTargetURLs(text string) []string {
	parts := strings.Split(text, ",")
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func pickTargetURL(urls []string, index int) string {
	if len(urls) == 0 {
		return "https://www.tiktok.com/"
	}
	if index < 0 {
		index = 0
	}
	return urls[index%len(urls)]
}

func percentile(values []float64, p float64) float64 {
	if len(values) == 0 {
		return 0
	}
	rank := (p / 100) * float64(len(values)-1)
	idx := int(math.Round(rank))
	if idx < 0 {
		idx = 0
	}
	if idx >= len(values) {
		idx = len(values) - 1
	}
	return values[idx]
}

func payload(workerID int, size int) []byte {
	if size <= 0 {
		size = 1
	}
	out := make([]byte, size)
	for i := range out {
		out[i] = byte('a' + (workerID+i)%26)
	}
	return out
}

func minDuration(a, b time.Duration) time.Duration {
	if a <= 0 {
		return b
	}
	if a < b {
		return a
	}
	return b
}

func cloneUintMap(in map[string]uint64) map[string]uint64 {
	out := make(map[string]uint64, len(in))
	for key, value := range in {
		out[key] = value
	}
	return out
}

func fail(prefix string, err error) {
	fmt.Fprintf(os.Stderr, "%s: %v\n", prefix, err)
	os.Exit(1)
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
