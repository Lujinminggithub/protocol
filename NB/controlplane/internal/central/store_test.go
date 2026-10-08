package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
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

func TestPruneControlHistoryKeepsSummariesAndRecentFailureDetails(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	nowAt := time.Date(2026, 9, 3, 12, 0, 0, 0, time.UTC)
	if _, err = store.db.Exec(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
 VALUES('line-logs','logs','active','entry','exit','test',10,?,?)`, now(), now()); err != nil {
		t.Fatal(err)
	}
	for _, operation := range []struct{ id, status, at string }{
		{"op-success", "succeeded", nowAt.Add(-25 * time.Hour).Format(time.RFC3339Nano)},
		{"op-failed-recent", "failed", nowAt.Add(-10 * 24 * time.Hour).Format(time.RFC3339Nano)},
		{"op-failed-old", "failed", nowAt.Add(-31 * 24 * time.Hour).Format(time.RFC3339Nano)},
	} {
		if _, err = store.db.Exec(`INSERT INTO operations
 (id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at)
 VALUES(?, 'line-logs','line.open',?,'test',?,'{}','{}',?,?)`, operation.id, operation.status,
			operation.id, operation.at, operation.at); err != nil {
			t.Fatal(err)
		}
		if _, err = store.db.Exec(`INSERT INTO operation_events
 (operation_id,sequence,stage,status,message,parameters,created_at) VALUES(?,1,'provision','done','detail','{}',?)`,
			operation.id, operation.at); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = store.db.Exec(`INSERT INTO raw_events(path,idempotency_key,payload,received_at)
	 VALUES('/agent','raw-old','{}',?)`, nowAt.Add(-8*24*time.Hour).Format(time.RFC3339Nano)); err != nil {
		t.Fatal(err)
	}
	if err = store.EnsureInitialUser(t.Context(), "retention-user", []byte("hash")); err != nil {
		t.Fatal(err)
	}
	user, err := store.UserForLogin(t.Context(), "retention-user")
	if err != nil {
		t.Fatal(err)
	}
	if err = store.CreateUserSession(t.Context(), user.UserID, []byte("expired-session"), nowAt.Add(-time.Hour)); err != nil {
		t.Fatal(err)
	}
	if err = store.PruneControlHistory(t.Context(), nowAt); err != nil {
		t.Fatal(err)
	}
	var operations, events, raw, sessions int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM operations`).Scan(&operations); err != nil {
		t.Fatal(err)
	}
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM operation_events`).Scan(&events); err != nil {
		t.Fatal(err)
	}
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM raw_events`).Scan(&raw); err != nil {
		t.Fatal(err)
	}
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM user_sessions`).Scan(&sessions); err != nil {
		t.Fatal(err)
	}
	if operations != 3 || events != 1 || raw != 0 || sessions != 0 {
		t.Fatalf("operations=%d events=%d raw=%d sessions=%d", operations, events, raw, sessions)
	}
}

func TestOperationEventDetailIsSizeBounded(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.db.Exec(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
 VALUES('line-event-size','event','active','entry','exit','test',10,?,?)`, now(), now()); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`INSERT INTO operations
 (id,line_id,kind,status,requested_by,idempotency_key,request,result,created_at,updated_at)
 VALUES('op-event-size','line-event-size','line.open','running','test','op-event-size','{}',NULL,?,?)`, now(), now()); err != nil {
		t.Fatal(err)
	}
	if err = store.RecordOperationEvent(t.Context(), OperationEvent{OperationID: "op-event-size", Sequence: 1,
		Stage: "provision", Status: "running", Message: strings.Repeat("x", 64<<10),
		Parameters: json.RawMessage(`{"detail":"` + strings.Repeat("y", 128<<10) + `"}`)}); err != nil {
		t.Fatal(err)
	}
	var messageBytes, parameterBytes int
	if err = store.db.QueryRow(`SELECT length(message),length(parameters) FROM operation_events WHERE operation_id='op-event-size'`).Scan(&messageBytes, &parameterBytes); err != nil {
		t.Fatal(err)
	}
	if messageBytes > 32<<10 || parameterBytes > 64<<10 {
		t.Fatalf("message=%d parameters=%d", messageBytes, parameterBytes)
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

func TestRuntimePortClaimsParticipateInAllocationAndRequireConfirmedAbsence(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	for _, id := range []string{"line-new", "entry-1", "relay-1", "exit-1"} {
		if id == "line-new" {
			if _, err = store.UpsertLine(ctx, Line{ID: id, Name: id, Status: "draft", Provider: "test"}); err != nil {
				t.Fatal(err)
			}
			continue
		}
		if _, err = store.UpsertDevice(ctx, Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.1",
			SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id, Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	claims := []RuntimePortClaim{{WorkerID: "worker-1", DeviceID: "entry-1", Role: "entry",
		ResourceKind: "socks", InstanceID: "legacy-a", PortStart: 1082, PortEnd: 1083,
		ObservedAt: "2026-09-07T00:00:00Z", ExpiresAt: "2026-09-07T00:05:00Z", Source: "runtime"}}
	if err = store.ReplaceRuntimePortClaims(ctx, "worker-1", "entry-1", "entry", claims); err != nil {
		t.Fatal(err)
	}
	spec := LineSpec{LineID: "line-new", ResourceGroup: "shared", InstanceID: "line-new_1", BandwidthMbps: 5,
		Nodes: []LineNode{{DeviceID: "entry-1", Role: "entry"}, {DeviceID: "relay-1", Role: "relay"}, {DeviceID: "exit-1", Role: "exit"}}}
	allocated, err := store.AllocateLineSpec(ctx, spec)
	if err != nil {
		t.Fatal(err)
	}
	if allocated.SocksPort != 1084 || !allocated.SocksPortAuto {
		t.Fatalf("runtime claim was ignored: %+v", allocated)
	}
	explicit := allocated
	explicit.SocksPort, explicit.SocksPortAuto = 1083, false
	conflict, err := store.LineSpecConflict(ctx, explicit)
	if err != nil || !strings.Contains(conflict, "legacy-a") {
		t.Fatalf("explicit runtime conflict=%q err=%v", conflict, err)
	}
	if err = store.ReplaceRuntimePortClaims(ctx, "worker-1", "entry-1", "entry", nil); err != nil {
		t.Fatal(err)
	}
	allocated.SocksPort = 0
	allocated, err = store.AllocateLineSpec(ctx, allocated)
	if err != nil || allocated.SocksPort != 1082 {
		t.Fatalf("confirmed absence did not release claim: %+v err=%v", allocated, err)
	}
}

func TestPrepareLineOpenOperationReallocatesAutomaticRuntimeConflict(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	if _, err = store.UpsertLine(ctx, Line{ID: "line-cas", Name: "cas", Status: "draft", Provider: "test"}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"entry-cas", "relay-cas", "exit-cas"} {
		if _, err = store.UpsertDevice(ctx, Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.1",
			SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id, Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	spec := LineSpec{LineID: "line-cas", ResourceGroup: "shared", InstanceID: "line-cas_1", BandwidthMbps: 5,
		SocksPort: 1082, SocksPortAuto: true, RelayPort: 4445, ExitPort: 4443, UDPPortMin: 22048, UDPPortMax: 23071,
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto",
		Nodes: []LineNode{{DeviceID: "entry-cas", Role: "entry"}, {DeviceID: "relay-cas", Role: "relay"}, {DeviceID: "exit-cas", Role: "exit"}}}
	if _, err = store.SaveLineSpec(ctx, spec); err != nil {
		t.Fatal(err)
	}
	request, _ := json.Marshal(map[string]any{"plan": spec})
	op := Operation{ID: "op-cas", LineID: spec.LineID, Kind: "line.open", RequestedBy: "operator", IdempotencyKey: "op-cas", Request: request}
	if _, _, err = store.CreateOperation(ctx, op); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`UPDATE operations SET status='dispatched' WHERE id=?`, op.ID); err != nil {
		t.Fatal(err)
	}
	claim := RuntimePortClaim{WorkerID: "worker-1", DeviceID: "entry-cas", Role: "entry", ResourceKind: "socks",
		InstanceID: "legacy-cas", PortStart: 1082, PortEnd: 1082, ObservedAt: "2026-09-07T00:00:00Z",
		ExpiresAt: "2026-09-07T00:05:00Z", Source: "runtime"}
	if err = store.ReplaceRuntimePortClaims(ctx, "worker-1", "entry-cas", "entry", []RuntimePortClaim{claim}); err != nil {
		t.Fatal(err)
	}
	prepared, err := store.PrepareLineOpenOperation(ctx, op.ID, op.LineID)
	if err != nil {
		t.Fatal(err)
	}
	updated, err := store.LineSpec(ctx, spec.LineID)
	if err != nil || updated.SocksPort != 1083 {
		t.Fatalf("updated spec=%+v err=%v", updated, err)
	}
	var values struct {
		Plan LineSpec `json:"plan"`
	}
	if err = json.Unmarshal(prepared.Request, &values); err != nil || values.Plan.SocksPort != 1083 {
		t.Fatalf("prepared plan=%+v err=%v", values.Plan, err)
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

func TestLineSpecEnvironmentDefaultsAndPersists(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-env", Name: "Environment", Status: "draft", EntryRegion: "test", ExitRegion: "test", Provider: "lab", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"entry", "exit"} {
		if _, err = store.UpsertDevice(t.Context(), Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.10", SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id}); err != nil {
			t.Fatal(err)
		}
	}
	spec := LineSpec{LineID: "line-env", ResourceGroup: "group", InstanceID: "line-env-1", BandwidthMbps: 10,
		SocksPort: 1080, UDPPortMin: 22000, UDPPortMax: 23023, RelayPort: 4443, ExitPort: 4444,
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Environment: "test",
		Whitelist: json.RawMessage(`[]`), Nodes: []LineNode{{DeviceID: "entry", Role: "entry"}, {DeviceID: "exit", Role: "exit"}}}
	if _, err = store.SaveLineSpec(t.Context(), spec); err != nil {
		t.Fatal(err)
	}
	loaded, err := store.LineSpec(t.Context(), spec.LineID)
	if err != nil || loaded.Environment != "test" {
		t.Fatalf("environment=%q err=%v", loaded.Environment, err)
	}
	line, err := store.Line(t.Context(), spec.LineID)
	if err != nil || line.Environment != "test" {
		t.Fatalf("line environment=%q err=%v", line.Environment, err)
	}
	if _, err = store.SaveLineSpec(t.Context(), LineSpec{LineID: "line-env", ResourceGroup: "group", InstanceID: "line-env-1", BandwidthMbps: 10,
		SocksPort: 1080, UDPPortMin: 22000, UDPPortMax: 23023, RelayPort: 4443, ExitPort: 4444,
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Environment: "invalid"}); err == nil {
		t.Fatal("invalid environment was accepted")
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

func TestOpenSeparatesBoundedTelemetryFromControlData(t *testing.T) {
	directory := t.TempDir()
	controlPath := filepath.Join(directory, "central.db")
	store, err := Open(controlPath)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-split", Name: "split", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	payload := json.RawMessage(`{"diagnostic":"latest-only"}`)
	item := Snapshot{LineID: "line-split", NodeID: "entry-0", Role: "entry", WorkerID: "0",
		ObservedAt: time.Now().UTC().Format(time.RFC3339Nano), Health: "ok", Payload: payload}
	if inserted, recordErr := store.RecordSnapshot(t.Context(), item); recordErr != nil || !inserted {
		t.Fatalf("RecordSnapshot inserted=%v err=%v", inserted, recordErr)
	}
	var controlSnapshots int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM snapshots`).Scan(&controlSnapshots); err != nil {
		t.Fatal(err)
	}
	if controlSnapshots != 0 {
		t.Fatalf("control database contains %d telemetry rows", controlSnapshots)
	}
	var historicalPayload string
	if err = store.telemetryDB.QueryRow(`SELECT payload FROM snapshots`).Scan(&historicalPayload); err != nil {
		t.Fatal(err)
	}
	if historicalPayload != `{}` {
		t.Fatalf("historical payload=%q want compact placeholder", historicalPayload)
	}
	latest, err := store.LatestSnapshots(t.Context(), "line-split")
	if err != nil || len(latest) != 1 || string(latest[0].Payload) != string(payload) {
		t.Fatalf("latest snapshots=%+v err=%v", latest, err)
	}
	if _, err = os.Stat(filepath.Join(directory, "central-telemetry.db")); err != nil {
		t.Fatalf("telemetry database missing: %v", err)
	}
	for name, database := range map[string]*sql.DB{"control": store.db, "telemetry": store.telemetryDB} {
		var pageSize, maxPages int64
		if err = database.QueryRow(`PRAGMA page_size`).Scan(&pageSize); err != nil {
			t.Fatal(err)
		}
		if err = database.QueryRow(`PRAGMA max_page_count`).Scan(&maxPages); err != nil {
			t.Fatal(err)
		}
		if size := pageSize * maxPages; size > 1<<30 {
			t.Fatalf("%s database cap=%d exceeds 1 GiB", name, size)
		}
	}
}

func TestCompleteOperationIsIdempotentAfterResultWasPersisted(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	stamp := now()
	_, err = store.db.Exec(`INSERT INTO lines
 (id,name,status,entry_region,exit_region,provider,capacity_mbps,created_at,updated_at)
 VALUES('line-idempotent','test','draft','entry','exit','test',10,?,?)`, stamp, stamp)
	if err != nil {
		t.Fatal(err)
	}
	operation := Operation{ID: "op-idempotent", LineID: "line-idempotent", Kind: "line.open",
		RequestedBy: "test", IdempotencyKey: "idempotent-1", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(context.Background(), operation); err != nil {
		t.Fatal(err)
	}
	if _, err = store.ClaimOperations(context.Background(), operation.LineID, 1); err != nil {
		t.Fatal(err)
	}
	result := json.RawMessage(`{"deployment":"d1","profile":"p1"}`)
	if err = store.CompleteOperation(context.Background(), operation.ID, operation.LineID, "failed", result); err != nil {
		t.Fatal(err)
	}
	if err = store.CompleteOperation(context.Background(), operation.ID, operation.LineID, "failed", result); err != nil {
		t.Fatalf("repeat completion should be idempotent: %v", err)
	}
}

func TestSuccessfulTuneAdvancesTransportGenerationFloor(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-generation", Name: "test", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	op := Operation{ID: "op-generation", LineID: "line-generation", Kind: "line.tune", RequestedBy: "test",
		IdempotencyKey: "generation", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(t.Context(), op); err != nil {
		t.Fatal(err)
	}
	if _, err = store.ClaimOperations(t.Context(), op.LineID, 1); err != nil {
		t.Fatal(err)
	}
	if err = store.CompleteOperation(t.Context(), op.ID, op.LineID, "succeeded",
		json.RawMessage(`{"profile":"line-generation:9","transport_generation":9}`)); err != nil {
		t.Fatal(err)
	}
	generation, err := store.AllocateTransportGeneration(t.Context(), op.LineID)
	if err != nil || generation != 10 {
		t.Fatalf("generation=%d err=%v want 10", generation, err)
	}
}

func TestSuccessfulOptimizeUpdatesProfileAndGeneration(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-optimize", Name: "test", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	op := Operation{ID: "op-optimize", LineID: "line-optimize", Kind: "line.optimize", RequestedBy: "test",
		IdempotencyKey: "optimize", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(t.Context(), op); err != nil {
		t.Fatal(err)
	}
	if _, err = store.ClaimOperations(t.Context(), op.LineID, 1); err != nil {
		t.Fatal(err)
	}
	if err = store.CompleteOperation(t.Context(), op.ID, op.LineID, "succeeded",
		json.RawMessage(`{"profile":"line-optimize:4","transport_generation":4}`)); err != nil {
		t.Fatal(err)
	}
	line, err := store.Line(t.Context(), op.LineID)
	if err != nil || line.Profile != "line-optimize:4" {
		t.Fatalf("line=%+v err=%v", line, err)
	}
	generation, err := store.AllocateTransportGeneration(t.Context(), op.LineID)
	if err != nil || generation != 5 {
		t.Fatalf("generation=%d err=%v", generation, err)
	}
}

func TestOpenCreatesLatestSnapshotIndex(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	var count int
	err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='snapshots_latest_node'`).Scan(&count)
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
	_, err = store.telemetryDB.Exec(`WITH RECURSIVE a(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM a WHERE i<250),
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
	if err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM latest_snapshots`).Scan(&latestCount); err != nil {
		t.Fatal(err)
	}
	if err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM snapshots`).Scan(&historyCount); err != nil {
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
	rows, err := store.telemetryDB.Query(`EXPLAIN QUERY PLAN SELECT s.role FROM latest_snapshots latest
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
	if err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM snapshots WHERE line_id='line-retention'`).Scan(&raw); err != nil {
		t.Fatal(err)
	}
	if raw != 1 {
		t.Fatalf("raw snapshots=%d want latest only", raw)
	}
	var rollups int
	if err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM traffic_rollups WHERE line_id='line-retention'`).Scan(&rollups); err != nil {
		t.Fatal(err)
	}
	if rollups == 0 {
		t.Fatal("recent rollups were removed")
	}
}

func TestPruneTrafficBatchCommitsBoundedProgress(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-batch-retention", Name: "retention", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	nowAt := time.Date(2026, 9, 3, 12, 0, 0, 0, time.UTC)
	for index := 0; index < 5; index++ {
		item := Snapshot{LineID: "line-batch-retention", NodeID: "entry-0", Role: "entry", WorkerID: "0",
			ObservedAt: nowAt.Add(time.Duration(-9*24*time.Hour) + time.Duration(index)*time.Second).Format(time.RFC3339Nano),
			Health:     "ok", UpstreamMbps: 1}
		if _, err = store.RecordSnapshot(t.Context(), item); err != nil {
			t.Fatal(err)
		}
	}

	deleted, err := store.pruneTrafficBatch(t.Context(), nowAt, 2)
	if err != nil {
		t.Fatal(err)
	}
	if deleted != 2 {
		t.Fatalf("deleted=%d want 2", deleted)
	}
	var remaining int
	if err = store.telemetryDB.QueryRow(`SELECT COUNT(*) FROM snapshots WHERE line_id='line-batch-retention'`).Scan(&remaining); err != nil {
		t.Fatal(err)
	}
	if remaining != 3 {
		t.Fatalf("remaining=%d want 3", remaining)
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
	if _, err = store.telemetryDB.Exec(`DELETE FROM traffic_rollups WHERE line_id='line-backfill'`); err != nil {
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
	if err = store.telemetryDB.QueryRow(`SELECT sample_count,upstream_sum,health_rank FROM traffic_rollups
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

func TestLinesSharingDeviceRolesReturnsExactBlastRadius(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, id := range []string{"gz", "hk", "us", "uk", "other-entry", "other-relay", "other-exit"} {
		if _, err = store.UpsertDevice(t.Context(), Device{ID: id, Name: id, Status: "ready",
			Host: "192.0.2.1", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	topologies := map[string][]LineNode{
		"line-uk":    {{DeviceID: "gz", Role: "entry"}, {DeviceID: "hk", Role: "relay"}, {DeviceID: "uk", Role: "exit"}},
		"line-us":    {{DeviceID: "gz", Role: "entry"}, {DeviceID: "hk", Role: "relay"}, {DeviceID: "us", Role: "exit"}},
		"line-other": {{DeviceID: "other-entry", Role: "entry"}, {DeviceID: "other-relay", Role: "relay"}, {DeviceID: "other-exit", Role: "exit"}},
	}
	index := 0
	for lineID, nodes := range topologies {
		if _, err = store.UpsertLine(t.Context(), Line{ID: lineID, Name: lineID, Status: "active",
			EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
			t.Fatal(err)
		}
		spec := LineSpec{LineID: lineID, ResourceGroup: "legacy", InstanceID: lineID + "_1",
			BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1082 + index,
			RelayPort: 4445 + index*2, ExitPort: 4443 + index*2,
			UDPPortMin: 22048 + index*1024, UDPPortMax: 23071 + index*1024,
			Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`),
			BuildMode: "auto", JumpPolicy: "auto", Nodes: nodes}
		if _, err = store.SaveLineSpec(t.Context(), spec); err != nil {
			t.Fatal(err)
		}
		index++
	}
	lines, err := store.LinesSharingDeviceRoles(t.Context(), "line-uk")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(lines, ",") != "line-uk,line-us" {
		t.Fatalf("affected lines=%v", lines)
	}
}

func TestReserveLineCapacitySeparatesDirectionsAndIsIdempotent(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, id := range []string{"gz", "hk", "exit"} {
		if _, err = store.UpsertDevice(t.Context(), Device{ID: id, Name: id, Status: "ready",
			Environment: "production", Host: "192.0.2.1", SSHPort: 22, SSHUser: "root",
			Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	nodes := []LineNode{{DeviceID: "gz", Role: "entry"}, {DeviceID: "hk", Role: "relay"},
		{DeviceID: "exit", Role: "exit"}}
	for index, item := range []struct {
		id         string
		upstream   int
		downstream int
	}{{"line-a", 10, 6}, {"line-b", 10, 5}} {
		if _, err = store.UpsertLine(t.Context(), Line{ID: item.id, Name: item.id, Status: "draft",
			Environment: "production", EntryRegion: "gz", ExitRegion: "exit", Provider: "test",
			CapacityMbps: 10}); err != nil {
			t.Fatal(err)
		}
		if _, err = store.SaveLineSpec(t.Context(), LineSpec{LineID: item.id, Environment: "production",
			ResourceGroup: "legacy", InstanceID: item.id + "_1", BandwidthMbps: 10,
			UpstreamMbps: item.upstream, DownstreamMbps: item.downstream,
			SocksPort: 1082 + index, RelayPort: 4445 + index*2, ExitPort: 4443 + index*2,
			UDPPortMin: 22048 + index*1024, UDPPortMax: 23071 + index*1024,
			Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`),
			BuildMode: "auto", JumpPolicy: "auto", Nodes: nodes}); err != nil {
			t.Fatal(err)
		}
	}
	for _, link := range []NetworkLink{
		{ID: "gz-hk", FromDeviceID: "gz", FromRole: "entry", ToDeviceID: "hk", ToRole: "relay",
			ForwardCapacityMbps: 20, ReverseCapacityMbps: 10, BillingMode: LinkBillingIndependent,
			Environment: "production", Status: "ready"},
		{ID: "hk-exit", FromDeviceID: "hk", FromRole: "relay", ToDeviceID: "exit", ToRole: "exit",
			ForwardCapacityMbps: 100, ReverseCapacityMbps: 100, BillingMode: LinkBillingIndependent,
			Environment: "production", Status: "ready"},
	} {
		if _, err = store.UpsertNetworkLink(t.Context(), link); err != nil {
			t.Fatal(err)
		}
	}
	first, err := store.ReserveLineCapacity(t.Context(), "line-a", "op-a")
	if err != nil || len(first) != 2 {
		t.Fatalf("first reservations=%+v err=%v", first, err)
	}
	links, err := store.NetworkLinks(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var shared NetworkLink
	for _, link := range links {
		if link.ID == "gz-hk" {
			shared = link
		}
	}
	if shared.ForwardReservedMbps != 10 || shared.ForwardAvailableMbps != 10 ||
		shared.ReverseReservedMbps != 6 || shared.ReverseAvailableMbps != 4 {
		t.Fatalf("shared link usage=%+v", shared)
	}
	if _, err = store.ReserveLineCapacity(t.Context(), "line-a", "op-a"); err != nil {
		t.Fatalf("idempotent retry failed: %v", err)
	}
	var count int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM line_capacity_reservations WHERE line_id='line-a'`).Scan(&count); err != nil || count != 2 {
		t.Fatalf("reservation count=%d err=%v", count, err)
	}
	if _, err = store.ReserveLineCapacity(t.Context(), "line-b", "op-b"); err == nil ||
		!strings.Contains(err.Error(), "reverse required=5 available=4") {
		t.Fatalf("reverse overcommit error=%v", err)
	}
	if err = store.ReleaseLineCapacity(t.Context(), "line-a", "op-release"); err != nil {
		t.Fatal(err)
	}
	if err = store.ReleaseLineCapacity(t.Context(), "line-a", "op-release"); err != nil {
		t.Fatalf("idempotent release failed: %v", err)
	}
	if _, err = store.ReserveLineCapacity(t.Context(), "line-b", "op-b"); err != nil {
		t.Fatalf("released capacity was not reusable: %v", err)
	}
}

func TestLineQualificationPersistsAndSelectsLatestDeterministically(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if _, err = store.UpsertLine(t.Context(), Line{ID: "line-qualified", Name: "qualified",
		Status: "qualification_pending", Environment: "production", EntryRegion: "gz",
		ExitRegion: "es", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	for _, operationID := range []string{"op-a", "op-b"} {
		if _, _, err = store.CreateOperation(t.Context(), Operation{ID: operationID,
			LineID: "line-qualified", Kind: "line.optimize", RequestedBy: "test",
			IdempotencyKey: "qualification-" + operationID, Request: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
		if err = store.CancelOperation(t.Context(), operationID, "qualification fixture"); err != nil {
			t.Fatal(err)
		}
	}
	first := LineQualification{LineID: "line-qualified", DeploymentID: "deployment-1",
		OperationID: "op-a", TargetUpstreamMbps: 10, TargetDownstreamMbps: 10,
		AchievedUpstreamMbps: 9.5, AchievedDownstreamMbps: 9.5, DurationSeconds: 90,
		RequiredRatio: 0.95, Status: "admitted", Reasons: json.RawMessage(`[]`),
		Evidence: json.RawMessage(`{"probe":"concurrent"}`)}
	if first, err = store.SaveLineQualification(t.Context(), first); err != nil {
		t.Fatal(err)
	}
	second := first
	second.OperationID = "op-b"
	second.Status = "rejected"
	second.AchievedDownstreamMbps = 9.49
	second.Reasons = json.RawMessage(`["insufficient-downlink"]`)
	if second, err = store.SaveLineQualification(t.Context(), second); err != nil {
		t.Fatal(err)
	}
	stamp := "2026-10-08T00:00:00Z"
	if _, err = store.db.ExecContext(t.Context(), `UPDATE line_qualifications SET created_at=? WHERE line_id=?`,
		stamp, "line-qualified"); err != nil {
		t.Fatal(err)
	}
	latest, err := store.LatestLineQualification(t.Context(), "line-qualified")
	if err != nil {
		t.Fatal(err)
	}
	if latest.OperationID != "op-b" || latest.Status != "rejected" ||
		latest.AchievedDownstreamMbps != 9.49 || string(latest.Reasons) != `["insufficient-downlink"]` {
		t.Fatalf("latest qualification=%+v", latest)
	}
	if _, err = store.SaveLineQualification(t.Context(), LineQualification{LineID: "line-qualified",
		DeploymentID: "deployment-1", OperationID: "op-invalid", TargetUpstreamMbps: 10,
		TargetDownstreamMbps: 10, AchievedUpstreamMbps: 10, AchievedDownstreamMbps: 10,
		DurationSeconds: 90, RequiredRatio: 0.95, Status: "admitted",
		Reasons: json.RawMessage(`not-json`), Evidence: json.RawMessage(`{}`)}); err == nil {
		t.Fatal("malformed reasons were accepted")
	}
	if _, err = store.SaveLineQualification(t.Context(), LineQualification{LineID: "line-qualified",
		DeploymentID: "deployment-1", OperationID: "op-invalid-ratio", TargetUpstreamMbps: 10,
		TargetDownstreamMbps: 10, AchievedUpstreamMbps: 10, AchievedDownstreamMbps: 10,
		DurationSeconds: 90, RequiredRatio: 1.1, Status: "admitted",
		Reasons: json.RawMessage(`[]`), Evidence: json.RawMessage(`{}`)}); err == nil {
		t.Fatal("invalid required ratio was accepted")
	}
}

func TestTwoPhaseProductionOpenRequiresMatchingQualification(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	lineID := "line-two-phase"
	if _, err = store.UpsertLine(ctx, Line{ID: lineID, Name: "two phase", Status: "draft",
		Environment: "production", EntryRegion: "gz", ExitRegion: "es", Provider: "test",
		CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"entry-phase", "relay-phase", "exit-phase"} {
		if _, err = store.UpsertDevice(ctx, Device{ID: id, Name: id, Status: "ready",
			Environment: "production", Host: "192.0.2.1", SSHPort: 22, SSHUser: "root",
			Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = store.SaveLineSpec(ctx, LineSpec{LineID: lineID, Environment: "production",
		ResourceGroup: "managed", InstanceID: lineID + "_1", BandwidthMbps: 10,
		UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
		UDPPortMin: 22048, UDPPortMax: 23071, Whitelist: json.RawMessage(`[]`),
		DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", JumpPolicy: "auto",
		Nodes: []LineNode{{DeviceID: "entry-phase", Role: "entry"},
			{DeviceID: "relay-phase", Role: "relay"}, {DeviceID: "exit-phase", Role: "exit"}}}); err != nil {
		t.Fatal(err)
	}
	for _, link := range []NetworkLink{
		{ID: "phase-entry-relay", FromDeviceID: "entry-phase", FromRole: "entry",
			ToDeviceID: "relay-phase", ToRole: "relay", ForwardCapacityMbps: 10,
			ReverseCapacityMbps: 10, BillingMode: LinkBillingIndependent, Environment: "production", Status: "ready"},
		{ID: "phase-relay-exit", FromDeviceID: "relay-phase", FromRole: "relay",
			ToDeviceID: "exit-phase", ToRole: "exit", ForwardCapacityMbps: 10,
			ReverseCapacityMbps: 10, BillingMode: LinkBillingIndependent, Environment: "production", Status: "ready"},
	} {
		if _, err = store.UpsertNetworkLink(ctx, link); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = store.UpsertNetworkLink(ctx, NetworkLink{ID: "phase-historical", FromDeviceID: "entry-phase",
		FromRole: "entry", ToDeviceID: "exit-phase", ToRole: "relay", ForwardCapacityMbps: 10,
		ReverseCapacityMbps: 10, BillingMode: LinkBillingIndependent, Environment: "production",
		Status: "ready"}); err != nil {
		t.Fatal(err)
	}
	historicalStamp := now()
	if _, err = store.db.ExecContext(ctx, `INSERT INTO line_capacity_reservations
 (line_id,link_id,forward_mbps,reverse_mbps,state,operation_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?)`, lineID, "phase-historical", 10, 10, "released", "old-open", historicalStamp, historicalStamp); err != nil {
		t.Fatal(err)
	}
	if _, err = store.UpsertLine(ctx, Line{ID: "line-cancel-open", Name: "cancel open", Status: "draft",
		Environment: "production", EntryRegion: "gz", ExitRegion: "es", Provider: "test",
		CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	stamp := now()
	for _, linkID := range []string{"phase-entry-relay", "phase-relay-exit"} {
		if _, err = store.db.ExecContext(ctx, `INSERT INTO line_capacity_reservations
 (line_id,link_id,forward_mbps,reverse_mbps,state,operation_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?)`, "line-cancel-open", linkID, 10, 10, "reserved", "op-cancel-open", stamp, stamp); err != nil {
			t.Fatal(err)
		}
	}
	cancelOpen := Operation{ID: "op-cancel-open", LineID: "line-cancel-open", Kind: "line.open",
		RequestedBy: "operator", IdempotencyKey: "cancel-two-phase", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(ctx, cancelOpen); err != nil {
		t.Fatal(err)
	}
	if err = store.CancelOperation(ctx, cancelOpen.ID, "operator cancelled"); err != nil {
		t.Fatal(err)
	}
	cancelledLine, err := store.Line(ctx, cancelOpen.LineID)
	if err != nil || cancelledLine.Status != "maintenance" {
		t.Fatalf("cancelled open line=%+v err=%v", cancelledLine, err)
	}
	var released int
	if err = store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM line_capacity_reservations WHERE line_id=? AND state='released'`,
		cancelOpen.LineID).Scan(&released); err != nil || released != 2 {
		t.Fatalf("cancelled open released=%d err=%v", released, err)
	}
	if _, err = store.ReserveLineCapacity(ctx, lineID, "op-open-phase"); err != nil {
		t.Fatal(err)
	}
	open := Operation{ID: "op-open-phase", LineID: lineID, Kind: "line.open", RequestedBy: "operator",
		IdempotencyKey: "open-two-phase", Request: json.RawMessage(`{"plan":{"line_id":"line-two-phase"}}`)}
	if _, _, err = store.CreateOperation(ctx, open); err != nil {
		t.Fatal(err)
	}
	if line, lineErr := store.Line(ctx, lineID); lineErr != nil || line.Status != "provisioning" {
		t.Fatalf("queued open line=%+v err=%v", line, lineErr)
	}
	if _, err = store.ClaimOperations(ctx, lineID, 1); err != nil {
		t.Fatal(err)
	}
	openResult := json.RawMessage(`{"deployment":"deployment-phase","profile":"line-two-phase:1","client_url":"socks5://user:secret@192.0.2.1:1082"}`)
	if err = store.CompleteOperation(ctx, open.ID, lineID, "succeeded", openResult); err != nil {
		t.Fatal(err)
	}
	line, err := store.Line(ctx, lineID)
	if err != nil || line.Status != "qualification_pending" || line.ActiveDeployment != "deployment-phase" {
		t.Fatalf("open completion line=%+v err=%v", line, err)
	}
	if err = store.ClientDeliveryAllowed(ctx, lineID); err == nil {
		t.Fatal("pending production line exposed client material")
	}
	qualify, err := store.OperationByIdempotencyKey(ctx, "auto-qualify-"+lineID+"-deployment-phase")
	if err != nil || qualify.Kind != "line.optimize" || qualify.Status != "queued" ||
		!strings.Contains(string(qualify.Request), `"deployment_id":"deployment-phase"`) {
		t.Fatalf("automatic qualification=%+v err=%v", qualify, err)
	}
	if _, err = store.ClaimOperations(ctx, lineID, 1); err != nil {
		t.Fatal(err)
	}
	evidence := `{"admission":{"status":"admitted","reasons":[],"target_upstream_mbps":10,"target_downstream_mbps":10,"achieved_upstream_mbps":9.5,"achieved_downstream_mbps":9.5,"required_ratio":0.95,"duration_seconds":90}}`
	if err = store.CompleteOperation(ctx, qualify.ID, lineID, "succeeded", json.RawMessage(
		`{"profile":"line-two-phase:2","transport_generation":2,"evidence":`+evidence+`}`)); err != nil {
		t.Fatal(err)
	}
	line, err = store.Line(ctx, lineID)
	if err != nil || line.Status != "active" || line.Profile != "line-two-phase:2" {
		t.Fatalf("qualified line=%+v err=%v", line, err)
	}
	if err = store.ClientDeliveryAllowed(ctx, lineID); err != nil {
		t.Fatalf("admitted production line did not expose client material: %v", err)
	}
	qualification, err := store.LatestLineQualification(ctx, lineID)
	if err != nil || qualification.Status != "admitted" || qualification.DeploymentID != "deployment-phase" {
		t.Fatalf("qualification=%+v err=%v", qualification, err)
	}
	var activeReservations int
	if err = store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM line_capacity_reservations WHERE line_id=? AND state='active'`, lineID).Scan(&activeReservations); err != nil || activeReservations != 2 {
		t.Fatalf("active reservations=%d err=%v", activeReservations, err)
	}
	var historicalReleased int
	if err = store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM line_capacity_reservations WHERE line_id=? AND state='released'`, lineID).Scan(&historicalReleased); err != nil || historicalReleased != 1 {
		t.Fatalf("historical released reservations=%d err=%v", historicalReleased, err)
	}
}

func TestTwoPhaseRejectsFailedAndStaleQualifications(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	for _, test := range []struct {
		lineID, deployment, requestDeployment, admission, target, completionStatus, wantStatus string
		wantError                                                                              bool
	}{
		{"line-rejected", "deployment-current", "deployment-current", "rejected", "10", "failed", "qualification_failed", false},
		{"line-stale", "deployment-current", "deployment-old", "admitted", "10", "succeeded", "qualification_pending", false},
		{"line-low-target", "deployment-current", "deployment-current", "admitted", "1", "succeeded", "qualification_failed", false},
	} {
		if _, err = store.UpsertLine(ctx, Line{ID: test.lineID, Name: test.lineID,
			Status: "qualification_pending", Environment: "production", EntryRegion: "gz",
			ExitRegion: "es", Provider: "test", CapacityMbps: 10,
			ActiveDeployment: test.deployment}); err != nil {
			t.Fatal(err)
		}
		request, _ := json.Marshal(map[string]any{"deployment_id": test.requestDeployment})
		op := Operation{ID: "op-" + test.lineID, LineID: test.lineID, Kind: "line.optimize",
			RequestedBy: "system", IdempotencyKey: "qualify-" + test.lineID, Request: request}
		if _, _, err = store.CreateOperation(ctx, op); err != nil {
			t.Fatal(err)
		}
		if _, err = store.ClaimOperations(ctx, test.lineID, 1); err != nil {
			t.Fatal(err)
		}
		reasons := `[]`
		if test.admission == "rejected" {
			reasons = `["insufficient-downlink"]`
		}
		result := json.RawMessage(`{"profile":"` + test.lineID + `:2","transport_generation":2,"evidence":{"admission":{"status":"` + test.admission + `","reasons":` + reasons + `,"target_upstream_mbps":` + test.target + `,"target_downstream_mbps":` + test.target + `,"achieved_upstream_mbps":10,"achieved_downstream_mbps":9.49,"required_ratio":0.95,"duration_seconds":90}}}`)
		err = store.CompleteOperation(ctx, op.ID, test.lineID, test.completionStatus, result)
		if (err != nil) != test.wantError {
			t.Fatalf("%s completion error=%v wantError=%v", test.lineID, err, test.wantError)
		}
		line, lineErr := store.Line(ctx, test.lineID)
		if lineErr != nil || line.Status != test.wantStatus {
			t.Fatalf("%s line=%+v err=%v", test.lineID, line, lineErr)
		}
		storedOperation, operationErr := store.Operation(ctx, op.ID)
		if operationErr != nil || (test.lineID == "line-stale" && storedOperation.Status != "failed") {
			t.Fatalf("%s operation=%+v err=%v", test.lineID, storedOperation, operationErr)
		}
	}
}

func TestGovernanceAuditsHistoricalProductionLines(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	seed := func(lineID, lineEnvironment, entryEnvironment, billingMode string, createLinks bool,
		qualificationDeployment string) {
		deployment := lineID + "-deployment"
		if _, seedErr := store.UpsertLine(ctx, Line{ID: lineID, Name: lineID, Status: "active",
			Environment: lineEnvironment, EntryRegion: "gz", ExitRegion: "exit", Provider: "test",
			CapacityMbps: 10, ActiveDeployment: deployment, Profile: lineID + ":2"}); seedErr != nil {
			t.Fatal(seedErr)
		}
		deviceIDs := []string{lineID + "-entry", lineID + "-relay", lineID + "-exit"}
		for index, deviceID := range deviceIDs {
			environment := "production"
			if index == 0 {
				environment = entryEnvironment
			}
			if _, seedErr := store.UpsertDevice(ctx, Device{ID: deviceID, Name: deviceID, Status: "ready",
				Environment: environment, Host: "192.0.2.1", SSHPort: 22, SSHUser: "root",
				Labels: json.RawMessage(`{}`)}); seedErr != nil {
				t.Fatal(seedErr)
			}
		}
		if _, seedErr := store.SaveLineSpec(ctx, LineSpec{LineID: lineID, Environment: lineEnvironment,
			ResourceGroup: "managed", InstanceID: lineID + "_1", BandwidthMbps: 10,
			UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
			UDPPortMin: 22048, UDPPortMax: 23071, Whitelist: json.RawMessage(`[]`),
			DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", JumpPolicy: "auto",
			Nodes: []LineNode{{DeviceID: deviceIDs[0], Role: "entry"},
				{DeviceID: deviceIDs[1], Role: "relay"}, {DeviceID: deviceIDs[2], Role: "exit"}}}); seedErr != nil {
			t.Fatal(seedErr)
		}
		if createLinks {
			for index, endpoints := range [][4]string{{deviceIDs[0], "entry", deviceIDs[1], "relay"},
				{deviceIDs[1], "relay", deviceIDs[2], "exit"}} {
				if _, seedErr := store.UpsertNetworkLink(ctx, NetworkLink{ID: lineID + "-link-" + string(rune('a'+index)),
					FromDeviceID: endpoints[0], FromRole: endpoints[1], ToDeviceID: endpoints[2], ToRole: endpoints[3],
					ForwardCapacityMbps: 10, ReverseCapacityMbps: 10, BillingMode: billingMode,
					Environment: "production", Status: "ready"}); seedErr != nil {
					t.Fatal(seedErr)
				}
			}
		}
		if qualificationDeployment != "" {
			operationID := "op-" + lineID
			if _, _, seedErr := store.CreateOperation(ctx, Operation{ID: operationID, LineID: lineID,
				Kind: "line.optimize", RequestedBy: "governance", IdempotencyKey: "governance-" + lineID,
				Request: json.RawMessage(`{}`)}); seedErr != nil {
				t.Fatal(seedErr)
			}
			if _, seedErr := store.ClaimOperations(ctx, lineID, 1); seedErr != nil {
				t.Fatal(seedErr)
			}
			if seedErr := store.CompleteOperation(ctx, operationID, lineID, "succeeded",
				json.RawMessage(`{"profile":"`+lineID+`:2"}`)); seedErr != nil {
				t.Fatal(seedErr)
			}
			if _, seedErr := store.SaveLineQualification(ctx, LineQualification{LineID: lineID,
				DeploymentID: qualificationDeployment, OperationID: operationID,
				TargetUpstreamMbps: 10, TargetDownstreamMbps: 10, AchievedUpstreamMbps: 10,
				AchievedDownstreamMbps: 10, DurationSeconds: 90, RequiredRatio: 0.95,
				Status: "admitted", Reasons: json.RawMessage(`[]`), Evidence: json.RawMessage(`{}`)}); seedErr != nil {
				t.Fatal(seedErr)
			}
		}
	}
	seed("gov-test-device", "production", "test", LinkBillingIndependent, true, "gov-test-device-deployment")
	seed("gov-missing-link", "production", "production", LinkBillingIndependent, false, "gov-missing-link-deployment")
	seed("gov-aggregate", "production", "production", LinkBillingAggregate, true, "gov-aggregate-deployment")
	seed("gov-stale", "production", "production", LinkBillingIndependent, true, "old-deployment")
	seed("gov-qualified", "production", "production", LinkBillingIndependent, true, "gov-qualified-deployment")
	seed("gov-weak", "production", "production", LinkBillingIndependent, true, "gov-weak-deployment")
	if _, err = store.db.ExecContext(ctx, `UPDATE line_qualifications SET achieved_downstream_mbps=1 WHERE line_id='gov-weak'`); err != nil {
		t.Fatal(err)
	}
	seed("gov-test-line", "test", "test", LinkBillingUnknown, false, "")

	findings, err := store.AuditProductionLines(ctx)
	if err != nil {
		t.Fatal(err)
	}
	codeSets := map[string]map[string]bool{}
	for _, finding := range findings {
		if codeSets[finding.LineID] == nil {
			codeSets[finding.LineID] = map[string]bool{}
		}
		codeSets[finding.LineID][finding.Code] = true
	}
	byLine := map[string][]string{}
	for lineID, codes := range codeSets {
		for code := range codes {
			byLine[lineID] = append(byLine[lineID], code)
		}
		sort.Strings(byLine[lineID])
	}
	want := map[string][]string{
		"gov-test-device":  {"maintenance_required"},
		"gov-missing-link": {"capacity_unknown"},
		"gov-aggregate":    {"full_duplex_unqualified"},
		"gov-stale":        {"qualification_required"},
		"gov-weak":         {"qualification_required"},
	}
	if !reflect.DeepEqual(byLine, want) {
		t.Fatalf("governance findings=%v want=%v", byLine, want)
	}
	if err = store.ClientDeliveryAllowed(ctx, "gov-aggregate"); err == nil {
		t.Fatal("aggregate historical line bypassed client delivery governance")
	}
	if err = store.ClientDeliveryAllowed(ctx, "gov-qualified"); err != nil {
		t.Fatalf("qualified historical line was blocked: %v", err)
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

func TestLineSpecTopologyAndServiceProfileRoundTrip(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	if _, err = store.UpsertLine(ctx, Line{ID: "single-hk", Name: "single-hk", Status: "draft",
		Environment: "test", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	spec := LineSpec{LineID: "single-hk", Environment: "test", ResourceGroup: "hk",
		InstanceID: "single-hk_1", BandwidthMbps: 5, SocksPort: 1082,
		UDPPortMin: 22048, UDPPortMax: 23071, ExitPort: 4443,
		TopologyMode: "single_hk", ServiceProfile: "general",
		Whitelist: json.RawMessage(`[]`), BuildMode: "auto", SourceRef: "repo://current",
		JumpPolicy: "direct"}
	stored, err := store.SaveLineSpec(ctx, spec)
	if err != nil {
		t.Fatal(err)
	}
	if stored.TopologyMode != "single_hk" || stored.ServiceProfile != "general" {
		t.Fatalf("topology/profile lost: %+v", stored)
	}
}

func TestLineSpecTopologyDefaultsAndRejectsUnknownValues(t *testing.T) {
	spec := LineSpec{}
	if err := spec.NormalizeTopology(); err != nil {
		t.Fatal(err)
	}
	if spec.TopologyMode != "trihop" || spec.ServiceProfile != "general" {
		t.Fatalf("legacy defaults=%q/%q", spec.TopologyMode, spec.ServiceProfile)
	}
	for _, invalid := range []LineSpec{
		{TopologyMode: "mesh", ServiceProfile: "general"},
		{TopologyMode: "single_hk", ServiceProfile: "unknown"},
	} {
		if err := invalid.NormalizeTopology(); err == nil {
			t.Fatalf("accepted invalid topology/profile: %+v", invalid)
		}
	}
}

func TestGeneralSingleHKNeedsNoExternalCapacityReservation(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	if _, err = store.UpsertDevice(ctx, Device{ID: "hk-prod", Name: "HK", Status: "ready",
		Environment: "production", Host: "192.0.2.20", SSHPort: 22, SSHUser: "root",
		Labels: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	if _, err = store.UpsertLine(ctx, Line{ID: "hk-general", Name: "HK general", Status: "draft",
		Environment: "production", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	if _, err = store.SaveLineSpec(ctx, LineSpec{LineID: "hk-general", Environment: "production",
		TopologyMode: "single_hk", ServiceProfile: "general", ResourceGroup: "hk",
		InstanceID: "hk-general_1", BandwidthMbps: 5, UpstreamMbps: 5, DownstreamMbps: 5,
		SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071, ExitPort: 4443,
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`),
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "direct",
		Nodes: []LineNode{{DeviceID: "hk-prod", Role: "entry"}, {DeviceID: "hk-prod", Role: "exit"}}}); err != nil {
		t.Fatal(err)
	}
	reservations, err := store.ReserveLineCapacity(ctx, "hk-general", "op-open")
	if err != nil || len(reservations) != 0 {
		t.Fatalf("single-HK reservations=%v error=%v", reservations, err)
	}
	findings, err := store.ProductionLineFindings(ctx, "hk-general")
	if err != nil {
		t.Fatal(err)
	}
	for _, finding := range findings {
		if finding.Code == "capacity_unknown" || finding.Code == "qualification_required" {
			t.Fatalf("general single-HK received strict finding: %+v", finding)
		}
	}
}
