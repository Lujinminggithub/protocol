package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"path/filepath"
	"sort"
	"sync"
	"testing"
	"time"
)

func TestTuneCompletionUpdatesLineProfile(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	stamp := now()
	_, err = store.db.Exec(`INSERT INTO lines
	 (id,name,status,entry_region,exit_region,provider,capacity_mbps,profile,created_at,updated_at)
	 VALUES('line-1','test','active','entry','exit','test',10,'line-1:1',?,?)`, stamp, stamp)
	if err != nil {
		t.Fatal(err)
	}
	operation := Operation{ID: "op-tune", LineID: "line-1", Kind: "line.tune", RequestedBy: "operator",
		IdempotencyKey: "tune-1", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(context.Background(), operation); err != nil {
		t.Fatal(err)
	}
	claimed, err := store.ClaimOperations(context.Background(), "line-1", 1)
	if err != nil || len(claimed) != 1 {
		t.Fatalf("claim=%+v err=%v", claimed, err)
	}
	if err = store.CompleteOperation(context.Background(), "op-tune", "line-1", "succeeded",
		json.RawMessage(`{"profile":"line-1:2","transport_generation":2}`)); err != nil {
		t.Fatal(err)
	}
	line, err := store.Line(context.Background(), "line-1")
	if err != nil || line.Profile != "line-1:2" {
		t.Fatalf("line=%+v err=%v", line, err)
	}
}

func TestAllocateTransportGenerationIsAtomic(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	stamp := now()
	_, err = store.db.Exec(`INSERT INTO lines
	 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
	 VALUES('line-1','test','active','entry','exit','test',10,?,?)`, stamp, stamp)
	if err != nil {
		t.Fatal(err)
	}
	const count = 16
	values := make([]int, count)
	errors := make(chan error, count)
	var wait sync.WaitGroup
	for index := range values {
		wait.Add(1)
		go func(at int) {
			defer wait.Done()
			generation, allocateErr := store.AllocateTransportGeneration(context.Background(), "line-1")
			values[at] = int(generation)
			errors <- allocateErr
		}(index)
	}
	wait.Wait()
	close(errors)
	for allocateErr := range errors {
		if allocateErr != nil {
			t.Fatal(allocateErr)
		}
	}
	sort.Ints(values)
	for index, generation := range values {
		if generation != index+1 {
			t.Fatalf("generations=%v", values)
		}
	}
}

func TestOpenMigratesExitBindIP(t *testing.T) {
	path := filepath.Join(t.TempDir(), "legacy.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Exec(`CREATE TABLE line_specs (
 line_id TEXT PRIMARY KEY, resource_group TEXT NOT NULL, instance_id TEXT NOT NULL,
 bandwidth_mbps INTEGER NOT NULL, socks_port INTEGER NOT NULL,
 udp_port_min INTEGER NOT NULL, udp_port_max INTEGER NOT NULL,
 relay_port INTEGER NOT NULL, exit_port INTEGER NOT NULL,
 whitelist BLOB NOT NULL, build_mode TEXT NOT NULL, artifact_ref TEXT NOT NULL,
 source_ref TEXT NOT NULL, srs_ref TEXT NOT NULL, jump_policy TEXT NOT NULL,
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL)`)
	if err != nil {
		t.Fatal(err)
	}
	if err = db.Close(); err != nil {
		t.Fatal(err)
	}
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	rows, err := store.db.Query(`PRAGMA table_info(line_specs)`)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for rows.Next() {
		var cid, notNull, primaryKey int
		var name, kind string
		var defaultValue any
		if err = rows.Scan(&cid, &name, &kind, &notNull, &defaultValue, &primaryKey); err != nil {
			t.Fatal(err)
		}
		found = found || name == "exit_bind_ip"
	}
	_ = rows.Close()
	if !found {
		t.Fatal("exit_bind_ip migration was not applied")
	}
}

func TestOpenAllowsConcurrentDatabaseWork(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	if got := store.db.Stats().MaxOpenConnections; got < 2 {
		t.Fatalf("MaxOpenConnections=%d, want at least 2", got)
	}
	held, err := store.db.Conn(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer held.Close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err = store.Ping(ctx); err != nil {
		t.Fatalf("ping blocked behind an occupied connection: %v", err)
	}
}

func TestOpenCreatesLatestSnapshotIndex(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	var count int
	err = store.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='snapshots_latest_node'`).Scan(&count)
	if err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("snapshots_latest_node count=%d, want 1", count)
	}
}

func TestDashboardHandlesSnapshotHistory(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	stamp := now()
	_, err = store.db.Exec(`INSERT INTO lines
	 (id,name,status,entry_region,exit_region,provider,capacity_mbps,active_deployment,profile,secret_ref,created_at,updated_at)
	 VALUES('line-1','test','active','entry','exit','test',10,'deployment-1','profile-1','',?,?)`, stamp, stamp)
	if err != nil {
		t.Fatal(err)
	}
	_, err = store.db.Exec(`WITH RECURSIVE a(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM a WHERE i<250),
	 b(j) AS (VALUES(1) UNION ALL SELECT j+1 FROM b WHERE j<100)
	 INSERT INTO snapshots
	 (line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
	 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at)
	 SELECT 'line-1','node-' || ((a.i*100+b.j)%6),'entry','worker-1',printf('%08d',a.i*100+b.j),
	 'ok','deployment-1','profile-1',1,1.0,1.0,0.0,1,0,'{}',? FROM a CROSS JOIN b`, stamp)
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	view, err := store.Dashboard(ctx)
	if err != nil {
		t.Fatalf("dashboard over snapshot history: %v", err)
	}
	if view.LinesTotal != 1 || len(view.Lines) != 1 || view.Lines[0].Workers != 6 {
		t.Fatalf("unexpected dashboard summary: %+v", view)
	}
}
