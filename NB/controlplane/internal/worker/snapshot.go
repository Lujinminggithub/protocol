package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
)

type Snapshot struct {
	LineID         string         `json:"line_id"`
	NodeID         string         `json:"node_id"`
	Role           string         `json:"role"`
	WorkerID       string         `json:"worker_id"`
	ObservedAt     string         `json:"observed_at"`
	Health         string         `json:"health"`
	Deployment     string         `json:"deployment"`
	Profile        string         `json:"profile"`
	Sessions       int64          `json:"sessions"`
	ThroughputMbps float64        `json:"throughput_mbps"`
	QueueAgeP95US  float64        `json:"queue_age_p95_us"`
	EffectiveLoss  float64        `json:"effective_loss_pct"`
	FECObserve     bool           `json:"fec_observe"`
	FECActive      bool           `json:"fec_active"`
	Payload        map[string]any `json:"payload,omitempty"`
}

func (r *Runner) CollectSnapshots(ctx context.Context, line LineSpec) ([]Snapshot, error) {
	environment, err := r.environment(line)
	if err != nil {
		return nil, err
	}
	var captured bytes.Buffer
	step := commandStep{Name: r.registry.Python, Stage: "snapshot", Args: []string{
		filepath.Join(r.registry.Root, "tools", "worker_snapshot.py"), "--line-id", line.LineID,
		"--state-file", filepath.Join(r.registry.StateDir, "snapshot-counters", line.LineID+".json")}}
	if err = r.execute(ctx, step, environment, &captured); err != nil {
		return nil, err
	}
	const marker = "SNAPSHOTS_JSON="
	index := strings.LastIndex(captured.String(), marker)
	if index < 0 {
		return nil, errors.New("snapshot collector did not return a payload")
	}
	lineText := strings.SplitN(captured.String()[index+len(marker):], "\n", 2)[0]
	var snapshots []Snapshot
	if err = json.Unmarshal([]byte(lineText), &snapshots); err != nil {
		return nil, err
	}
	return snapshots, nil
}
