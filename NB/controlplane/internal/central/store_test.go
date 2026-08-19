package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestDeviceHostKeyMigrationAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "central.db")
	legacy, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	_, err = legacy.Exec(`CREATE TABLE devices (
 id TEXT PRIMARY KEY, name TEXT NOT NULL, status TEXT NOT NULL,
 host TEXT NOT NULL, ssh_port INTEGER NOT NULL, ssh_user TEXT NOT NULL,
 private_ip TEXT NOT NULL DEFAULT '', region TEXT NOT NULL DEFAULT '',
 provider TEXT NOT NULL DEFAULT '', os TEXT NOT NULL DEFAULT '', arch TEXT NOT NULL DEFAULT '',
 secret_ref TEXT NOT NULL DEFAULT '', labels BLOB NOT NULL DEFAULT '{}',
 last_health TEXT NOT NULL DEFAULT 'unknown', last_seen_at TEXT NOT NULL DEFAULT '',
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
INSERT INTO devices(id,name,status,host,ssh_port,ssh_user,created_at,updated_at)
VALUES('legacy-exit','legacy','ready','192.0.2.40',5222,'root','2026-08-05T00:00:00Z','2026-08-05T00:00:00Z')`)
	if err != nil {
		t.Fatal(err)
	}
	if err = legacy.Close(); err != nil {
		t.Fatal(err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	legacyDevice, err := store.Device(t.Context(), "legacy-exit")
	if err != nil {
		t.Fatal(err)
	}
	if legacyDevice.SSHHostKeyStatus != "pending" || legacyDevice.SSHHostKey != "" || legacyDevice.SSHHostKeySHA256 != "" {
		t.Fatalf("legacy device did not migrate to pending trust: %+v", legacyDevice)
	}

	trusted := Device{ID: "trusted-exit", Name: "trusted", Status: "ready", Host: "192.0.2.41",
		SSHPort: 5222, SSHUser: "root", SecretRef: "device:trusted-exit", Labels: json.RawMessage(`{}`),
		SSHHostKey: "AAAAC3NzaC1lZDI1NTE5AAAAITestKey", SSHHostKeyType: "ssh-ed25519",
		SSHHostKeySHA256: "SHA256:test-fingerprint", SSHHostKeyStatus: "trusted",
		SSHHostKeyConfirmedAt: "2026-08-05T01:00:00Z"}
	stored, err := store.UpsertDevice(t.Context(), trusted)
	if err != nil {
		t.Fatal(err)
	}
	if stored.SSHHostKey != trusted.SSHHostKey || stored.SSHHostKeyType != trusted.SSHHostKeyType ||
		stored.SSHHostKeySHA256 != trusted.SSHHostKeySHA256 || stored.SSHHostKeyStatus != "trusted" ||
		stored.SSHHostKeyConfirmedAt != trusted.SSHHostKeyConfirmedAt {
		t.Fatalf("trusted SSH identity was not persisted: %+v", stored)
	}
	pending, err := store.UpsertDevice(t.Context(), Device{ID: "discovered-exit", Name: "discovered", Status: "ready",
		Host: "192.0.2.42", SSHPort: 22, SSHUser: "root", SecretRef: "worker-local:exit", Labels: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatal(err)
	}
	if pending.SSHHostKeyStatus != "pending" {
		t.Fatalf("device without a confirmed identity was not normalized to pending: %+v", pending)
	}
}

func TestCreateOperationRejectsAnotherActiveOperationOnLine(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-1", Name: "test", Status: "draft",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	first := Operation{ID: "op-open", LineID: "line-1", Kind: "line.open", RequestedBy: "operator",
		IdempotencyKey: "open-1", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(t.Context(), first); err != nil {
		t.Fatal(err)
	}
	second := Operation{ID: "op-validate", LineID: "line-1", Kind: "line.validate", RequestedBy: "operator",
		IdempotencyKey: "validate-1", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(t.Context(), second); err == nil || !strings.Contains(err.Error(), "active operation") {
		t.Fatalf("second active operation error=%v", err)
	}
	if err = store.CancelOperation(t.Context(), first.ID, "test complete"); err != nil {
		t.Fatal(err)
	}
	if _, _, err = store.CreateOperation(t.Context(), second); err != nil {
		t.Fatalf("completed line did not accept next operation: %v", err)
	}
}

func TestDeleteOperationsOnlyRemovesTerminalTasksAtomically(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-clean", Name: "clean", Status: "draft",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	terminal := Operation{ID: "op-terminal", LineID: "line-clean", Kind: "line.validate", RequestedBy: "operator",
		IdempotencyKey: "terminal-1", Request: json.RawMessage(`{}`)}
	terminal, _, err = store.CreateOperation(t.Context(), terminal)
	if err != nil {
		t.Fatal(err)
	}
	if err = store.CancelOperation(t.Context(), terminal.ID, "test terminal task"); err != nil {
		t.Fatal(err)
	}
	if err = store.RecordOperationEvent(t.Context(), OperationEvent{OperationID: terminal.ID, Sequence: 1,
		Stage: "prepare", Status: "cancelled", Message: "test", Parameters: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	active := Operation{ID: "op-active", LineID: "line-clean", Kind: "line.validate", RequestedBy: "operator",
		IdempotencyKey: "active-1", Request: json.RawMessage(`{}`)}
	active, _, err = store.CreateOperation(t.Context(), active)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = store.DeleteOperations(t.Context(), []string{terminal.ID, active.ID}); err == nil {
		t.Fatal("batch containing an active task must be rejected")
	}
	if _, err = store.Operation(t.Context(), terminal.ID); err != nil {
		t.Fatalf("atomic rejection hid terminal task: %v", err)
	}
	if err = store.CancelOperation(t.Context(), active.ID, "test cleanup"); err != nil {
		t.Fatal(err)
	}
	deleted, err := store.DeleteOperations(t.Context(), []string{terminal.ID, active.ID})
	if err != nil || deleted != 2 {
		t.Fatalf("delete terminal tasks count=%d err=%v", deleted, err)
	}
	if _, err = store.Operation(t.Context(), terminal.ID); err != nil {
		t.Fatalf("cleanup removed durable task evidence: %v", err)
	}
	visible, err := store.Operations(t.Context(), "line-clean", 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(visible) != 0 {
		t.Fatalf("cleaned tasks remain visible: %+v", visible)
	}
	var events int
	if err = store.db.QueryRowContext(t.Context(), `SELECT COUNT(*) FROM operation_events WHERE operation_id=?`, terminal.ID).Scan(&events); err != nil {
		t.Fatal(err)
	}
	if events != 1 {
		t.Fatalf("cleanup removed operation evidence: %d", events)
	}
}

func TestOperationCleanupMigratesLegacyDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "central.db")
	legacy, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	_, err = legacy.Exec(`CREATE TABLE operations (
 id TEXT PRIMARY KEY, line_id TEXT NOT NULL, kind TEXT NOT NULL,
 status TEXT NOT NULL, requested_by TEXT NOT NULL, idempotency_key TEXT NOT NULL UNIQUE,
 request BLOB NOT NULL, result BLOB, created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);`)
	if err != nil {
		t.Fatal(err)
	}
	if err = legacy.Close(); err != nil {
		t.Fatal(err)
	}
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var tables int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='operation_cleanups'`).Scan(&tables); err != nil {
		t.Fatal(err)
	}
	if tables != 1 {
		t.Fatalf("legacy database missing operation_cleanups table: %d", tables)
	}
}

func TestAllocateLineSpecAssignsUniqueEntryPortAndSaveRechecksConflict(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, id := range []string{"line-1", "line-2"} {
		if _, err = store.UpsertLine(t.Context(), Line{ID: id, Name: id, Status: "draft",
			EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"entry-1", "relay-1", "exit-1"} {
		if _, err = store.UpsertDevice(t.Context(), Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.1",
			SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id, Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	nodes := []LineNode{{DeviceID: "entry-1", Role: "entry"}, {DeviceID: "relay-1", Role: "relay"}, {DeviceID: "exit-1", Role: "exit"}}
	first := LineSpec{LineID: "line-1", ResourceGroup: "shared", InstanceID: "line-1_1", BandwidthMbps: 10,
		UpstreamMbps: 6, DownstreamMbps: 14,
		SocksPort: 1082, RelayPort: 4445, ExitPort: 4443, UDPPortMin: 22048, UDPPortMax: 23071,
		Whitelist: json.RawMessage(`[]`), BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Nodes: nodes}
	savedFirst, err := store.SaveLineSpec(t.Context(), first)
	if err != nil {
		t.Fatal(err)
	}
	if savedFirst.UpstreamMbps != 6 || savedFirst.DownstreamMbps != 14 || savedFirst.BandwidthMbps != 14 {
		t.Fatalf("asymmetric rates were not preserved: %+v", savedFirst)
	}
	second := first
	second.LineID, second.InstanceID = "line-2", "line-2_1"
	second.UpstreamMbps, second.DownstreamMbps = 0, 0
	second.SocksPort, second.RelayPort, second.ExitPort, second.UDPPortMin, second.UDPPortMax = 0, 0, 0, 0, 0
	second, err = store.AllocateLineSpec(t.Context(), second)
	if err != nil {
		t.Fatal(err)
	}
	if second.SocksPort != 1083 || second.RelayPort != 4447 || second.ExitPort != 4445 || second.UDPPortMin != 23072 {
		t.Fatalf("unexpected automatic resources: %+v", second)
	}
	if second.UpstreamMbps != 10 || second.DownstreamMbps != 10 {
		t.Fatalf("legacy bandwidth was not inherited by both directions: %+v", second)
	}
	conflicting := second
	conflicting.SocksPort = 1082
	if _, err = store.SaveLineSpec(t.Context(), conflicting); err == nil || !strings.Contains(err.Error(), "入口端口") {
		t.Fatalf("conflicting entry port error=%v", err)
	}
	if _, err = store.SaveLineSpec(t.Context(), second); err != nil {
		t.Fatalf("allocated resources were rejected: %v", err)
	}
}

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

func TestOpenMigratesLineSpecFields(t *testing.T) {
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
	_, err = db.Exec(`INSERT INTO line_specs VALUES('legacy','group','instance',7,1080,20000,21023,4443,4443,'[]','auto','','repo://current','','auto','created','updated')`)
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
	foundExit, foundUpstream, foundDownstream, foundDNS := false, false, false, false
	for rows.Next() {
		var cid, notNull, primaryKey int
		var name, kind string
		var defaultValue any
		if err = rows.Scan(&cid, &name, &kind, &notNull, &defaultValue, &primaryKey); err != nil {
			t.Fatal(err)
		}
		foundExit = foundExit || name == "exit_bind_ip"
		foundUpstream = foundUpstream || name == "upstream_mbps"
		foundDownstream = foundDownstream || name == "downstream_mbps"
		foundDNS = foundDNS || name == "dns_servers"
	}
	_ = rows.Close()
	if !foundExit || !foundUpstream || !foundDownstream || !foundDNS {
		t.Fatal("line spec migrations were not applied")
	}
	var upstream, downstream int
	if err = store.db.QueryRow(`SELECT upstream_mbps,downstream_mbps FROM line_specs WHERE line_id='legacy'`).Scan(&upstream, &downstream); err != nil || upstream != 7 || downstream != 7 {
		t.Fatalf("legacy rates were not inherited: upstream=%d downstream=%d err=%v", upstream, downstream, err)
	}
	var dnsServers string
	if err = store.db.QueryRow(`SELECT dns_servers FROM line_specs WHERE line_id='legacy'`).Scan(&dnsServers); err != nil || dnsServers != `["1.1.1.1","8.8.8.8"]` {
		t.Fatalf("legacy DNS defaults were not applied: dns=%q err=%v", dnsServers, err)
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

func TestOpenRemovesSyntheticCollectorSnapshots(t *testing.T) {
	path := filepath.Join(t.TempDir(), "central.db")
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	stamp := now()
	if _, err = store.db.Exec(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
 VALUES('line-1','test','active','entry','exit','test',10,?,?)`, stamp, stamp); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`INSERT INTO snapshots
 (line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at)
 VALUES
 ('line-1','nb-line-1-entry-collector','entry','collector',?,'down','','',0,0,0,0,0,0,'{}',?),
 ('line-1','nb-line-1-entry-0','entry','entry-0',?,'ok','deployment-1','profile-1',0,0,0,0,1,0,'{}',?)`,
		stamp, stamp, stamp, stamp); err != nil {
		t.Fatal(err)
	}
	if err = store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	var collectors, realWorkers int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM snapshots WHERE node_id LIKE '%-collector'`).Scan(&collectors); err != nil {
		t.Fatal(err)
	}
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM snapshots WHERE node_id='nb-line-1-entry-0'`).Scan(&realWorkers); err != nil {
		t.Fatal(err)
	}
	if collectors != 0 || realWorkers != 1 {
		t.Fatalf("collectors=%d real_workers=%d", collectors, realWorkers)
	}
}

func TestRecordSnapshotRejectsSyntheticCollector(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	stamp := now()
	if _, err = store.db.Exec(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
 VALUES('line-1','test','active','entry','exit','test',10,?,?)`, stamp, stamp); err != nil {
		t.Fatal(err)
	}
	inserted, err := store.RecordSnapshot(context.Background(), Snapshot{LineID: "line-1",
		NodeID: "nb-line-1-entry-collector", Role: "entry", WorkerID: "collector",
		ObservedAt: stamp, Health: "down"})
	if err != nil || inserted {
		t.Fatalf("inserted=%v err=%v", inserted, err)
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

func TestDashboardUsesMaterializedLatestSnapshots(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	stamp := now()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-1", Name: "test", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10,
		ActiveDeployment: "deployment-1", Profile: "profile-1"}); err != nil {
		t.Fatal(err)
	}
	newest := Snapshot{LineID: "line-1", NodeID: "entry-0", Role: "entry", WorkerID: "worker-1",
		ObservedAt: stamp, Health: "ok", Deployment: "deployment-1", Profile: "profile-1",
		Sessions: 7, ThroughputMbps: 4, UpstreamMbps: 3, DownstreamMbps: 1}
	if inserted, recordErr := store.RecordSnapshot(t.Context(), newest); recordErr != nil || !inserted {
		t.Fatalf("record newest inserted=%v err=%v", inserted, recordErr)
	}
	older := newest
	older.ObservedAt = "2020-01-01T00:00:00Z"
	older.Health = "down"
	older.Sessions = 99
	if inserted, recordErr := store.RecordSnapshot(t.Context(), older); recordErr != nil || !inserted {
		t.Fatalf("record older inserted=%v err=%v", inserted, recordErr)
	}

	var latestCount, historyCount int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM latest_snapshots`).Scan(&latestCount); err != nil {
		t.Fatal(err)
	}
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM snapshots`).Scan(&historyCount); err != nil {
		t.Fatal(err)
	}
	if latestCount != 1 || historyCount != 2 {
		t.Fatalf("latest=%d history=%d", latestCount, historyCount)
	}
	view, err := store.Dashboard(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if got := view.Lines[0]; got.Health != "healthy" || got.Sessions != 7 || got.UpstreamMbps != 3 || got.DownstreamMbps != 1 {
		t.Fatalf("dashboard did not retain newest snapshot: %+v", got)
	}
	rows, err := store.db.Query(`EXPLAIN QUERY PLAN SELECT s.role FROM latest_snapshots latest
	 JOIN snapshots s ON s.id=latest.snapshot_id WHERE latest.line_id=?`, "line-1")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var plan strings.Builder
	for rows.Next() {
		var id, parent, unused int
		var detail string
		if err = rows.Scan(&id, &parent, &unused, &detail); err != nil {
			t.Fatal(err)
		}
		plan.WriteString(detail)
	}
	if !strings.Contains(plan.String(), "latest_snapshots") {
		t.Fatalf("query plan does not use materialized latest snapshots: %s", plan.String())
	}
}

func TestTrafficHistoryAggregatesWorkersWithoutAddingHops(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-traffic", Name: "traffic", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	base := time.Date(2026, 8, 18, 10, 0, 0, 0, time.UTC)
	for _, item := range []Snapshot{
		{LineID: "line-traffic", NodeID: "entry-0", Role: "entry", WorkerID: "0", ObservedAt: base.Add(2 * time.Second).Format(time.RFC3339Nano), Health: "ok", UpstreamMbps: 2, DownstreamMbps: 3, Sessions: 2},
		{LineID: "line-traffic", NodeID: "entry-1", Role: "entry", WorkerID: "1", ObservedAt: base.Add(4 * time.Second).Format(time.RFC3339Nano), Health: "ok", UpstreamMbps: 1, DownstreamMbps: 1.5, Sessions: 1},
		{LineID: "line-traffic", NodeID: "middle-0", Role: "middle", WorkerID: "0", ObservedAt: base.Add(3 * time.Second).Format(time.RFC3339Nano), Health: "degraded", UpstreamMbps: 8, DownstreamMbps: 9, Sessions: 2},
		{LineID: "line-traffic", NodeID: "exit-0", Role: "exit", WorkerID: "0", ObservedAt: base.Add(5 * time.Second).Format(time.RFC3339Nano), Health: "ok", UpstreamMbps: 7, DownstreamMbps: 8, Sessions: 2},
	} {
		if inserted, recordErr := store.RecordSnapshot(t.Context(), item); recordErr != nil || !inserted {
			t.Fatalf("RecordSnapshot inserted=%v err=%v", inserted, recordErr)
		}
	}
	history, err := store.TrafficHistory(t.Context(), "line-traffic", base, base.Add(time.Minute), 60)
	if err != nil {
		t.Fatal(err)
	}
	if history.ResolutionSeconds != 60 {
		t.Fatalf("resolution=%d", history.ResolutionSeconds)
	}
	entry := history.Role("entry")
	if len(entry.Points) != 1 || entry.Points[0].UpstreamMbps != 3 || entry.Points[0].DownstreamMbps != 4.5 {
		t.Fatalf("entry boundary was not summed by worker only: %+v", entry)
	}
	if got := history.Role("middle").Points[0].Health; got != "degraded" {
		t.Fatalf("middle health=%q", got)
	}
	if len(history.Workers) != 4 {
		t.Fatalf("workers=%d want 4", len(history.Workers))
	}
}

func TestTrafficHistoryIncludesSafeOperationalMarkers(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-markers", Name: "markers", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	base := time.Date(2026, 8, 19, 1, 0, 0, 0, time.UTC)
	if _, err = store.db.Exec(`INSERT INTO operations(id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?)`, "op-marker", "line-markers", "line.validate", "succeeded", "operator", "marker-key",
		[]byte(`{"secret":"must-not-leak"}`), []byte(`{"stdout":"must-not-leak"}`),
		base.Add(10*time.Second).Format(time.RFC3339Nano), base.Add(30*time.Second).Format(time.RFC3339Nano)); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`INSERT INTO operation_events(operation_id,sequence,stage,status,message,parameters,created_at)
 VALUES(?,?,?,?,?,?,?)`, "op-marker", 1, "probe", "running", "private command output", []byte(`{"password":"secret"}`),
		base.Add(20*time.Second).Format(time.RFC3339Nano)); err != nil {
		t.Fatal(err)
	}
	if _, err = store.RecordIncident(t.Context(), Incident{ID: "incident-marker", LineID: "line-markers", Severity: "warning",
		Status: "open", Kind: "packet-loss", Message: "sensitive remote output", ObservedAt: base.Add(40 * time.Second).Format(time.RFC3339Nano),
		Payload: json.RawMessage(`{"credential":"secret"}`)}); err != nil {
		t.Fatal(err)
	}
	history, err := store.TrafficHistory(t.Context(), "line-markers", base, base.Add(time.Minute), 15)
	if err != nil {
		t.Fatal(err)
	}
	if len(history.Markers) != 3 {
		t.Fatalf("markers=%+v want operation, event, incident", history.Markers)
	}
	encoded, err := json.Marshal(history.Markers)
	if err != nil {
		t.Fatal(err)
	}
	text := string(encoded)
	for _, required := range []string{`"kind":"operation"`, `"kind":"operation_event"`, `"kind":"incident"`, "probe", "packet-loss"} {
		if !strings.Contains(text, required) {
			t.Fatalf("markers missing %q: %s", required, text)
		}
	}
	for _, forbidden := range []string{"must-not-leak", "private command output", "password", "credential"} {
		if strings.Contains(text, forbidden) {
			t.Fatalf("markers leaked %q: %s", forbidden, text)
		}
	}
}

func TestTrafficResolutionAndRetention(t *testing.T) {
	if got := TrafficResolution(2 * time.Hour); got != 15 {
		t.Fatalf("2h resolution=%d", got)
	}
	if got := TrafficResolution(24 * time.Hour); got != 60 {
		t.Fatalf("24h resolution=%d", got)
	}
	if got := TrafficResolution(30 * 24 * time.Hour); got != 300 {
		t.Fatalf("30d resolution=%d", got)
	}

	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-retention", Name: "retention", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	nowAt := time.Date(2026, 8, 18, 12, 0, 0, 0, time.UTC)
	old := Snapshot{LineID: "line-retention", NodeID: "entry-0", Role: "entry", WorkerID: "0",
		ObservedAt: nowAt.Add(-8 * 24 * time.Hour).Format(time.RFC3339Nano), Health: "ok", UpstreamMbps: 1}
	if _, err = store.RecordSnapshot(t.Context(), old); err != nil {
		t.Fatal(err)
	}
	newest := old
	newest.ObservedAt = nowAt.Add(-time.Hour).Format(time.RFC3339Nano)
	if _, err = store.RecordSnapshot(t.Context(), newest); err != nil {
		t.Fatal(err)
	}
	if err = store.PruneTraffic(t.Context(), nowAt); err != nil {
		t.Fatal(err)
	}
	var raw int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM snapshots WHERE line_id='line-retention'`).Scan(&raw); err != nil {
		t.Fatal(err)
	}
	if raw != 1 {
		t.Fatalf("raw snapshots=%d want latest only", raw)
	}
	var rollups int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM traffic_rollups WHERE line_id='line-retention'`).Scan(&rollups); err != nil {
		t.Fatal(err)
	}
	if rollups == 0 {
		t.Fatal("recent rollups were removed")
	}
}

func TestBackfillTrafficRollupsIsIdempotent(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-backfill", Name: "backfill", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	base := time.Date(2026, 8, 18, 10, 0, 0, 0, time.UTC)
	for offset, upstream := range []float64{2, 4} {
		health := "ok"
		if offset == 1 {
			health = "unreachable"
		}
		item := Snapshot{LineID: "line-backfill", NodeID: "entry-0", Role: "entry", WorkerID: "0",
			ObservedAt: base.Add(time.Duration(offset*15) * time.Second).Format(time.RFC3339Nano), Health: health,
			Deployment: "deployment-1", Profile: "profile-1", UpstreamMbps: upstream, DownstreamMbps: 1}
		if inserted, recordErr := store.RecordSnapshot(t.Context(), item); recordErr != nil || !inserted {
			t.Fatalf("RecordSnapshot inserted=%v err=%v", inserted, recordErr)
		}
	}
	if _, err = store.db.Exec(`DELETE FROM traffic_rollups WHERE line_id='line-backfill'`); err != nil {
		t.Fatal(err)
	}
	completed, err := store.BackfillTrafficHistory(t.Context())
	if err != nil || !completed {
		t.Fatalf("first backfill completed=%v err=%v", completed, err)
	}
	completed, err = store.BackfillTrafficHistory(t.Context())
	if err != nil || completed {
		t.Fatalf("second backfill completed=%v err=%v", completed, err)
	}
	var samples, healthRank int64
	var upstream float64
	if err = store.db.QueryRow(`SELECT sample_count,upstream_sum,health_rank FROM traffic_rollups
	 WHERE line_id='line-backfill' AND resolution_s=60`).Scan(&samples, &upstream, &healthRank); err != nil {
		t.Fatal(err)
	}
	if samples != 2 || upstream != 6 || healthRank != 2 {
		t.Fatalf("samples=%d upstream=%v health_rank=%d, want 2, 6 and 2", samples, upstream, healthRank)
	}
}

func TestTopologyDeduplicatesSharedDevicesAndKeepsLineEdges(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, device := range []Device{
		{ID: "gz", Name: "GZ", Status: "ready", Host: "192.0.2.1", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)},
		{ID: "hk", Name: "HK", Status: "ready", Host: "192.0.2.2", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)},
		{ID: "us", Name: "US", Status: "ready", Host: "192.0.2.3", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)},
		{ID: "kz", Name: "KZ", Status: "ready", Host: "192.0.2.4", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)},
	} {
		if _, err = store.UpsertDevice(t.Context(), device); err != nil {
			t.Fatal(err)
		}
	}
	for index, exit := range []string{"us", "kz"} {
		lineID := "line-" + exit
		if _, err = store.UpsertLine(t.Context(), Line{ID: lineID, Name: lineID, Status: "active",
			EntryRegion: "GZ", ExitRegion: exit, Provider: "test", CapacityMbps: 10}); err != nil {
			t.Fatal(err)
		}
		spec := LineSpec{LineID: lineID, ResourceGroup: "shared", InstanceID: lineID + "_1", BandwidthMbps: 10,
			UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1080 + index, RelayPort: 4440 + index*10,
			ExitPort: 4450 + index, UDPPortMin: 20000 + index*1024, UDPPortMax: 21023 + index*1024,
			Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto",
			JumpPolicy: "auto", Nodes: []LineNode{
				{DeviceID: "gz", Role: "entry", NextHopDevice: "hk", JumpCandidates: json.RawMessage(`[]`), Config: json.RawMessage(`{}`)},
				{DeviceID: "hk", Role: "relay", NextHopDevice: exit, JumpCandidates: json.RawMessage(`["gz"]`), Config: json.RawMessage(`{}`)},
				{DeviceID: exit, Role: "exit", JumpCandidates: json.RawMessage(`["hk"]`), Config: json.RawMessage(`{}`)},
			}}
		if _, err = store.SaveLineSpec(t.Context(), spec); err != nil {
			t.Fatal(err)
		}
	}
	topology, err := store.Topology(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(topology.Devices) != 4 || len(topology.Links) != 4 {
		t.Fatalf("devices=%d links=%d topology=%+v", len(topology.Devices), len(topology.Links), topology)
	}
	var shared int
	for _, link := range topology.Links {
		if link.Source == "gz" && link.Target == "hk" {
			shared++
		}
	}
	if shared != 2 {
		t.Fatalf("shared GZ-HK line edges=%d want 2", shared)
	}
}

func TestTopologyLayoutPersistsAndResets(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertDevice(t.Context(), Device{ID: "entry-1", Name: "Entry", Status: "ready",
		Host: "192.0.2.10", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	want := TopologyLayout{DeviceID: "entry-1", X: -120.5, Y: 8, Z: 3, UpdatedBy: "operator"}
	if err = store.SaveTopologyLayouts(t.Context(), []TopologyLayout{want}); err != nil {
		t.Fatal(err)
	}
	topology, err := store.Topology(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(topology.Devices) != 1 || topology.Devices[0].Layout == nil {
		t.Fatalf("layout missing from topology: %+v", topology)
	}
	got := topology.Devices[0].Layout
	if got.X != want.X || got.Y != want.Y || got.Z != want.Z || got.UpdatedBy != want.UpdatedBy || got.UpdatedAt == "" {
		t.Fatalf("layout=%+v want=%+v", got, want)
	}
	if err = store.ResetTopologyLayouts(t.Context()); err != nil {
		t.Fatal(err)
	}
	topology, err = store.Topology(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if topology.Devices[0].Layout != nil {
		t.Fatalf("layout survived reset: %+v", topology.Devices[0].Layout)
	}
}

func TestTopologyLayoutRejectsUnknownDevice(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	err = store.SaveTopologyLayouts(t.Context(), []TopologyLayout{{DeviceID: "missing", X: 1, UpdatedBy: "operator"}})
	if err == nil {
		t.Fatal("unknown device layout was accepted")
	}
}
