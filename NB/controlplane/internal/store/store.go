package store

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	_ "modernc.org/sqlite"
)

type Store struct{ db *sql.DB }

var ErrOperationConflict = errors.New("idempotency key was already used for a different request")

type User struct {
	ID         string `json:"id"`
	Username   string `json:"username"`
	Status     string `json:"status"`
	Plan       string `json:"plan"`
	Route      string `json:"route"`
	MaxTCP     int64  `json:"max_tcp"`
	MaxUDP     int64  `json:"max_udp"`
	RateKbps   int64  `json:"rate_kbps"`
	QuotaMB    int64  `json:"quota_mb"`
	Credential string `json:"credential_id,omitempty"`
	UpdatedAt  string `json:"updated_at"`
}

type Credential struct {
	ID         string
	Iterations int
	Salt       string
	Hash       string
}

type UsageEvent struct {
	EventID   string `json:"event_id"`
	NodeID    string `json:"node_id"`
	WorkerID  string `json:"worker_id"`
	BootID    string `json:"boot_id"`
	Sequence  int64  `json:"sequence"`
	Tenant    string `json:"tenant"`
	BytesUp   int64  `json:"bytes_up"`
	BytesDown int64  `json:"bytes_down"`
	Observed  string `json:"observed_at"`
}

type OutboxItem struct {
	ID             int64
	Path           string
	IdempotencyKey string
	Payload        []byte
	Attempts       int
}

type BillingLine struct {
	Tenant    string `json:"tenant"`
	BytesUp   int64  `json:"bytes_up"`
	BytesDown int64  `json:"bytes_down"`
}

type BillingPeriod struct {
	ID        string        `json:"id"`
	From      string        `json:"from"`
	To        string        `json:"to"`
	Status    string        `json:"status"`
	CreatedAt string        `json:"created_at"`
	Lines     []BillingLine `json:"lines"`
}

func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for _, pragma := range []string{
		"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=5000",
	} {
		if _, err = db.ExecContext(ctx, pragma); err != nil {
			db.Close()
			return nil, fmt.Errorf("sqlite %s: %w", pragma, err)
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

func (s *Store) migrate(ctx context.Context) error {
	schema := `
CREATE TABLE IF NOT EXISTS users (
 id TEXT PRIMARY KEY, username TEXT NOT NULL UNIQUE, status TEXT NOT NULL,
 plan TEXT NOT NULL, route TEXT NOT NULL, max_tcp INTEGER NOT NULL,
 max_udp INTEGER NOT NULL, rate_kbps INTEGER NOT NULL, quota_mb INTEGER NOT NULL,
 credential_id TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS usage_events (
 event_id TEXT PRIMARY KEY, node_id TEXT NOT NULL, worker_id TEXT NOT NULL,
 boot_id TEXT NOT NULL, sequence INTEGER NOT NULL, tenant TEXT NOT NULL,
 bytes_up INTEGER NOT NULL, bytes_down INTEGER NOT NULL, observed_at TEXT NOT NULL,
 received_at TEXT NOT NULL, UNIQUE(node_id,worker_id,boot_id,sequence)
);
CREATE TABLE IF NOT EXISTS usage_cursors (
 cursor_key TEXT PRIMARY KEY, node_id TEXT NOT NULL, worker_id TEXT NOT NULL,
 boot_id TEXT NOT NULL, tenant TEXT NOT NULL, bytes_up INTEGER NOT NULL,
 bytes_down INTEGER NOT NULL, sequence INTEGER NOT NULL, observed_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS node_snapshots (
 idempotency_key TEXT PRIMARY KEY, node_id TEXT NOT NULL, observed_at TEXT NOT NULL,
 payload BLOB NOT NULL, received_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS incidents (
 incident_id TEXT PRIMARY KEY, severity TEXT NOT NULL, status TEXT NOT NULL,
 payload BLOB NOT NULL, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS outbox (
 id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL, idempotency_key TEXT NOT NULL UNIQUE,
 payload BLOB NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, next_attempt_at INTEGER NOT NULL DEFAULT 0,
 last_error TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, delivered_at TEXT
);
CREATE TABLE IF NOT EXISTS audit_log (
 id INTEGER PRIMARY KEY AUTOINCREMENT, operation_id TEXT NOT NULL UNIQUE,
 actor TEXT NOT NULL, action TEXT NOT NULL, object_id TEXT NOT NULL,
 before_json BLOB, after_json BLOB, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS billing_periods (
 id TEXT PRIMARY KEY, from_at TEXT NOT NULL, to_at TEXT NOT NULL,
 status TEXT NOT NULL, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS billing_lines (
 period_id TEXT NOT NULL REFERENCES billing_periods(id), tenant TEXT NOT NULL,
 bytes_up INTEGER NOT NULL, bytes_down INTEGER NOT NULL,
 PRIMARY KEY(period_id,tenant)
);
CREATE INDEX IF NOT EXISTS outbox_ready ON outbox(delivered_at,next_attempt_at,id);
CREATE INDEX IF NOT EXISTS usage_tenant_time ON usage_events(tenant,observed_at);
`
	_, err := s.db.ExecContext(ctx, schema)
	if err == nil {
		// Additive migrations keep existing local control databases usable.
		for _, statement := range []string{
			`ALTER TABLE users ADD COLUMN auth_iterations INTEGER NOT NULL DEFAULT 0`,
			`ALTER TABLE users ADD COLUMN auth_salt TEXT NOT NULL DEFAULT ''`,
			`ALTER TABLE users ADD COLUMN auth_hash TEXT NOT NULL DEFAULT ''`,
			`ALTER TABLE audit_log ADD COLUMN request_hash TEXT NOT NULL DEFAULT ''`,
		} {
			if _, alterErr := s.db.ExecContext(ctx, statement); alterErr != nil && !containsDuplicateColumn(alterErr.Error()) {
				return alterErr
			}
		}
	}
	return err
}

func containsDuplicateColumn(message string) bool {
	for i := 0; i+16 <= len(message); i++ {
		if message[i:i+16] == "duplicate column" {
			return true
		}
	}
	return false
}

func now() string { return time.Now().UTC().Format(time.RFC3339Nano) }

func (s *Store) withTx(ctx context.Context, fn func(*sql.Tx) error) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	if err = fn(tx); err != nil {
		_ = tx.Rollback()
		return err
	}
	return tx.Commit()
}

func enqueue(ctx context.Context, tx *sql.Tx, path, key string, payload []byte) error {
	_, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO outbox(path,idempotency_key,payload,created_at)
 VALUES(?,?,?,?)`, path, key, payload, now())
	return err
}

func (s *Store) RecordUsage(ctx context.Context, event UsageEvent) (bool, error) {
	payload, err := json.Marshal(event)
	if err != nil {
		return false, err
	}
	inserted := false
	err = s.withTx(ctx, func(tx *sql.Tx) error {
		result, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO usage_events
 (event_id,node_id,worker_id,boot_id,sequence,tenant,bytes_up,bytes_down,observed_at,received_at)
 VALUES(?,?,?,?,?,?,?,?,?,?)`, event.EventID, event.NodeID, event.WorkerID, event.BootID,
			event.Sequence, event.Tenant, event.BytesUp, event.BytesDown, event.Observed, now())
		if err != nil {
			return err
		}
		count, err := result.RowsAffected()
		if err != nil || count == 0 {
			return err
		}
		inserted = true
		return enqueue(ctx, tx, "/api/nb/v1/usage-events", event.EventID, payload)
	})
	return inserted, err
}

func (s *Store) RecordTenantCounters(ctx context.Context, nodeID, workerID, bootID, tenant string,
	bytesUp, bytesDown int64, observed string) (bool, error) {
	key := nodeID + ":" + workerID + ":" + bootID + ":" + tenant
	inserted := false
	return inserted, s.withTx(ctx, func(tx *sql.Tx) error {
		var previousUp, previousDown, sequence int64
		err := tx.QueryRowContext(ctx, `SELECT bytes_up,bytes_down,sequence FROM usage_cursors WHERE cursor_key=?`, key).
			Scan(&previousUp, &previousDown, &sequence)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		if errors.Is(err, sql.ErrNoRows) {
			previousUp, previousDown, sequence = 0, 0, 0
		}
		deltaUp, deltaDown := bytesUp-previousUp, bytesDown-previousDown
		if deltaUp < 0 {
			deltaUp = bytesUp
		}
		if deltaDown < 0 {
			deltaDown = bytesDown
		}
		sequence++
		_, err = tx.ExecContext(ctx, `INSERT INTO usage_cursors(cursor_key,node_id,worker_id,boot_id,tenant,bytes_up,bytes_down,sequence,observed_at)
		 VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(cursor_key) DO UPDATE SET bytes_up=excluded.bytes_up,bytes_down=excluded.bytes_down,
		 sequence=excluded.sequence,observed_at=excluded.observed_at`, key, nodeID, workerID, bootID, tenant, bytesUp, bytesDown, sequence, observed)
		if err != nil || (deltaUp == 0 && deltaDown == 0) {
			return err
		}
		digest := sha256.Sum256([]byte(fmt.Sprintf("%s:%d", key, sequence)))
		event := UsageEvent{EventID: "usage-" + hex.EncodeToString(digest[:12]), NodeID: nodeID, WorkerID: workerID,
			BootID: bootID, Sequence: sequence, Tenant: tenant, BytesUp: deltaUp, BytesDown: deltaDown, Observed: observed}
		payload, err := json.Marshal(event)
		if err != nil {
			return err
		}
		result, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO usage_events
		 (event_id,node_id,worker_id,boot_id,sequence,tenant,bytes_up,bytes_down,observed_at,received_at)
		 VALUES(?,?,?,?,?,?,?,?,?,?)`, event.EventID, nodeID, workerID, bootID, sequence, tenant, deltaUp, deltaDown, observed, now())
		if err != nil {
			return err
		}
		count, err := result.RowsAffected()
		if err != nil || count == 0 {
			return err
		}
		inserted = true
		return enqueue(ctx, tx, "/api/nb/v1/usage-events", event.EventID, payload)
	})
}

func (s *Store) RecordSnapshot(ctx context.Context, key, nodeID, observed string, payload []byte) (bool, error) {
	inserted := false
	err := s.withTx(ctx, func(tx *sql.Tx) error {
		result, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO node_snapshots
 (idempotency_key,node_id,observed_at,payload,received_at) VALUES(?,?,?,?,?)`, key, nodeID, observed, payload, now())
		if err != nil {
			return err
		}
		count, err := result.RowsAffected()
		if err != nil || count == 0 {
			return err
		}
		inserted = true
		return enqueue(ctx, tx, "/api/nb/v1/node-snapshots", key, payload)
	})
	return inserted, err
}

func (s *Store) RecordIncident(ctx context.Context, id, severity, status string, payload []byte) (bool, error) {
	inserted := false
	err := s.withTx(ctx, func(tx *sql.Tx) error {
		result, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO incidents
 (incident_id,severity,status,payload,created_at) VALUES(?,?,?,?,?)`, id, severity, status, payload, now())
		if err != nil {
			return err
		}
		count, err := result.RowsAffected()
		if err != nil || count == 0 {
			return err
		}
		inserted = true
		return enqueue(ctx, tx, "/api/nb/v1/incidents", id, payload)
	})
	return inserted, err
}

func (s *Store) UpsertUser(ctx context.Context, operationID, requestHash, actor, action string,
	user User, credential *Credential) (User, bool, error) {
	if user.UpdatedAt == "" {
		user.UpdatedAt = now()
	}
	var result User
	replayed := false
	err := s.withTx(ctx, func(tx *sql.Tx) error {
		var priorAction, priorObject, priorHash string
		var priorAfter []byte
		err := tx.QueryRowContext(ctx, `SELECT action,object_id,request_hash,after_json FROM audit_log WHERE operation_id=?`, operationID).
			Scan(&priorAction, &priorObject, &priorHash, &priorAfter)
		if err == nil {
			if priorAction != action || priorObject != user.ID || priorHash != requestHash || json.Unmarshal(priorAfter, &result) != nil {
				return ErrOperationConflict
			}
			replayed = true
			return nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		var before []byte
		var old User
		err = tx.QueryRowContext(ctx, `SELECT id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,updated_at
 FROM users WHERE id=?`, user.ID).Scan(&old.ID, &old.Username, &old.Status, &old.Plan, &old.Route, &old.MaxTCP, &old.MaxUDP, &old.RateKbps, &old.QuotaMB, &old.Credential, &old.UpdatedAt)
		if err == nil {
			before, _ = json.Marshal(old)
		} else if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		if credential != nil {
			user.Credential = credential.ID
			_, err = tx.ExecContext(ctx, `INSERT INTO users
 (id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,created_at,updated_at,auth_iterations,auth_salt,auth_hash)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET username=excluded.username,status=excluded.status,
 plan=excluded.plan,route=excluded.route,max_tcp=excluded.max_tcp,max_udp=excluded.max_udp,
 rate_kbps=excluded.rate_kbps,quota_mb=excluded.quota_mb,credential_id=excluded.credential_id,updated_at=excluded.updated_at,
 auth_iterations=excluded.auth_iterations,auth_salt=excluded.auth_salt,auth_hash=excluded.auth_hash`,
				user.ID, user.Username, user.Status, user.Plan, user.Route, user.MaxTCP, user.MaxUDP, user.RateKbps,
				user.QuotaMB, user.Credential, now(), user.UpdatedAt, credential.Iterations, credential.Salt, credential.Hash)
		} else {
			_, err = tx.ExecContext(ctx, `INSERT INTO users
 (id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET username=excluded.username,status=excluded.status,
 plan=excluded.plan,route=excluded.route,max_tcp=excluded.max_tcp,max_udp=excluded.max_udp,
 rate_kbps=excluded.rate_kbps,quota_mb=excluded.quota_mb,credential_id=excluded.credential_id,updated_at=excluded.updated_at`,
				user.ID, user.Username, user.Status, user.Plan, user.Route, user.MaxTCP, user.MaxUDP, user.RateKbps, user.QuotaMB, user.Credential, now(), user.UpdatedAt)
		}
		if err != nil {
			return err
		}
		after, _ := json.Marshal(user)
		if _, err = tx.ExecContext(ctx, `INSERT INTO audit_log(operation_id,actor,action,object_id,before_json,after_json,created_at,request_hash)
 VALUES(?,?,?,?,?,?,?,?)`, operationID, actor, action, user.ID, before, after, now(), requestHash); err != nil {
			return err
		}
		result = user
		return enqueue(ctx, tx, "/api/nb/v1/provision-results", operationID, after)
	})
	return result, replayed, err
}

func (s *Store) User(ctx context.Context, id string) (User, error) {
	var u User
	err := s.db.QueryRowContext(ctx, `SELECT id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,updated_at
 FROM users WHERE id=?`, id).Scan(&u.ID, &u.Username, &u.Status, &u.Plan, &u.Route, &u.MaxTCP, &u.MaxUDP, &u.RateKbps, &u.QuotaMB, &u.Credential, &u.UpdatedAt)
	return u, err
}

func (s *Store) Users(ctx context.Context) ([]User, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,updated_at FROM users ORDER BY username`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var users []User
	for rows.Next() {
		var u User
		if err = rows.Scan(&u.ID, &u.Username, &u.Status, &u.Plan, &u.Route, &u.MaxTCP, &u.MaxUDP, &u.RateKbps, &u.QuotaMB, &u.Credential, &u.UpdatedAt); err != nil {
			return nil, err
		}
		users = append(users, u)
	}
	return users, rows.Err()
}

func (s *Store) BootstrapUsers(ctx context.Context, users []User, credentials map[string]Credential) (bool, error) {
	imported := false
	err := s.withTx(ctx, func(tx *sql.Tx) error {
		var count int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM users`).Scan(&count); err != nil {
			return err
		}
		if count != 0 {
			return nil
		}
		for _, user := range users {
			credential, ok := credentials[user.Username]
			if !ok {
				return fmt.Errorf("missing credential for tenant %s", user.Username)
			}
			user.Credential = credential.ID
			stamp := now()
			if _, err := tx.ExecContext(ctx, `INSERT INTO users
 (id,username,status,plan,route,max_tcp,max_udp,rate_kbps,quota_mb,credential_id,created_at,updated_at,auth_iterations,auth_salt,auth_hash)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`, user.ID, user.Username, user.Status, user.Plan, user.Route,
				user.MaxTCP, user.MaxUDP, user.RateKbps, user.QuotaMB, user.Credential, stamp, stamp,
				credential.Iterations, credential.Salt, credential.Hash); err != nil {
				return err
			}
		}
		imported = len(users) > 0
		return nil
	})
	return imported, err
}

type AuthRecord struct {
	Username   string
	Iterations int
	Salt, Hash string
}

func (s *Store) SetCredential(ctx context.Context, id, credentialID string, iterations int, salt, hash string) error {
	_, err := s.db.ExecContext(ctx, `UPDATE users SET credential_id=?,auth_iterations=?,auth_salt=?,auth_hash=?,updated_at=? WHERE id=?`,
		credentialID, iterations, salt, hash, now(), id)
	return err
}

func (s *Store) AuthRecords(ctx context.Context) ([]AuthRecord, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT username,auth_iterations,auth_salt,auth_hash FROM users
 WHERE status='active' AND auth_iterations>0 ORDER BY username`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var records []AuthRecord
	for rows.Next() {
		var r AuthRecord
		if err = rows.Scan(&r.Username, &r.Iterations, &r.Salt, &r.Hash); err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	return records, rows.Err()
}

func (s *Store) UsageTotals(ctx context.Context, tenant, from, to string) (int64, int64, error) {
	var up, down sql.NullInt64
	err := s.db.QueryRowContext(ctx, `SELECT SUM(bytes_up),SUM(bytes_down) FROM usage_events
 WHERE tenant=? AND observed_at>=? AND observed_at<?`, tenant, from, to).Scan(&up, &down)
	return up.Int64, down.Int64, err
}

func (s *Store) CloseBillingPeriod(ctx context.Context, operationID, requestHash, id, from, to string) (BillingPeriod, bool, error) {
	var period BillingPeriod
	replayed := false
	err := s.withTx(ctx, func(tx *sql.Tx) error {
		var action, objectID, priorHash string
		var after []byte
		err := tx.QueryRowContext(ctx, `SELECT action,object_id,request_hash,after_json FROM audit_log WHERE operation_id=?`, operationID).
			Scan(&action, &objectID, &priorHash, &after)
		if err == nil {
			if action != "billing.close" || objectID != id || priorHash != requestHash || json.Unmarshal(after, &period) != nil {
				return ErrOperationConflict
			}
			replayed = true
			return nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		period = BillingPeriod{ID: id, From: from, To: to, Status: "closed", CreatedAt: now()}
		if _, err = tx.ExecContext(ctx, `INSERT INTO billing_periods(id,from_at,to_at,status,created_at) VALUES(?,?,?,?,?)`,
			id, from, to, period.Status, period.CreatedAt); err != nil {
			return err
		}
		rows, err := tx.QueryContext(ctx, `SELECT u.username,COALESCE(SUM(e.bytes_up),0),COALESCE(SUM(e.bytes_down),0)
 FROM users u LEFT JOIN usage_events e ON e.tenant=u.username AND e.observed_at>=? AND e.observed_at<?
 GROUP BY u.username ORDER BY u.username`, from, to)
		if err != nil {
			return err
		}
		for rows.Next() {
			var line BillingLine
			if err = rows.Scan(&line.Tenant, &line.BytesUp, &line.BytesDown); err != nil {
				rows.Close()
				return err
			}
			period.Lines = append(period.Lines, line)
		}
		if err = rows.Close(); err != nil {
			return err
		}
		for _, line := range period.Lines {
			if _, err = tx.ExecContext(ctx, `INSERT INTO billing_lines(period_id,tenant,bytes_up,bytes_down) VALUES(?,?,?,?)`,
				id, line.Tenant, line.BytesUp, line.BytesDown); err != nil {
				return err
			}
		}
		after, _ = json.Marshal(period)
		if _, err = tx.ExecContext(ctx, `INSERT INTO audit_log(operation_id,actor,action,object_id,after_json,created_at,request_hash)
 VALUES(?,?,?,?,?,?,?)`, operationID, "web", "billing.close", id, after, now(), requestHash); err != nil {
			return err
		}
		return enqueue(ctx, tx, "/api/nb/v1/billing-periods", operationID, after)
	})
	return period, replayed, err
}

func (s *Store) BillingPeriod(ctx context.Context, id string) (BillingPeriod, error) {
	var period BillingPeriod
	err := s.db.QueryRowContext(ctx, `SELECT id,from_at,to_at,status,created_at FROM billing_periods WHERE id=?`, id).
		Scan(&period.ID, &period.From, &period.To, &period.Status, &period.CreatedAt)
	if err != nil {
		return period, err
	}
	rows, err := s.db.QueryContext(ctx, `SELECT tenant,bytes_up,bytes_down FROM billing_lines WHERE period_id=? ORDER BY tenant`, id)
	if err != nil {
		return period, err
	}
	defer rows.Close()
	for rows.Next() {
		var line BillingLine
		if err = rows.Scan(&line.Tenant, &line.BytesUp, &line.BytesDown); err != nil {
			return period, err
		}
		period.Lines = append(period.Lines, line)
	}
	return period, rows.Err()
}

func (s *Store) ReadyOutbox(ctx context.Context, limit int) ([]OutboxItem, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,path,idempotency_key,payload,attempts FROM outbox
 WHERE delivered_at IS NULL AND next_attempt_at<=? ORDER BY id LIMIT ?`, time.Now().Unix(), limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []OutboxItem
	for rows.Next() {
		var item OutboxItem
		if err = rows.Scan(&item.ID, &item.Path, &item.IdempotencyKey, &item.Payload, &item.Attempts); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) MarkDelivered(ctx context.Context, id int64) error {
	_, err := s.db.ExecContext(ctx, `UPDATE outbox SET delivered_at=?,last_error='' WHERE id=?`, now(), id)
	return err
}

func (s *Store) MarkFailed(ctx context.Context, id int64, attempts int, message string) error {
	delay := int64(1 << min(attempts, 8))
	if delay > 300 {
		delay = 300
	}
	_, err := s.db.ExecContext(ctx, `UPDATE outbox SET attempts=?,next_attempt_at=?,last_error=? WHERE id=?`, attempts, time.Now().Unix()+delay, message, id)
	return err
}

func (s *Store) Stats(ctx context.Context) (pending, failed, users, usage int64, err error) {
	queries := []struct {
		q   string
		out *int64
	}{
		{`SELECT COUNT(*) FROM outbox WHERE delivered_at IS NULL`, &pending},
		{`SELECT COUNT(*) FROM outbox WHERE delivered_at IS NULL AND attempts>0`, &failed},
		{`SELECT COUNT(*) FROM users WHERE status='active'`, &users},
		{`SELECT COUNT(*) FROM usage_events`, &usage},
	}
	for _, item := range queries {
		if err = s.db.QueryRowContext(ctx, item.q).Scan(item.out); err != nil {
			return
		}
	}
	return
}
