package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type ClientConfig struct {
	BaseURL          string
	Token            string
	Version          string
	PollEvery        time.Duration
	HeartbeatEvery   time.Duration
	OperationTimeout time.Duration
	MaintenanceEvery time.Duration
	SnapshotEvery    time.Duration
	SnapshotTimeout  time.Duration
}

type operationRunner interface {
	Run(context.Context, Operation) (Result, error)
}

type Client struct {
	registry  Registry
	runner    operationRunner
	cfg       ClientConfig
	http      *http.Client
	snapshots sync.Map
}

type persistedResult struct {
	LineID       string          `json:"line_id"`
	Status       string          `json:"status"`
	Result       json.RawMessage `json:"result"`
	Acknowledged bool            `json:"acknowledged,omitempty"`
}

type eventEmitter func(context.Context, OperationEvent) error
type eventContextKey struct{}

type OperationEvent struct {
	Sequence   int            `json:"sequence"`
	Stage      string         `json:"stage"`
	Status     string         `json:"status"`
	Message    string         `json:"message"`
	Parameters map[string]any `json:"parameters,omitempty"`
}

func emitOperationEvent(ctx context.Context, event OperationEvent) error {
	if emit, ok := ctx.Value(eventContextKey{}).(eventEmitter); ok {
		return emit(ctx, event)
	}
	return nil
}

func NewClient(registry Registry, runner operationRunner, cfg ClientConfig) (*Client, error) {
	if strings.TrimSpace(cfg.BaseURL) == "" || strings.TrimSpace(cfg.Token) == "" {
		return nil, errors.New("worker base URL and token are required")
	}
	if cfg.PollEvery <= 0 {
		cfg.PollEvery = 2 * time.Second
	}
	if cfg.HeartbeatEvery <= 0 {
		cfg.HeartbeatEvery = 10 * time.Second
	}
	if cfg.OperationTimeout <= 0 {
		cfg.OperationTimeout = 45 * time.Minute
	}
	if cfg.MaintenanceEvery <= 0 {
		cfg.MaintenanceEvery = 5 * time.Minute
	}
	if cfg.SnapshotEvery <= 0 {
		cfg.SnapshotEvery = 15 * time.Second
	}
	if cfg.SnapshotTimeout <= 0 {
		cfg.SnapshotTimeout = 30 * time.Second
	}
	return &Client{registry: registry, runner: runner, cfg: cfg,
		http: &http.Client{Timeout: 20 * time.Second}}, nil
}

func (c *Client) request(ctx context.Context, method, path string, body any, output any) error {
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return err
		}
		reader = bytes.NewReader(data)
	}
	request, err := http.NewRequestWithContext(ctx, method, strings.TrimRight(c.cfg.BaseURL, "/")+path, reader)
	if err != nil {
		return err
	}
	request.Header.Set("Authorization", "Bearer "+c.cfg.Token)
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	response, err := c.http.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 2<<20))
	if err != nil {
		return err
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		var problem struct {
			Error string `json:"error"`
		}
		if json.Unmarshal(data, &problem) == nil && problem.Error != "" {
			return fmt.Errorf("central returned HTTP %d: %s", response.StatusCode, problem.Error)
		}
		return fmt.Errorf("central returned HTTP %d", response.StatusCode)
	}
	if output != nil && len(data) > 0 {
		return json.Unmarshal(data, output)
	}
	return nil
}

func (c *Client) heartbeat(ctx context.Context) error {
	type lineCapability struct {
		LineID     string   `json:"line_id"`
		Operations []string `json:"operations"`
		Reason     string   `json:"reason,omitempty"`
	}
	lines := make([]lineCapability, 0, len(c.registry.Lines))
	for _, line := range c.registry.Lines {
		lines = append(lines, lineCapability{LineID: line.LineID, Operations: line.SortedOperations(), Reason: line.DisabledReason})
	}
	if c.registry.Dynamic.Enabled {
		lines = append(lines, lineCapability{LineID: "*", Operations: append([]string(nil), c.registry.Dynamic.Operations...)})
	}
	payload := map[string]any{"worker_id": c.registry.WorkerID, "status": "ready", "version": c.cfg.Version,
		"lines": lines, "observed_at": time.Now().UTC().Format(time.RFC3339Nano)}
	return c.request(ctx, http.MethodPost, "/agent/v1/executors/heartbeat", payload, nil)
}

func (c *Client) claim(ctx context.Context, lineID string) ([]Operation, error) {
	var response struct {
		Operations []Operation `json:"operations"`
	}
	path := "/agent/v1/operations?line_id=" + url.QueryEscape(lineID) + "&limit=1"
	err := c.request(ctx, http.MethodGet, path, nil, &response)
	return response.Operations, err
}

func (c *Client) complete(ctx context.Context, operation Operation, status string, result any) error {
	payload := map[string]any{"line_id": operation.LineID, "status": status, "result": result}
	return c.request(ctx, http.MethodPost, "/agent/v1/operations/"+url.PathEscape(operation.ID)+"/result", payload, nil)
}

func retryableCompletionError(err error) bool {
	message := strings.ToLower(err.Error())
	for _, marker := range []string{"database is locked", "sqlite_busy", "http 500", "http 502", "http 503", "http 504", "timeout", "temporarily unavailable"} {
		if strings.Contains(message, marker) {
			return true
		}
	}
	return false
}

func (c *Client) completeWithRetry(ctx context.Context, operation Operation, status string, result any) error {
	var lastErr error
	for attempt := 0; attempt < 8; attempt++ {
		if attempt > 0 {
			delay := 250 * time.Millisecond * time.Duration(1<<(attempt-1))
			if delay > 8*time.Second {
				delay = 8 * time.Second
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(delay):
			}
		}
		lastErr = c.complete(ctx, operation, status, result)
		if lastErr == nil || !retryableCompletionError(lastErr) {
			return lastErr
		}
	}
	return lastErr
}

func acknowledgePersistedResult(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var persisted persistedResult
	if err = json.Unmarshal(data, &persisted); err != nil {
		return err
	}
	persisted.Acknowledged = true
	encoded, err := json.Marshal(persisted)
	if err != nil {
		return err
	}
	return writePrivateFile(path, encoded)
}

func (c *Client) retryPersistedResults(ctx context.Context) error {
	paths, err := filepath.Glob(filepath.Join(c.registry.StateDir, "*", "result.json"))
	if err != nil {
		return err
	}
	for _, resultPath := range paths {
		data, readErr := os.ReadFile(resultPath)
		if readErr != nil {
			continue
		}
		var persisted persistedResult
		if json.Unmarshal(data, &persisted) != nil || persisted.Acknowledged || persisted.LineID == "" || persisted.Status == "" {
			continue
		}
		operation := Operation{ID: filepath.Base(filepath.Dir(resultPath)), LineID: persisted.LineID}
		if err = c.completeWithRetry(ctx, operation, persisted.Status, persisted.Result); err != nil {
			return fmt.Errorf("persisted result %s: %w", operation.ID, err)
		}
		if err = acknowledgePersistedResult(resultPath); err != nil {
			return fmt.Errorf("acknowledge persisted result %s: %w", operation.ID, err)
		}
	}
	return nil
}

func (c *Client) syncClientConfigs(ctx context.Context) error {
	paths := map[string]string{}
	for _, line := range c.registry.Lines {
		if line.StateDir != "" {
			paths[line.LineID] = filepath.Join(line.StateDir, "provision", line.LineID, "client.json")
		}
	}
	dynamic, _ := filepath.Glob(filepath.Join(c.registry.StateDir, "lines", "*", "provision", "*", "client.json"))
	for _, path := range dynamic {
		lineID := filepath.Base(filepath.Dir(path))
		if lineID != "" && !strings.ContainsAny(lineID, `/\\`) {
			paths[lineID] = path
		}
	}
	failures := make([]string, 0)
	for lineID, path := range paths {
		clientURL, err := clientURLFromFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			failures = append(failures, fmt.Sprintf("line %s: invalid local client config", lineID))
			continue
		}
		payload := map[string]string{"client_url": clientURL}
		if err = c.request(ctx, http.MethodPost, "/agent/v1/lines/"+url.PathEscape(lineID)+"/client-config", payload, nil); err != nil {
			failures = append(failures, fmt.Sprintf("line %s: central rejected client config", lineID))
		}
	}
	if len(failures) > 0 {
		return errors.New(strings.Join(failures, "; "))
	}
	return nil
}

func (c *Client) event(ctx context.Context, operation Operation, event OperationEvent) error {
	return c.request(ctx, http.MethodPost, "/agent/v1/operations/"+url.PathEscape(operation.ID)+"/events", event, nil)
}

func (c *Client) snapshotLines(ctx context.Context) []LineSpec {
	lines := append([]LineSpec(nil), c.registry.Lines...)
	resolver, ok := c.runner.(interface {
		resolveSnapshotLine(dynamicPlan) (LineSpec, error)
	})
	if !ok || !c.registry.Dynamic.Enabled {
		return lines
	}
	var response struct {
		Plans []dynamicPlan `json:"plans"`
	}
	if err := c.request(ctx, http.MethodGet, "/agent/v1/line-plans", nil, &response); err != nil {
		fmt.Fprintf(os.Stderr, "nb-web-worker dynamic snapshot plans unavailable: %v\n", err)
		return lines
	}
	seen := map[string]bool{}
	for _, line := range lines {
		seen[line.LineID] = true
	}
	for _, plan := range response.Plans {
		if seen[plan.LineID] || !contains(plan.ResourceGroup, c.registry.Dynamic.ResourceGroups) {
			continue
		}
		line, err := resolver.resolveSnapshotLine(plan)
		if err != nil {
			fmt.Fprintf(os.Stderr, "nb-web-worker dynamic snapshot plan rejected line=%s: %v\n", plan.LineID, err)
			continue
		}
		seen[line.LineID] = true
		lines = append(lines, line)
	}
	return lines
}

func (c *Client) collectSnapshots(ctx context.Context) {
	collector, ok := c.runner.(interface {
		CollectSnapshots(context.Context, LineSpec) ([]Snapshot, error)
	})
	if !ok {
		return
	}
	type collected struct {
		lineID    string
		snapshots []Snapshot
		err       error
	}
	lines := c.snapshotLines(ctx)
	results := make(chan collected, len(lines))
	var group sync.WaitGroup
	for _, line := range lines {
		line := line
		if _, loaded := c.snapshots.LoadOrStore(line.LineID, struct{}{}); loaded {
			continue
		}
		group.Add(1)
		go func() {
			defer group.Done()
			defer c.snapshots.Delete(line.LineID)
			lineContext, cancel := context.WithTimeout(ctx, c.cfg.SnapshotTimeout)
			defer cancel()
			snapshots, err := collector.CollectSnapshots(lineContext, line)
			results <- collected{lineID: line.LineID, snapshots: snapshots, err: err}
		}()
	}
	go func() {
		group.Wait()
		close(results)
	}()
	for result := range results {
		if result.err != nil {
			fmt.Fprintf(os.Stderr, "nb-web-worker snapshot collection failed line=%s: %v\n", result.lineID, result.err)
			continue
		}
		for _, snapshot := range result.snapshots {
			if err := c.request(ctx, http.MethodPost, "/agent/v1/snapshots", snapshot, nil); err != nil {
				fmt.Fprintf(os.Stderr, "nb-web-worker snapshot delivery failed line=%s node=%s: %v\n", result.lineID, snapshot.NodeID, err)
			}
		}
	}
}

func (c *Client) poll(ctx context.Context) error {
	if err := c.retryPersistedResults(ctx); err != nil {
		return err
	}
	lines := append([]LineSpec(nil), c.registry.Lines...)
	if c.registry.Dynamic.Enabled {
		lines = append(lines, LineSpec{LineID: "*"})
	}
	for _, line := range lines {
		operations, err := c.claim(ctx, line.LineID)
		if err != nil {
			return err
		}
		for _, operation := range operations {
			resultPath := filepath.Join(c.registry.StateDir, operation.ID, "result.json")
			var persisted persistedResult
			if data, readErr := os.ReadFile(resultPath); readErr == nil && json.Unmarshal(data, &persisted) == nil && !persisted.Acknowledged {
				if err = c.completeWithRetry(ctx, operation, persisted.Status, persisted.Result); err != nil {
					return err
				}
				if err = acknowledgePersistedResult(resultPath); err != nil {
					return err
				}
				continue
			}
			operationCtx, cancel := context.WithTimeout(ctx, c.cfg.OperationTimeout)
			operationCtx = context.WithValue(operationCtx, eventContextKey{}, eventEmitter(func(eventCtx context.Context, event OperationEvent) error { return c.event(eventCtx, operation, event) }))
			result, runErr := c.runner.Run(operationCtx, operation)
			cancel()
			status := "succeeded"
			var payload any = result
			if runErr != nil {
				status = "failed"
				values := map[string]any{}
				if encodedResult, encodeErr := json.Marshal(result); encodeErr == nil {
					_ = json.Unmarshal(encodedResult, &values)
				}
				values["message"], values["log_file"] = runErr.Error(), result.LogFile
				payload = values
			}
			encoded, marshalErr := json.Marshal(payload)
			if marshalErr != nil {
				return marshalErr
			}
			persisted = persistedResult{LineID: operation.LineID, Status: status, Result: encoded}
			persistedData, _ := json.Marshal(persisted)
			if err = writePrivateFile(resultPath, persistedData); err != nil {
				return err
			}
			if err = c.completeWithRetry(ctx, operation, status, json.RawMessage(encoded)); err != nil {
				return err
			}
			if err = acknowledgePersistedResult(resultPath); err != nil {
				return err
			}
		}
	}
	return nil
}

func (c *Client) runHeartbeat(ctx context.Context) {
	ticker := time.NewTicker(c.cfg.HeartbeatEvery)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if err := c.heartbeat(ctx); err != nil {
				fmt.Fprintf(os.Stderr, "nb-web-worker heartbeat failed: %v; retrying\n", err)
				continue
			}
			if err := c.syncInventory(ctx); err != nil {
				fmt.Fprintf(os.Stderr, "nb-web-worker %v\n", err)
			}
		}
	}
}

func (c *Client) Run(ctx context.Context) error {
	if err := c.heartbeat(ctx); err != nil {
		return err
	}
	if err := c.syncInventory(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "nb-web-worker %v\n", err)
	}
	if err := c.syncClientConfigs(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "nb-web-worker %v\n", err)
	}
	go c.collectSnapshots(ctx)
	go c.runHeartbeat(ctx)
	poll := time.NewTicker(c.cfg.PollEvery)
	maintenance := time.NewTicker(c.cfg.MaintenanceEvery)
	snapshots := time.NewTicker(c.cfg.SnapshotEvery)
	defer poll.Stop()
	defer maintenance.Stop()
	defer snapshots.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-poll.C:
			if err := c.poll(ctx); err != nil {
				return err
			}
		case <-maintenance.C:
			if runner, ok := c.runner.(interface{ Maintain(context.Context) error }); ok {
				if err := runner.Maintain(ctx); err != nil {
					fmt.Fprintf(os.Stderr, "nb-web-worker maintenance failed: %v\n", err)
				}
			}
		case <-snapshots.C:
			go c.collectSnapshots(ctx)
		}
	}
}
