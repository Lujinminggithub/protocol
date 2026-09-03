package central

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"sync"
	"time"

	_ "github.com/go-sql-driver/mysql"
	_ "modernc.org/sqlite"
)

var ErrConflict = errors.New("idempotency key was already used for a different operation")

type Store struct {
	db               *sql.DB
	telemetryDB      *sql.DB
	operationMu      sync.Mutex
	specMu           sync.Mutex
	writeMu          sync.Mutex
	telemetryWriteMu sync.Mutex
	dialect          string
}

const (
	controlDatabaseMaxBytes   int64 = 128 << 20
	telemetryDatabaseMaxBytes int64 = 512 << 20
	controlJournalMaxBytes    int64 = 16 << 20
	telemetryJournalMaxBytes  int64 = 64 << 20
)

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
	UpstreamMbps   float64         `json:"upstream_mbps"`
	DownstreamMbps float64         `json:"downstream_mbps"`
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
	UpstreamMbps   float64 `json:"upstream_mbps"`
	DownstreamMbps float64 `json:"downstream_mbps"`
	QueueAgeP95US  float64 `json:"queue_age_p95_us"`
	EffectiveLoss  float64 `json:"effective_loss_pct"`
	FECObserve     bool    `json:"fec_observe"`
	FECActive      bool    `json:"fec_active"`
	LastObservedAt string  `json:"last_observed_at"`
}

func Open(path string) (*Store, error) {
	db, err := openSQLite(path, controlDatabaseMaxBytes, controlJournalMaxBytes)
	if err != nil {
		return nil, err
	}
	telemetryPath := strings.TrimSuffix(path, filepath.Ext(path)) + "-telemetry" + filepath.Ext(path)
	telemetryDB, err := openSQLite(telemetryPath, telemetryDatabaseMaxBytes, telemetryJournalMaxBytes)
	if err != nil {
		db.Close()
		return nil, err
	}
	s := &Store{db: db, telemetryDB: telemetryDB, dialect: "sqlite"}
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	if err = s.migrate(ctx); err == nil {
		err = s.migrateTelemetry(ctx)
	}
	if err != nil {
		telemetryDB.Close()
		db.Close()
		return nil, err
	}
	return s, nil
}

func OpenMySQL(dsn, telemetryPath string) (*Store, error) {
	if strings.TrimSpace(dsn) == "" {
		return nil, errors.New("MySQL DSN is required")
	}
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(16)
	db.SetMaxIdleConns(8)
	db.SetConnMaxLifetime(3 * time.Minute)
	telemetryDB, err := openSQLite(telemetryPath, telemetryDatabaseMaxBytes, telemetryJournalMaxBytes)
	if err != nil {
		db.Close()
		return nil, err
	}
	s := &Store{db: db, telemetryDB: telemetryDB, dialect: "mysql"}
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	if err = db.PingContext(ctx); err == nil {
		err = s.migrateMySQL(ctx)
	}
	if err == nil {
		err = s.migrateTelemetry(ctx)
	}
	if err != nil {
		telemetryDB.Close()
		db.Close()
		return nil, err
	}
	return s, nil
}

func openSQLite(path string, maxBytes, journalMaxBytes int64) (*sql.DB, error) {
	separator := "?"
	if strings.Contains(path, "?") {
		separator = "&"
	}
	dsn := path + separator + "_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// WAL permits readers to make progress while the worker records snapshots.
	// A single connection lets one dashboard query stall every API request.
	db.SetMaxOpenConns(8)
	db.SetMaxIdleConns(8)
	// A production database can be several gigabytes. WAL setup and additive
	// migrations must have enough time to finish during a controlled restart;
	// the HTTP listener is not started until this initialization succeeds.
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	for _, pragma := range []string{"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA foreign_keys=ON",
		"PRAGMA busy_timeout=5000", "PRAGMA auto_vacuum=INCREMENTAL", "PRAGMA wal_autocheckpoint=1000",
		fmt.Sprintf("PRAGMA journal_size_limit=%d", journalMaxBytes)} {
		if _, err = db.ExecContext(ctx, pragma); err != nil {
			db.Close()
			return nil, err
		}
	}
	var pageSize int64
	if err = db.QueryRowContext(ctx, `PRAGMA page_size`).Scan(&pageSize); err != nil {
		db.Close()
		return nil, err
	}
	if _, err = db.ExecContext(ctx, fmt.Sprintf("PRAGMA max_page_count=%d", maxBytes/pageSize)); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

func (s *Store) Close() error {
	telemetryErr := s.telemetryDB.Close()
	controlErr := s.db.Close()
	if controlErr != nil {
		return controlErr
	}
	return telemetryErr
}

func (s *Store) Ping(ctx context.Context) error {
	if err := s.db.PingContext(ctx); err != nil {
		return err
	}
	return s.telemetryDB.PingContext(ctx)
}
func now() string { return time.Now().UTC().Format(time.RFC3339Nano) }

func (s *Store) lockWrite() func() {
	s.writeMu.Lock()
	return s.writeMu.Unlock
}

func (s *Store) lockTelemetryWrite() func() {
	s.telemetryWriteMu.Lock()
	return s.telemetryWriteMu.Unlock
}

func (s *Store) migrateTelemetry(ctx context.Context) error {
	_, err := s.telemetryDB.ExecContext(ctx, `
CREATE TABLE IF NOT EXISTS snapshots (
 id INTEGER PRIMARY KEY AUTOINCREMENT, line_id TEXT NOT NULL,
 node_id TEXT NOT NULL, role TEXT NOT NULL, worker_id TEXT NOT NULL,
 observed_at TEXT NOT NULL, health TEXT NOT NULL, deployment TEXT NOT NULL,
 profile TEXT NOT NULL, sessions INTEGER NOT NULL, throughput_mbps REAL NOT NULL,
 upstream_mbps REAL NOT NULL DEFAULT 0, downstream_mbps REAL NOT NULL DEFAULT 0,
 queue_age_p95_us REAL NOT NULL, effective_loss_pct REAL NOT NULL,
 fec_observe INTEGER NOT NULL, fec_active INTEGER NOT NULL, payload BLOB NOT NULL DEFAULT '{}',
 received_at TEXT NOT NULL, UNIQUE(line_id,node_id,worker_id,observed_at)
);
CREATE TABLE IF NOT EXISTS latest_snapshots (
 line_id TEXT NOT NULL, node_id TEXT NOT NULL, observed_at TEXT NOT NULL,
 snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
 PRIMARY KEY(line_id,node_id)
);
CREATE TABLE IF NOT EXISTS latest_snapshot_payloads (
 line_id TEXT NOT NULL, node_id TEXT NOT NULL, observed_at TEXT NOT NULL,
 payload BLOB NOT NULL, PRIMARY KEY(line_id,node_id)
);
CREATE TABLE IF NOT EXISTS traffic_rollups (
 line_id TEXT NOT NULL, node_id TEXT NOT NULL, role TEXT NOT NULL, worker_id TEXT NOT NULL,
 resolution_s INTEGER NOT NULL CHECK(resolution_s IN (60,300,3600)),
 bucket_at TEXT NOT NULL, sample_count INTEGER NOT NULL,
 upstream_sum REAL NOT NULL, downstream_sum REAL NOT NULL,
 sessions_max INTEGER NOT NULL, queue_age_max_us REAL NOT NULL,
 effective_loss_max_pct REAL NOT NULL, health_rank INTEGER NOT NULL,
 deployment TEXT NOT NULL, profile TEXT NOT NULL, last_observed_at TEXT NOT NULL,
 PRIMARY KEY(line_id,node_id,worker_id,resolution_s,bucket_at)
);
CREATE TABLE IF NOT EXISTS maintenance_jobs (name TEXT PRIMARY KEY, completed_at TEXT NOT NULL);
CREATE TRIGGER IF NOT EXISTS snapshots_update_latest AFTER INSERT ON snapshots BEGIN
 INSERT INTO latest_snapshots(line_id,node_id,observed_at,snapshot_id)
 VALUES(NEW.line_id,NEW.node_id,NEW.observed_at,NEW.id)
 ON CONFLICT(line_id,node_id) DO UPDATE SET
  observed_at=excluded.observed_at,snapshot_id=excluded.snapshot_id
 WHERE excluded.observed_at>latest_snapshots.observed_at OR
  (excluded.observed_at=latest_snapshots.observed_at AND excluded.snapshot_id>latest_snapshots.snapshot_id);
END;
CREATE INDEX IF NOT EXISTS snapshots_latest ON snapshots(line_id,node_id,worker_id,observed_at DESC);
CREATE INDEX IF NOT EXISTS snapshots_latest_node ON snapshots(line_id,node_id,observed_at DESC,id DESC);
CREATE INDEX IF NOT EXISTS snapshots_retention ON snapshots(observed_at,id);
CREATE INDEX IF NOT EXISTS traffic_rollups_range ON traffic_rollups(line_id,resolution_s,bucket_at);
CREATE INDEX IF NOT EXISTS traffic_rollups_retention ON traffic_rollups(resolution_s,bucket_at);
`)
	return err
}

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
 upstream_mbps REAL NOT NULL DEFAULT 0, downstream_mbps REAL NOT NULL DEFAULT 0,
 queue_age_p95_us REAL NOT NULL, effective_loss_pct REAL NOT NULL,
 fec_observe INTEGER NOT NULL, fec_active INTEGER NOT NULL, payload BLOB NOT NULL,
 received_at TEXT NOT NULL, UNIQUE(line_id,node_id,worker_id,observed_at)
);
CREATE TABLE IF NOT EXISTS latest_snapshots (
 line_id TEXT NOT NULL REFERENCES lines(id) ON DELETE CASCADE,
 node_id TEXT NOT NULL, observed_at TEXT NOT NULL,
 snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
 PRIMARY KEY(line_id,node_id)
);
CREATE TABLE IF NOT EXISTS traffic_rollups (
 line_id TEXT NOT NULL REFERENCES lines(id) ON DELETE CASCADE,
 node_id TEXT NOT NULL, role TEXT NOT NULL, worker_id TEXT NOT NULL,
 resolution_s INTEGER NOT NULL CHECK(resolution_s IN (60,300)),
 bucket_at TEXT NOT NULL, sample_count INTEGER NOT NULL,
 upstream_sum REAL NOT NULL, downstream_sum REAL NOT NULL,
 sessions_max INTEGER NOT NULL, queue_age_max_us REAL NOT NULL,
 effective_loss_max_pct REAL NOT NULL, health_rank INTEGER NOT NULL,
 deployment TEXT NOT NULL, profile TEXT NOT NULL, last_observed_at TEXT NOT NULL,
 PRIMARY KEY(line_id,node_id,worker_id,resolution_s,bucket_at)
);
CREATE TABLE IF NOT EXISTS maintenance_jobs (
 name TEXT PRIMARY KEY, completed_at TEXT NOT NULL
);
CREATE TRIGGER IF NOT EXISTS snapshots_update_latest AFTER INSERT ON snapshots BEGIN
 INSERT INTO latest_snapshots(line_id,node_id,observed_at,snapshot_id)
 VALUES(NEW.line_id,NEW.node_id,NEW.observed_at,NEW.id)
 ON CONFLICT(line_id,node_id) DO UPDATE SET
  observed_at=excluded.observed_at,snapshot_id=excluded.snapshot_id
 WHERE excluded.observed_at>latest_snapshots.observed_at OR
  (excluded.observed_at=latest_snapshots.observed_at AND excluded.snapshot_id>latest_snapshots.snapshot_id);
END;
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
CREATE TABLE IF NOT EXISTS operation_cleanups (
 operation_id TEXT PRIMARY KEY REFERENCES operations(id) ON DELETE CASCADE,
 cleaned_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS executors (
 worker_id TEXT PRIMARY KEY, status TEXT NOT NULL, version TEXT NOT NULL,
 capabilities BLOB NOT NULL, observed_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
 id TEXT PRIMARY KEY, name TEXT NOT NULL, status TEXT NOT NULL,
 host TEXT NOT NULL, ssh_port INTEGER NOT NULL, ssh_user TEXT NOT NULL,
	ssh_host_key TEXT NOT NULL DEFAULT '', ssh_host_key_type TEXT NOT NULL DEFAULT '',
	ssh_host_key_sha256 TEXT NOT NULL DEFAULT '', ssh_host_key_status TEXT NOT NULL DEFAULT 'pending',
	ssh_host_key_confirmed_at TEXT NOT NULL DEFAULT '',
 private_ip TEXT NOT NULL DEFAULT '', region TEXT NOT NULL DEFAULT '',
 provider TEXT NOT NULL DEFAULT '', os TEXT NOT NULL DEFAULT '', arch TEXT NOT NULL DEFAULT '',
 secret_ref TEXT NOT NULL DEFAULT '', labels BLOB NOT NULL DEFAULT '{}',
 last_health TEXT NOT NULL DEFAULT 'unknown', last_seen_at TEXT NOT NULL DEFAULT '',
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS topology_layouts (
 device_id TEXT PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
 x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,
 updated_by TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS line_specs (
 line_id TEXT PRIMARY KEY REFERENCES lines(id) ON DELETE CASCADE,
 resource_group TEXT NOT NULL, instance_id TEXT NOT NULL,
 bandwidth_mbps INTEGER NOT NULL, upstream_mbps INTEGER NOT NULL DEFAULT 0,
 downstream_mbps INTEGER NOT NULL DEFAULT 0, socks_port INTEGER NOT NULL,
 udp_port_min INTEGER NOT NULL, udp_port_max INTEGER NOT NULL,
 relay_port INTEGER NOT NULL, exit_port INTEGER NOT NULL,
 exit_bind_ip TEXT NOT NULL DEFAULT '',
 dns_servers BLOB NOT NULL DEFAULT '["1.1.1.1","8.8.8.8"]',
 whitelist BLOB NOT NULL, build_mode TEXT NOT NULL, artifact_ref TEXT NOT NULL,
 source_ref TEXT NOT NULL, srs_ref TEXT NOT NULL, jump_policy TEXT NOT NULL,
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS line_nodes (
 line_id TEXT NOT NULL REFERENCES lines(id) ON DELETE CASCADE,
 device_id TEXT NOT NULL REFERENCES devices(id), role TEXT NOT NULL,
 ordinal INTEGER NOT NULL, next_hop_device_id TEXT NOT NULL DEFAULT '',
 jump_candidates BLOB NOT NULL DEFAULT '[]', config BLOB NOT NULL DEFAULT '{}',
 PRIMARY KEY(line_id,role,ordinal)
);
CREATE TABLE IF NOT EXISTS operation_events (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 operation_id TEXT NOT NULL REFERENCES operations(id) ON DELETE CASCADE,
 sequence INTEGER NOT NULL, stage TEXT NOT NULL, status TEXT NOT NULL,
 message TEXT NOT NULL, parameters BLOB NOT NULL DEFAULT '{}', created_at TEXT NOT NULL,
 UNIQUE(operation_id,sequence)
);
CREATE TABLE IF NOT EXISTS raw_events (
 id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL, idempotency_key TEXT NOT NULL UNIQUE,
 payload BLOB NOT NULL, received_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS line_deletion_audit (
 id INTEGER PRIMARY KEY AUTOINCREMENT, line_id TEXT NOT NULL, line_name TEXT NOT NULL,
 requested_by TEXT NOT NULL, reason TEXT NOT NULL, snapshot BLOB NOT NULL,
 deleted_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS transport_generations (
 line_id TEXT PRIMARY KEY REFERENCES lines(id) ON DELETE CASCADE,
 current_generation INTEGER NOT NULL CHECK(current_generation > 0), updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS users (
 id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE,
 password_hash BLOB NOT NULL, must_change_password INTEGER NOT NULL DEFAULT 1,
 status TEXT NOT NULL DEFAULT 'active', created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS user_sessions (
 token_hash BLOB PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 expires_at TEXT NOT NULL, created_at TEXT NOT NULL, last_seen_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS snapshots_latest ON snapshots(line_id,node_id,worker_id,observed_at DESC);
CREATE INDEX IF NOT EXISTS snapshots_latest_node ON snapshots(line_id,node_id,observed_at DESC,id DESC);
CREATE INDEX IF NOT EXISTS traffic_rollups_range ON traffic_rollups(line_id,resolution_s,bucket_at);
CREATE INDEX IF NOT EXISTS incidents_line ON incidents(line_id,status,observed_at DESC);
CREATE INDEX IF NOT EXISTS operations_ready ON operations(line_id,status,created_at);
CREATE INDEX IF NOT EXISTS raw_events_path ON raw_events(path,received_at);
CREATE INDEX IF NOT EXISTS devices_status ON devices(status,region,name);
CREATE INDEX IF NOT EXISTS line_nodes_device ON line_nodes(device_id,line_id);
CREATE INDEX IF NOT EXISTS operation_events_order ON operation_events(operation_id,sequence);
CREATE INDEX IF NOT EXISTS line_deletion_audit_line ON line_deletion_audit(line_id,deleted_at DESC);
CREATE INDEX IF NOT EXISTS user_sessions_user ON user_sessions(user_id,expires_at);
`)
	if err != nil {
		return err
	}
	rows, err := s.db.QueryContext(ctx, `PRAGMA table_info(line_specs)`)
	if err != nil {
		return err
	}
	foundExitBindIP, foundUpstreamMbps, foundDownstreamMbps, foundDNSServers := false, false, false, false
	for rows.Next() {
		var cid, notNull, primaryKey int
		var name, kind string
		var defaultValue any
		if err = rows.Scan(&cid, &name, &kind, &notNull, &defaultValue, &primaryKey); err != nil {
			_ = rows.Close()
			return err
		}
		foundExitBindIP = foundExitBindIP || name == "exit_bind_ip"
		foundUpstreamMbps = foundUpstreamMbps || name == "upstream_mbps"
		foundDownstreamMbps = foundDownstreamMbps || name == "downstream_mbps"
		foundDNSServers = foundDNSServers || name == "dns_servers"
	}
	if err = rows.Close(); err != nil {
		return err
	}
	if !foundExitBindIP {
		_, err = s.db.ExecContext(ctx, `ALTER TABLE line_specs ADD COLUMN exit_bind_ip TEXT NOT NULL DEFAULT ''`)
		if err != nil {
			return err
		}
	}
	if !foundUpstreamMbps {
		if _, err = s.db.ExecContext(ctx, `ALTER TABLE line_specs ADD COLUMN upstream_mbps INTEGER NOT NULL DEFAULT 0`); err != nil {
			return err
		}
	}
	if !foundDownstreamMbps {
		if _, err = s.db.ExecContext(ctx, `ALTER TABLE line_specs ADD COLUMN downstream_mbps INTEGER NOT NULL DEFAULT 0`); err != nil {
			return err
		}
	}
	if !foundDNSServers {
		if _, err = s.db.ExecContext(ctx, `ALTER TABLE line_specs ADD COLUMN dns_servers BLOB NOT NULL DEFAULT '["1.1.1.1","8.8.8.8"]'`); err != nil {
			return err
		}
	}
	if _, err = s.db.ExecContext(ctx, `UPDATE line_specs SET
	 upstream_mbps=CASE WHEN upstream_mbps<=0 THEN bandwidth_mbps ELSE upstream_mbps END,
	 downstream_mbps=CASE WHEN downstream_mbps<=0 THEN bandwidth_mbps ELSE downstream_mbps END`); err != nil {
		return err
	}
	snapshotColumns := map[string]bool{}
	snapshotRows, snapshotErr := s.db.QueryContext(ctx, `PRAGMA table_info(snapshots)`)
	if snapshotErr != nil {
		return snapshotErr
	}
	for snapshotRows.Next() {
		var cid, notNull, primaryKey int
		var name, kind string
		var defaultValue any
		if snapshotErr = snapshotRows.Scan(&cid, &name, &kind, &notNull, &defaultValue, &primaryKey); snapshotErr != nil {
			_ = snapshotRows.Close()
			return snapshotErr
		}
		snapshotColumns[name] = true
	}
	if snapshotErr = snapshotRows.Close(); snapshotErr != nil {
		return snapshotErr
	}
	for _, name := range []string{"upstream_mbps", "downstream_mbps"} {
		if !snapshotColumns[name] {
			if _, snapshotErr = s.db.ExecContext(ctx, `ALTER TABLE snapshots ADD COLUMN `+name+` REAL NOT NULL DEFAULT 0`); snapshotErr != nil {
				return snapshotErr
			}
		}
	}
	deviceColumns := map[string]bool{}
	deviceRows, deviceErr := s.db.QueryContext(ctx, `PRAGMA table_info(devices)`)
	if deviceErr != nil {
		return deviceErr
	}
	for deviceRows.Next() {
		var cid, notNull, primaryKey int
		var name, kind string
		var defaultValue any
		if deviceErr = deviceRows.Scan(&cid, &name, &kind, &notNull, &defaultValue, &primaryKey); deviceErr != nil {
			_ = deviceRows.Close()
			return deviceErr
		}
		deviceColumns[name] = true
	}
	if deviceErr = deviceRows.Close(); deviceErr != nil {
		return deviceErr
	}
	for name, definition := range map[string]string{
		"ssh_host_key":              "TEXT NOT NULL DEFAULT ''",
		"ssh_host_key_type":         "TEXT NOT NULL DEFAULT ''",
		"ssh_host_key_sha256":       "TEXT NOT NULL DEFAULT ''",
		"ssh_host_key_status":       "TEXT NOT NULL DEFAULT 'pending'",
		"ssh_host_key_confirmed_at": "TEXT NOT NULL DEFAULT ''",
	} {
		if !deviceColumns[name] {
			if _, deviceErr = s.db.ExecContext(ctx, `ALTER TABLE devices ADD COLUMN `+name+` `+definition); deviceErr != nil {
				return deviceErr
			}
		}
	}
	// Older workers represented a role-wide collection failure as a fake node.
	// Remove those placeholders so they cannot degrade an otherwise healthy line.
	_, err = s.db.ExecContext(ctx, `DELETE FROM snapshots
 WHERE worker_id='collector' AND health='down' AND node_id LIKE '%-collector'`)
	if err != nil {
		return err
	}
	var hasSnapshots, hasLatest int
	if err = s.db.QueryRowContext(ctx, `SELECT EXISTS(SELECT 1 FROM snapshots LIMIT 1),
	 EXISTS(SELECT 1 FROM latest_snapshots LIMIT 1)`).Scan(&hasSnapshots, &hasLatest); err != nil {
		return err
	}
	if hasSnapshots == 1 && hasLatest == 0 {
		_, err = s.db.ExecContext(ctx, `INSERT INTO latest_snapshots(line_id,node_id,observed_at,snapshot_id)
		 SELECT line_id,node_id,observed_at,id FROM (
		  SELECT line_id,node_id,observed_at,id,
		   ROW_NUMBER() OVER (PARTITION BY line_id,node_id ORDER BY observed_at DESC,id DESC) AS rn
		  FROM snapshots
		 ) WHERE rn=1`)
	}
	return err
}

func (s *Store) UpsertLine(ctx context.Context, line Line) (Line, error) {
	stamp := now()
	query := s.controlSQL(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,active_deployment,profile,secret_ref,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,status=excluded.status,
 entry_region=excluded.entry_region,exit_region=excluded.exit_region,provider=excluded.provider,
 capacity_mbps=excluded.capacity_mbps,active_deployment=excluded.active_deployment,
	 profile=excluded.profile,secret_ref=excluded.secret_ref,updated_at=excluded.updated_at`, `INSERT INTO `+s.linesTable()+`
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,active_deployment,profile,secret_ref,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE name=VALUES(name),status=VALUES(status),
 entry_region=VALUES(entry_region),exit_region=VALUES(exit_region),provider=VALUES(provider),
 capacity_mbps=VALUES(capacity_mbps),active_deployment=VALUES(active_deployment),profile=VALUES(profile),
 secret_ref=VALUES(secret_ref),updated_at=VALUES(updated_at)`)
	_, err := s.db.ExecContext(ctx, query,
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
 active_deployment,profile,secret_ref,created_at,updated_at FROM `+s.linesTable()+` WHERE id=?`, id).Scan(
		&line.ID, &line.Name, &line.Status, &line.EntryRegion, &line.ExitRegion, &line.Provider,
		&line.CapacityMbps, &line.ActiveDeployment, &line.Profile, &line.SecretRef, &line.CreatedAt, &line.UpdatedAt)
	return line, err
}

func (s *Store) Lines(ctx context.Context) ([]Line, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,name,status,entry_region,exit_region,provider,capacity_mbps,
 active_deployment,profile,secret_ref,created_at,updated_at FROM `+s.linesTable()+` WHERE status<>'archived' ORDER BY name`)
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
	if _, err := s.Line(ctx, item.LineID); err != nil {
		return false, err
	}
	unlock := s.lockTelemetryWrite()
	defer unlock()
	if item.WorkerID == "collector" && item.Health == "down" && strings.HasSuffix(item.NodeID, "-collector") {
		return false, nil
	}
	payload := item.Payload
	if len(payload) == 0 {
		payload = json.RawMessage(`{}`)
	}
	tx, err := s.telemetryDB.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO snapshots
	 (line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
	 upstream_mbps,downstream_mbps,queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at)
	 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, item.LineID, item.NodeID, item.Role, item.WorkerID, item.ObservedAt,
		item.Health, item.Deployment, item.Profile, item.Sessions, item.ThroughputMbps, item.UpstreamMbps, item.DownstreamMbps, item.QueueAgeP95US,
		item.EffectiveLoss, item.FECObserve, item.FECActive, []byte(`{}`), now())
	if err != nil {
		return false, err
	}
	count, err := result.RowsAffected()
	if err != nil || count == 0 {
		return false, err
	}
	observedAt, err := time.Parse(time.RFC3339Nano, item.ObservedAt)
	if err != nil {
		return false, err
	}
	for _, resolution := range []time.Duration{time.Minute, 5 * time.Minute, time.Hour} {
		if err = upsertTrafficRollup(ctx, tx, item, observedAt.Truncate(resolution), int(resolution/time.Second)); err != nil {
			return false, err
		}
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO latest_snapshot_payloads(line_id,node_id,observed_at,payload)
	 VALUES(?,?,?,?) ON CONFLICT(line_id,node_id) DO UPDATE SET observed_at=excluded.observed_at,payload=excluded.payload
	 WHERE excluded.observed_at>=latest_snapshot_payloads.observed_at`, item.LineID, item.NodeID, item.ObservedAt, []byte(payload)); err != nil {
		return false, err
	}
	if err = tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}

func (s *Store) RecordIncident(ctx context.Context, item Incident) (bool, error) {
	unlock := s.lockWrite()
	defer unlock()
	payload := item.Payload
	if len(payload) == 0 {
		payload = json.RawMessage(`{}`)
	}
	query := s.controlSQL(`INSERT OR IGNORE INTO incidents
	 (id,line_id,severity,status,kind,message,observed_at,payload) VALUES(?,?,?,?,?,?,?,?)`, `INSERT IGNORE INTO incidents
	 (id,line_id,severity,status,kind,message,observed_at,payload) VALUES(?,?,?,?,?,?,?,?)`)
	result, err := s.db.ExecContext(ctx, query,
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
	s.operationMu.Lock()
	defer s.operationMu.Unlock()
	unlock := s.lockWrite()
	defer unlock()
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
	inserted, err := s.db.ExecContext(ctx, `INSERT INTO operations
 (id,line_id,kind,status,requested_by,idempotency_key,request,created_at,updated_at)
 SELECT ?,?,?,?,?,?,?,?,? WHERE NOT EXISTS (
 SELECT 1 FROM operations WHERE line_id=? AND status IN ('queued','dispatched','running'))`,
		operation.ID, operation.LineID, operation.Kind, operation.Status, operation.RequestedBy,
		operation.IdempotencyKey, []byte(operation.Request), stamp, stamp, operation.LineID)
	if err != nil {
		return Operation{}, false, err
	}
	count, err := inserted.RowsAffected()
	if err != nil {
		return Operation{}, false, err
	}
	if count != 1 {
		return Operation{}, false, errors.New("line already has an active operation")
	}
	return operation, false, nil
}

func (s *Store) OperationByIdempotencyKey(ctx context.Context, key string) (Operation, error) {
	var item Operation
	err := scanOperation(s.db.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,
	 request,result,created_at,updated_at FROM operations WHERE idempotency_key=?`, key), &item)
	return item, err
}

func (s *Store) AllocateTransportGeneration(ctx context.Context, lineID string) (uint64, error) {
	unlock := s.lockWrite()
	defer unlock()
	if s.dialect == "mysql" {
		tx, err := s.db.BeginTx(ctx, nil)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback()
		if _, err = tx.ExecContext(ctx, `INSERT INTO transport_generations(line_id,current_generation,updated_at)
		 VALUES(?,1,?) ON DUPLICATE KEY UPDATE current_generation=current_generation+1,updated_at=VALUES(updated_at)`, lineID, now()); err != nil {
			return 0, err
		}
		var generation uint64
		if err = tx.QueryRowContext(ctx, `SELECT current_generation FROM transport_generations WHERE line_id=?`, lineID).Scan(&generation); err != nil {
			return 0, err
		}
		return generation, tx.Commit()
	}
	var generation uint64
	err := s.db.QueryRowContext(ctx, `INSERT INTO transport_generations(line_id,current_generation,updated_at)
	 VALUES(?,1,?) ON CONFLICT(line_id) DO UPDATE SET
	 current_generation=transport_generations.current_generation+1,updated_at=excluded.updated_at
	 RETURNING current_generation`, lineID, now()).Scan(&generation)
	return generation, err
}

func (s *Store) Operations(ctx context.Context, lineID string, limit int) ([]Operation, error) {
	query := `SELECT id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at FROM operations
	 WHERE NOT EXISTS (SELECT 1 FROM operation_cleanups cleanup WHERE cleanup.operation_id=operations.id)`
	args := []any{}
	if lineID != "" {
		query += ` AND line_id=?`
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

func (s *Store) LatestSuccessfulOperation(ctx context.Context, lineID, kind string) (Operation, error) {
	var item Operation
	err := scanOperation(s.db.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,
	 request,result,created_at,updated_at FROM operations WHERE line_id=? AND kind=? AND status='succeeded'
	 ORDER BY updated_at DESC LIMIT 1`, lineID, kind), &item)
	return item, err
}

func (s *Store) AttachClientURL(ctx context.Context, lineID, clientURL string) (string, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	var operationID string
	var raw []byte
	err = tx.QueryRowContext(ctx, `SELECT id,result FROM operations
	 WHERE line_id=? AND kind='line.open' AND status='succeeded' ORDER BY updated_at DESC LIMIT 1`, lineID).Scan(&operationID, &raw)
	if err != nil {
		return "", err
	}
	result := map[string]any{}
	if len(raw) > 0 && json.Unmarshal(raw, &result) != nil {
		return "", errors.New("operation result is invalid")
	}
	result["client_url"] = clientURL
	encoded, err := json.Marshal(result)
	if err != nil {
		return "", err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE operations SET result=? WHERE id=?`, encoded, operationID); err != nil {
		return "", err
	}
	if err = tx.Commit(); err != nil {
		return "", err
	}
	return operationID, nil
}

func (s *Store) ClaimOperations(ctx context.Context, lineID string, limit int) ([]Operation, error) {
	return s.claimOperations(ctx, lineID, limit, nil)
}

func (s *Store) ClaimAnyOperations(ctx context.Context, limit int) ([]Operation, error) {
	return s.ClaimAnyOperationsExcept(ctx, limit, nil)
}

func (s *Store) ClaimAnyOperationsExcept(ctx context.Context, limit int, excluded []string) ([]Operation, error) {
	return s.claimOperations(ctx, "", limit, excluded)
}

func (s *Store) claimOperations(ctx context.Context, lineID string, limit int, excluded []string) ([]Operation, error) {
	unlock := s.lockWrite()
	defer unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	query := `SELECT id FROM operations WHERE status='queued'`
	args := []any{}
	if lineID != "" {
		query += ` AND line_id=?`
		args = append(args, lineID)
	}
	if len(excluded) > 0 {
		query += ` AND line_id NOT IN (` + strings.TrimSuffix(strings.Repeat("?,", len(excluded)), ",") + `)`
		for _, id := range excluded {
			args = append(args, id)
		}
	}
	query += ` ORDER BY created_at LIMIT ?`
	args = append(args, limit)
	rows, err := tx.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			_ = rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	stamp := now()
	result := make([]Operation, 0, len(ids))
	for _, id := range ids {
		updated, updateErr := tx.ExecContext(ctx, `UPDATE operations SET status='dispatched',updated_at=? WHERE id=? AND status='queued'`, stamp, id)
		if updateErr != nil {
			return nil, updateErr
		}
		count, countErr := updated.RowsAffected()
		if countErr != nil || count != 1 {
			return nil, errors.New("operation claim conflict")
		}
		var item Operation
		if err = scanOperation(tx.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at FROM operations WHERE id=?`, id), &item); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return result, nil
}

func (s *Store) CompleteOperation(ctx context.Context, id, lineID, status string, result json.RawMessage) error {
	unlock := s.lockWrite()
	defer unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var kind, currentStatus string
	var currentResult []byte
	if err = tx.QueryRowContext(ctx, `SELECT kind,status,result FROM operations WHERE id=? AND line_id=?`, id, lineID).Scan(&kind, &currentStatus, &currentResult); err != nil {
		return fmt.Errorf("operation is not active")
	}
	if currentStatus != "dispatched" && currentStatus != "running" {
		if currentStatus == status && bytes.Equal(currentResult, result) {
			return nil
		}
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
			Deployment          string `json:"deployment"`
			Profile             string `json:"profile"`
			TransportGeneration uint64 `json:"transport_generation"`
		}
		_ = json.Unmarshal(result, &values)
		switch kind {
		case "line.open", "line.upgrade", "line.rollback":
			if _, err = tx.ExecContext(ctx, `UPDATE `+s.linesTable()+` SET status='active',
 active_deployment=CASE WHEN ?='' THEN active_deployment ELSE ? END,
 profile=CASE WHEN ?='' THEN profile ELSE ? END,updated_at=? WHERE id=?`,
				values.Deployment, values.Deployment, values.Profile, values.Profile, now(), lineID); err != nil {
				return err
			}
		case "line.disable":
			if _, err = tx.ExecContext(ctx, `UPDATE `+s.linesTable()+` SET status='disabled',updated_at=? WHERE id=?`, now(), lineID); err != nil {
				return err
			}
		case "line.tune":
			if values.Profile == "" {
				return errors.New("successful transport tuning requires a profile result")
			}
			if _, err = tx.ExecContext(ctx, `UPDATE `+s.linesTable()+` SET profile=?,updated_at=? WHERE id=?`, values.Profile, now(), lineID); err != nil {
				return err
			}
			if values.TransportGeneration > 0 {
				query := s.controlSQL(`INSERT INTO transport_generations(line_id,current_generation,updated_at)
				 VALUES(?,?,?) ON CONFLICT(line_id) DO UPDATE SET current_generation=MAX(transport_generations.current_generation,excluded.current_generation),updated_at=excluded.updated_at`,
					`INSERT INTO transport_generations(line_id,current_generation,updated_at) VALUES(?,?,?)
				 ON DUPLICATE KEY UPDATE current_generation=GREATEST(current_generation,VALUES(current_generation)),updated_at=VALUES(updated_at)`)
				if _, err = tx.ExecContext(ctx, query, lineID, values.TransportGeneration, now()); err != nil {
					return err
				}
			}
		}
	}
	return tx.Commit()
}

func (s *Store) CancelOperation(ctx context.Context, id, reason string) error {
	unlock := s.lockWrite()
	defer unlock()
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
	unlock := s.lockWrite()
	defer unlock()
	capabilities, err := json.Marshal(item.Lines)
	if err != nil {
		return err
	}
	query := s.controlSQL(`INSERT INTO executors
 (worker_id,status,version,capabilities,observed_at,updated_at) VALUES(?,?,?,?,?,?)
 ON CONFLICT(worker_id) DO UPDATE SET status=excluded.status,version=excluded.version,
	 capabilities=excluded.capabilities,observed_at=excluded.observed_at,updated_at=excluded.updated_at`, `INSERT INTO executors
 (worker_id,status,version,capabilities,observed_at,updated_at) VALUES(?,?,?,?,?,?)
 ON DUPLICATE KEY UPDATE status=VALUES(status),version=VALUES(version),capabilities=VALUES(capabilities),
 observed_at=VALUES(observed_at),updated_at=VALUES(updated_at)`)
	_, err = s.db.ExecContext(ctx, query,
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
	query := s.controlSQL(`INSERT OR IGNORE INTO raw_events
	 (path,idempotency_key,payload,received_at) VALUES(?,?,?,?)`, `INSERT IGNORE INTO raw_events
	 (path,idempotency_key,payload,received_at) VALUES(?,?,?,?)`)
	result, err := s.db.ExecContext(ctx, query, path, key, []byte(payload), now())
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
		rows, queryErr := s.telemetryDB.QueryContext(ctx, `SELECT s.role,s.health,s.deployment,s.profile,s.sessions,s.throughput_mbps,
	 s.upstream_mbps,s.downstream_mbps,s.queue_age_p95_us,s.effective_loss_pct,s.fec_observe,s.fec_active,s.observed_at
	 FROM latest_snapshots latest JOIN snapshots s ON s.id=latest.snapshot_id WHERE latest.line_id=?`, line.ID)
		if queryErr != nil {
			return Dashboard{}, queryErr
		}
		healthy := true
		for rows.Next() {
			var role, health, deployment, profile, observed string
			var sessions int64
			var throughput, upstream, downstream, queue, loss float64
			var observe, active bool
			if queryErr = rows.Scan(&role, &health, &deployment, &profile, &sessions, &throughput, &upstream, &downstream, &queue, &loss, &observe, &active, &observed); queryErr != nil {
				rows.Close()
				return Dashboard{}, queryErr
			}
			item.Workers++
			if role == "entry" {
				item.Sessions += sessions
				item.ThroughputMbps += throughput
				item.UpstreamMbps += upstream
				item.DownstreamMbps += downstream
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
