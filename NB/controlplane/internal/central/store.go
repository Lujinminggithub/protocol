package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	_ "modernc.org/sqlite"
)

var ErrConflict = errors.New("idempotency key was already used for a different operation")

type Store struct{ db *sql.DB }

type scanner interface{ Scan(...any) error }

type Line struct {
	ID               string `json:"id"`
	Name             string `json:"name"`
	Status           string `json:"status"`
	EntryRegion      string `json:"entry_region"`
	ExitRegion       string `json:"exit_region"`
	Provider         string `json:"provider"`
	CapacityMbps     int64  `json:"capacity_mbps"`
	ActiveDeployment string `json:"active_deployment"`
	Profile          string `json:"profile"`
	SecretRef        string `json:"secret_ref"`
	CreatedAt        string `json:"created_at"`
	UpdatedAt        string `json:"updated_at"`
}

type Snapshot struct {
	LineID         string          `json:"line_id"`
	NodeID         string          `json:"node_id"`
	Role           string          `json:"role"`
	WorkerID       string          `json:"worker_id"`
	ObservedAt     string          `json:"observed_at"`
	Health         string          `json:"health"`
	Deployment     string          `json:"deployment"`
	Profile        string          `json:"profile"`
	Sessions       int64           `json:"sessions"`
	ThroughputMbps float64         `json:"throughput_mbps"`
	QueueAgeP95US  float64         `json:"queue_age_p95_us"`
	EffectiveLoss  float64         `json:"effective_loss_pct"`
	FECObserve     bool            `json:"fec_observe"`
	FECActive      bool            `json:"fec_active"`
	Payload        json.RawMessage `json:"payload,omitempty"`
	ReceivedAt     string          `json:"received_at,omitempty"`
}

type Incident struct {
	ID         string          `json:"id"`
	LineID     string          `json:"line_id"`
	Severity   string          `json:"severity"`
	Status     string          `json:"status"`
	Kind       string          `json:"kind"`
	Message    string          `json:"message"`
	ObservedAt string          `json:"observed_at"`
	Payload    json.RawMessage `json:"payload,omitempty"`
}

type Operation struct {
	ID             string          `json:"id"`
	LineID         string          `json:"line_id"`
	Kind           string          `json:"kind"`
	Status         string          `json:"status"`
	RequestedBy    string          `json:"requested_by"`
	IdempotencyKey string          `json:"idempotency_key,omitempty"`
	Request        json.RawMessage `json:"request"`
	Result         json.RawMessage `json:"result,omitempty"`
	CreatedAt      string          `json:"created_at"`
	UpdatedAt      string          `json:"updated_at"`
}

type ExecutorLine struct {
	LineID     string   `json:"line_id"`
	Operations []string `json:"operations"`
	Reason     string   `json:"reason,omitempty"`
}

type Executor struct {
	WorkerID   string         `json:"worker_id"`
	Status     string         `json:"status"`
	Version    string         `json:"version"`
	Lines      []ExecutorLine `json:"lines"`
	ObservedAt string         `json:"observed_at"`
	UpdatedAt  string         `json:"updated_at,omitempty"`
	Online     bool           `json:"online"`
}

func scanOperation(row scanner, item *Operation) error {
	var request, result []byte
	err := row.Scan(&item.ID, &item.LineID, &item.Kind, &item.Status, &item.RequestedBy,
		&item.IdempotencyKey, &request, &result, &item.CreatedAt, &item.UpdatedAt)
	item.Request = json.RawMessage(request)
	item.Result = json.RawMessage(result)
	return err
}

type Dashboard struct {
	LinesTotal       int64      `json:"lines_total"`
	LinesHealthy     int64      `json:"lines_healthy"`
	LinesDegraded    int64      `json:"lines_degraded"`
	ActiveOperations int64      `json:"active_operations"`
	OpenIncidents    int64      `json:"open_incidents"`
	CapacityMbps     int64      `json:"capacity_mbps"`
	ThroughputMbps   float64    `json:"throughput_mbps"`
	Lines            []LineView `json:"lines"`
}

type LineView struct {
	Line
	Health         string  `json:"health"`
	Workers        int64   `json:"workers"`
	Sessions       int64   `json:"sessions"`
	ThroughputMbps float64 `json:"throughput_mbps"`
	QueueAgeP95US  float64 `json:"queue_age_p95_us"`
	EffectiveLoss  float64 `json:"effective_loss_pct"`
	FECObserve     bool    `json:"fec_observe"`
	FECActive      bool    `json:"fec_active"`
	LastObservedAt string  `json:"last_observed_at"`
}

func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for _, pragma := range []string{"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=5000"} {
		if _, err = db.ExecContext(ctx, pragma); err != nil {
			db.Close()
			return nil, err
		}
	}
	s := &Store{db: db}
	if err = s.migrate(ctx); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error                   { return s.db.Close() }
func (s *Store) Ping(ctx context.Context) error { return s.db.PingContext(ctx) }
func now() string                               { return time.Now().UTC().Format(time.RFC3339Nano) }

func (s *Store) migrate(ctx context.Context) error {
	_, err := s.db.ExecContext(ctx, `
CREATE TABLE IF NOT EXISTS lines (
 id TEXT PRIMARY KEY, name TEXT NOT NULL, status TEXT NOT NULL,
 entry_region TEXT NOT NULL, exit_region TEXT NOT NULL, provider TEXT NOT NULL,
 capacity_mbps INTEGER NOT NULL, active_deployment TEXT NOT NULL DEFAULT '',
 profile TEXT NOT NULL DEFAULT '', secret_ref TEXT NOT NULL DEFAULT '',
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS snapshots (
 id INTEGER PRIMARY KEY AUTOINCREMENT, line_id TEXT NOT NULL REFERENCES lines(id),
 node_id TEXT NOT NULL, role TEXT NOT NULL, worker_id TEXT NOT NULL,
 observed_at TEXT NOT NULL, health TEXT NOT NULL, deployment TEXT NOT NULL,
 profile TEXT NOT NULL, sessions INTEGER NOT NULL, throughput_mbps REAL NOT NULL,
 queue_age_p95_us REAL NOT NULL, effective_loss_pct REAL NOT NULL,
 fec_observe INTEGER NOT NULL, fec_active INTEGER NOT NULL, payload BLOB NOT NULL,
 received_at TEXT NOT NULL, UNIQUE(line_id,node_id,worker_id,observed_at)
);
CREATE TABLE IF NOT EXISTS incidents (
 id TEXT PRIMARY KEY, line_id TEXT NOT NULL REFERENCES lines(id), severity TEXT NOT NULL,
 status TEXT NOT NULL, kind TEXT NOT NULL, message TEXT NOT NULL,
 observed_at TEXT NOT NULL, payload BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS operations (
 id TEXT PRIMARY KEY, line_id TEXT NOT NULL REFERENCES lines(id), kind TEXT NOT NULL,
 status TEXT NOT NULL, requested_by TEXT NOT NULL, idempotency_key TEXT NOT NULL UNIQUE,
 request BLOB NOT NULL, result BLOB, created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS executors (
 worker_id TEXT PRIMARY KEY, status TEXT NOT NULL, version TEXT NOT NULL,
 capabilities BLOB NOT NULL, observed_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS raw_events (
 id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL, idempotency_key TEXT NOT NULL UNIQUE,
 payload BLOB NOT NULL, received_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS snapshots_latest ON snapshots(line_id,node_id,worker_id,observed_at DESC);
CREATE INDEX IF NOT EXISTS incidents_line ON incidents(line_id,status,observed_at DESC);
CREATE INDEX IF NOT EXISTS operations_ready ON operations(line_id,status,created_at);
CREATE INDEX IF NOT EXISTS raw_events_path ON raw_events(path,received_at);
`)
	return err
}

func (s *Store) UpsertLine(ctx context.Context, line Line) (Line, error) {
	stamp := now()
	_, err := s.db.ExecContext(ctx, `INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,active_deployment,profile,secret_ref,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,status=excluded.status,
 entry_region=excluded.entry_region,exit_region=excluded.exit_region,provider=excluded.provider,
 capacity_mbps=excluded.capacity_mbps,active_deployment=excluded.active_deployment,
 profile=excluded.profile,secret_ref=excluded.secret_ref,updated_at=excluded.updated_at`,
		line.ID, line.Name, line.Status, line.EntryRegion, line.ExitRegion, line.Provider,
		line.CapacityMbps, line.ActiveDeployment, line.Profile, line.SecretRef, stamp, stamp)
	if err != nil {
		return Line{}, err
	}
	return s.Line(ctx, line.ID)
}

func (s *Store) Line(ctx context.Context, id string) (Line, error) {
	var line Line
	err := s.db.QueryRowContext(ctx, `SELECT id,name,status,entry_region,exit_region,provider,capacity_mbps,
 active_deployment,profile,secret_ref,created_at,updated_at FROM lines WHERE id=?`, id).Scan(
		&line.ID, &line.Name, &line.Status, &line.EntryRegion, &line.ExitRegion, &line.Provider,
		&line.CapacityMbps, &line.ActiveDeployment, &line.Profile, &line.SecretRef, &line.CreatedAt, &line.UpdatedAt)
	return line, err
}

func (s *Store) Lines(ctx context.Context) ([]Line, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,name,status,entry_region,exit_region,provider,capacity_mbps,
 active_deployment,profile,secret_ref,created_at,updated_at FROM lines WHERE status<>'archived' ORDER BY name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Line
	for rows.Next() {
		var line Line
		if err = rows.Scan(&line.ID, &line.Name, &line.Status, &line.EntryRegion, &line.ExitRegion, &line.Provider,
			&line.CapacityMbps, &line.ActiveDeployment, &line.Profile, &line.SecretRef, &line.CreatedAt, &line.UpdatedAt); err != nil {
			return nil, err
		}
		result = append(result, line)
	}
	return result, rows.Err()
}

func (s *Store) RecordSnapshot(ctx context.Context, item Snapshot) (bool, error) {
	payload := item.Payload
	if len(payload) == 0 {
		payload = json.RawMessage(`{}`)
	}
	result, err := s.db.ExecContext(ctx, `INSERT OR IGNORE INTO snapshots
 (line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, item.LineID, item.NodeID, item.Role, item.WorkerID, item.ObservedAt,
		item.Health, item.Deployment, item.Profile, item.Sessions, item.ThroughputMbps, item.QueueAgeP95US,
		item.EffectiveLoss, item.FECObserve, item.FECActive, []byte(payload), now())
	if err != nil {
		return false, err
	}
	count, err := result.RowsAffected()
	return count > 0, err
}

func (s *Store) RecordIncident(ctx context.Context, item Incident) (bool, error) {
	payload := item.Payload
	if len(payload) == 0 {
		payload = json.RawMessage(`{}`)
	}
	result, err := s.db.ExecContext(ctx, `INSERT OR IGNORE INTO incidents
 (id,line_id,severity,status,kind,message,observed_at,payload) VALUES(?,?,?,?,?,?,?,?)`,
		item.ID, item.LineID, item.Severity, item.Status, item.Kind, item.Message, item.ObservedAt, []byte(payload))
	if err != nil {
		return false, err
	}
	count, err := result.RowsAffected()
	return count > 0, err
}

func (s *Store) Incidents(ctx context.Context, limit int) ([]Incident, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,line_id,severity,status,kind,message,observed_at,payload
 FROM incidents ORDER BY observed_at DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Incident
	for rows.Next() {
		var item Incident
		if err = rows.Scan(&item.ID, &item.LineID, &item.Severity, &item.Status, &item.Kind, &item.Message, &item.ObservedAt, &item.Payload); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) CreateOperation(ctx context.Context, operation Operation) (Operation, bool, error) {
	var existing Operation
	err := scanOperation(s.db.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at
	 FROM operations WHERE idempotency_key=?`, operation.IdempotencyKey), &existing)
	if err == nil {
		if existing.LineID != operation.LineID || existing.Kind != operation.Kind || string(existing.Request) != string(operation.Request) {
			return Operation{}, false, ErrConflict
		}
		return existing, true, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return Operation{}, false, err
	}
	stamp := now()
	operation.Status, operation.CreatedAt, operation.UpdatedAt = "queued", stamp, stamp
	_, err = s.db.ExecContext(ctx, `INSERT INTO operations
 (id,line_id,kind,status,requested_by,idempotency_key,request,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?)`, operation.ID, operation.LineID, operation.Kind, operation.Status, operation.RequestedBy,
		operation.IdempotencyKey, []byte(operation.Request), stamp, stamp)
	return operation, false, err
}

func (s *Store) Operations(ctx context.Context, lineID string, limit int) ([]Operation, error) {
	query := `SELECT id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at FROM operations`
	args := []any{}
	if lineID != "" {
		query += ` WHERE line_id=?`
		args = append(args, lineID)
	}
	query += ` ORDER BY created_at DESC LIMIT ?`
	args = append(args, limit)
	rows, err := s.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Operation
	for rows.Next() {
		var item Operation
		if err = scanOperation(rows, &item); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) ClaimOperations(ctx context.Context, lineID string, limit int) ([]Operation, error) {
	if _, err := s.db.ExecContext(ctx, `UPDATE operations SET status='dispatched',updated_at=? WHERE id IN
 (SELECT id FROM operations WHERE line_id=? AND status='queued' ORDER BY created_at LIMIT ?)`, now(), lineID, limit); err != nil {
		return nil, err
	}
	return s.operationsByStatus(ctx, lineID, "dispatched", limit)
}

func (s *Store) operationsByStatus(ctx context.Context, lineID, status string, limit int) ([]Operation, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at
 FROM operations WHERE line_id=? AND status=? ORDER BY created_at LIMIT ?`, lineID, status, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Operation
	for rows.Next() {
		var item Operation
		if err = scanOperation(rows, &item); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) CompleteOperation(ctx context.Context, id, lineID, status string, result json.RawMessage) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var kind string
	if err = tx.QueryRowContext(ctx, `SELECT kind FROM operations WHERE id=? AND line_id=?
 AND status IN ('dispatched','running')`, id, lineID).Scan(&kind); err != nil {
		return fmt.Errorf("operation is not active")
	}
	updated, err := tx.ExecContext(ctx, `UPDATE operations SET status=?,result=?,updated_at=?
 WHERE id=? AND line_id=? AND status IN ('dispatched','running')`, status, []byte(result), now(), id, lineID)
	if err != nil {
		return err
	}
	count, err := updated.RowsAffected()
	if err == nil && count != 1 {
		err = fmt.Errorf("operation is not active")
	}
	if err != nil {
		return err
	}
	if status == "succeeded" {
		var values struct {
			Deployment string `json:"deployment"`
			Profile    string `json:"profile"`
		}
		_ = json.Unmarshal(result, &values)
		switch kind {
		case "line.open", "line.upgrade", "line.rollback":
			if _, err = tx.ExecContext(ctx, `UPDATE lines SET status='active',
 active_deployment=CASE WHEN ?='' THEN active_deployment ELSE ? END,
 profile=CASE WHEN ?='' THEN profile ELSE ? END,updated_at=? WHERE id=?`,
				values.Deployment, values.Deployment, values.Profile, values.Profile, now(), lineID); err != nil {
				return err
			}
		case "line.disable":
			if _, err = tx.ExecContext(ctx, `UPDATE lines SET status='disabled',updated_at=? WHERE id=?`, now(), lineID); err != nil {
				return err
			}
		}
	}
	return tx.Commit()
}

func (s *Store) CancelOperation(ctx context.Context, id, reason string) error {
	result, _ := json.Marshal(map[string]string{"reason": reason})
	updated, err := s.db.ExecContext(ctx, `UPDATE operations SET status='cancelled',result=?,updated_at=?
 WHERE id=? AND status IN ('queued','cancelled')`, result, now(), id)
	if err != nil {
		return err
	}
	count, err := updated.RowsAffected()
	if err == nil && count != 1 {
		err = fmt.Errorf("only queued or cancelled operations can be cancelled")
	}
	return err
}

func (s *Store) RecordExecutor(ctx context.Context, item Executor) error {
	capabilities, err := json.Marshal(item.Lines)
	if err != nil {
		return err
	}
	_, err = s.db.ExecContext(ctx, `INSERT INTO executors
 (worker_id,status,version,capabilities,observed_at,updated_at) VALUES(?,?,?,?,?,?)
 ON CONFLICT(worker_id) DO UPDATE SET status=excluded.status,version=excluded.version,
 capabilities=excluded.capabilities,observed_at=excluded.observed_at,updated_at=excluded.updated_at`,
		item.WorkerID, item.Status, item.Version, capabilities, item.ObservedAt, now())
	return err
}

func (s *Store) Executors(ctx context.Context, onlineWindow time.Duration) ([]Executor, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT worker_id,status,version,capabilities,observed_at,updated_at
 FROM executors ORDER BY worker_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Executor
	cutoff := time.Now().UTC().Add(-onlineWindow)
	for rows.Next() {
		var item Executor
		var capabilities []byte
		if err = rows.Scan(&item.WorkerID, &item.Status, &item.Version, &capabilities, &item.ObservedAt, &item.UpdatedAt); err != nil {
			return nil, err
		}
		if err = json.Unmarshal(capabilities, &item.Lines); err != nil {
			return nil, err
		}
		observed, parseErr := time.Parse(time.RFC3339Nano, item.ObservedAt)
		item.Online = parseErr == nil && item.Status == "ready" && observed.After(cutoff)
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) RecordRawEvent(ctx context.Context, path, key string, payload json.RawMessage) (bool, error) {
	result, err := s.db.ExecContext(ctx, `INSERT OR IGNORE INTO raw_events
 (path,idempotency_key,payload,received_at) VALUES(?,?,?,?)`, path, key, []byte(payload), now())
	if err != nil {
		return false, err
	}
	count, err := result.RowsAffected()
	return count > 0, err
}

func (s *Store) Dashboard(ctx context.Context) (Dashboard, error) {
	lines, err := s.Lines(ctx)
	if err != nil {
		return Dashboard{}, err
	}
	view := Dashboard{Lines: make([]LineView, 0, len(lines))}
	for _, line := range lines {
		item := LineView{Line: line, Health: "unknown"}
		rows, queryErr := s.db.QueryContext(ctx, `SELECT role,health,deployment,profile,sessions,throughput_mbps,
 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,observed_at FROM snapshots WHERE line_id=?
 AND id IN (SELECT MAX(id) FROM snapshots WHERE line_id=? GROUP BY node_id,worker_id)`, line.ID, line.ID)
		if queryErr != nil {
			return Dashboard{}, queryErr
		}
		healthy := true
		for rows.Next() {
			var role, health, deployment, profile, observed string
			var sessions int64
			var throughput, queue, loss float64
			var observe, active bool
			if queryErr = rows.Scan(&role, &health, &deployment, &profile, &sessions, &throughput, &queue, &loss, &observe, &active, &observed); queryErr != nil {
				rows.Close()
				return Dashboard{}, queryErr
			}
			item.Workers++
			if role == "entry" {
				item.Sessions += sessions
				item.ThroughputMbps += throughput
			}
			if queue > item.QueueAgeP95US {
				item.QueueAgeP95US = queue
			}
			if loss > item.EffectiveLoss {
				item.EffectiveLoss = loss
			}
			item.FECObserve = item.FECObserve || observe
			item.FECActive = item.FECActive || active
			if health != "ok" {
				healthy = false
			}
			if observed > item.LastObservedAt {
				item.LastObservedAt = observed
			}
			if deployment != line.ActiveDeployment || profile != line.Profile {
				healthy = false
			}
		}
		rows.Close()
		if item.Workers > 0 && healthy {
			item.Health = "healthy"
		} else if item.Workers > 0 {
			item.Health = "degraded"
		}
		view.LinesTotal++
		view.CapacityMbps += line.CapacityMbps
		view.ThroughputMbps += item.ThroughputMbps
		if item.Health == "healthy" {
			view.LinesHealthy++
		} else {
			view.LinesDegraded++
		}
		view.Lines = append(view.Lines, item)
	}
	for query, target := range map[string]*int64{
		`SELECT COUNT(*) FROM operations WHERE status IN ('queued','dispatched','running')`: &view.ActiveOperations,
		`SELECT COUNT(*) FROM incidents WHERE status IN ('open','firing')`:                  &view.OpenIncidents,
	} {
		if err = s.db.QueryRowContext(ctx, query).Scan(target); err != nil {
			return Dashboard{}, err
		}
	}
	return view, nil
}
