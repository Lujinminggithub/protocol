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
