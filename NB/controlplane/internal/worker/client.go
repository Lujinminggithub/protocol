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
	"time"
)

type ClientConfig struct {
	BaseURL          string
	Token            string
	Version          string
	PollEvery        time.Duration
	HeartbeatEvery   time.Duration
	OperationTimeout time.Duration
}

type operationRunner interface {
	Run(context.Context, Operation) (Result, error)
}

type Client struct {
	registry Registry
	runner   operationRunner
	cfg      ClientConfig
	http     *http.Client
}

type persistedResult struct {
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
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

func (c *Client) poll(ctx context.Context) error {
	for _, line := range c.registry.Lines {
		operations, err := c.claim(ctx, line.LineID)
		if err != nil {
			return err
		}
		for _, operation := range operations {
			resultPath := filepath.Join(c.registry.StateDir, operation.ID, "result.json")
			var persisted persistedResult
			if data, readErr := os.ReadFile(resultPath); readErr == nil && json.Unmarshal(data, &persisted) == nil {
				if err = c.complete(ctx, operation, persisted.Status, persisted.Result); err != nil {
					return err
				}
				continue
			}
			operationCtx, cancel := context.WithTimeout(ctx, c.cfg.OperationTimeout)
			result, runErr := c.runner.Run(operationCtx, operation)
			cancel()
			status := "succeeded"
			var payload any = result
			if runErr != nil {
				status = "failed"
				payload = map[string]any{"message": runErr.Error(), "log_file": result.LogFile}
			}
			encoded, marshalErr := json.Marshal(payload)
			if marshalErr != nil {
				return marshalErr
			}
			persisted = persistedResult{Status: status, Result: encoded}
			persistedData, _ := json.Marshal(persisted)
			if err = os.MkdirAll(filepath.Dir(resultPath), 0700); err != nil {
				return err
			}
			temporary := resultPath + ".new"
			if err = os.WriteFile(temporary, persistedData, 0600); err != nil {
				return err
			}
			if err = os.Rename(temporary, resultPath); err != nil {
				return err
			}
			if err = c.complete(ctx, operation, status, json.RawMessage(encoded)); err != nil {
				return err
			}
		}
	}
	return nil
}

func (c *Client) Run(ctx context.Context) error {
	if err := c.heartbeat(ctx); err != nil {
		return err
	}
	heartbeat := time.NewTicker(c.cfg.HeartbeatEvery)
	poll := time.NewTicker(c.cfg.PollEvery)
	defer heartbeat.Stop()
	defer poll.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-heartbeat.C:
			if err := c.heartbeat(ctx); err != nil {
				return err
			}
		case <-poll.C:
			if err := c.poll(ctx); err != nil {
				return err
			}
		}
	}
}
