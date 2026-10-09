package central

import (
	"database/sql"
	"encoding/json"
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

func TestForceDeleteLineRequiresConfirmationAndPreservesAudit(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	lineID := "line-unreachable"
	if _, err = store.UpsertLine(ctx, Line{ID: lineID, Name: "unreachable", Status: "active",
		EntryRegion: "gz", ExitRegion: "kz", Provider: "mixed", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	if _, err = store.UpsertDevice(ctx, Device{ID: "kz-exit", Name: "KZ Exit", Status: "ready",
		Host: "192.0.2.80", SSHPort: 5222, SSHUser: "root", SecretRef: "device:kz-exit", Labels: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	if err = store.UpdateDeviceHealth(ctx, "kz-exit", "healthy"); err != nil {
		t.Fatal(err)
	}
	if _, err = store.SaveLineSpec(ctx, LineSpec{LineID: lineID, ResourceGroup: "gz-hk", InstanceID: "kz-1",
		BandwidthMbps: 5, SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071,
		RelayPort: 4445, ExitPort: 4443, BuildMode: "auto", SourceRef: "repo://current",
		Nodes: []LineNode{{DeviceID: "kz-exit", Role: "exit", Ordinal: 0}}}); err != nil {
		t.Fatal(err)
	}

	normal := LineDeletionRequest{RequestedBy: "operator", Reason: "normal deletion"}
	if err = store.DeleteLine(ctx, lineID, normal); err == nil || !strings.Contains(err.Error(), "停用") {
		t.Fatalf("normal deletion error=%v", err)
	}
	invalid := LineDeletionRequest{RequestedBy: "operator", Reason: "force deletion", Force: true,
		Confirmation: "wrong-line", AcknowledgeOrphans: true}
	if err = store.DeleteLine(ctx, lineID, invalid); err == nil || !strings.Contains(err.Error(), "线路 ID") {
		t.Fatalf("invalid force confirmation error=%v", err)
	}
	force := LineDeletionRequest{RequestedBy: "operator", Reason: "exit permanently unreachable", Force: true,
		Confirmation: lineID, AcknowledgeOrphans: true}
	if err = store.DeleteLine(ctx, lineID, force); err == nil || !strings.Contains(err.Error(), "异常节点") {
		t.Fatalf("healthy line force deletion error=%v", err)
	}
	if err = store.UpdateDeviceHealth(ctx, "kz-exit", "unreachable"); err != nil {
		t.Fatal(err)
	}

	operation := Operation{ID: "op-active-delete", LineID: lineID, Kind: "line.disable", RequestedBy: "operator",
		IdempotencyKey: "active-delete", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(ctx, operation); err != nil {
		t.Fatal(err)
	}
	if err = store.DeleteLine(ctx, lineID, force); err == nil || !strings.Contains(err.Error(), "执行中") {
		t.Fatalf("force deletion with active operation error=%v", err)
	}
	if err = store.CancelOperation(ctx, operation.ID, "test completed"); err != nil {
		t.Fatal(err)
	}
	if err = store.DeleteLine(ctx, lineID, force); err != nil {
		t.Fatalf("force deletion failed: %v", err)
	}
	if _, err = store.Line(ctx, lineID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("deleted line lookup error=%v", err)
	}
	var snapshot []byte
	if err = store.db.QueryRowContext(ctx, `SELECT snapshot FROM line_deletion_audit WHERE line_id=? ORDER BY id DESC LIMIT 1`, lineID).Scan(&snapshot); err != nil {
		t.Fatal(err)
	}
	var audit struct {
		Force   bool `json:"force"`
		Devices []struct {
			ID     string `json:"id"`
			Health string `json:"health"`
		} `json:"devices"`
	}
	if err = json.Unmarshal(snapshot, &audit); err != nil {
		t.Fatal(err)
	}
	if !audit.Force || len(audit.Devices) != 1 || audit.Devices[0].ID != "kz-exit" || audit.Devices[0].Health != "unreachable" {
		t.Fatalf("force deletion audit=%s", snapshot)
	}
}

func TestDeleteDraftLineWithSeparateTelemetryDatabase(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	lineID := "draft-split-telemetry"
	if _, err = store.UpsertLine(ctx, Line{ID: lineID, Name: "draft", Status: "draft",
		EntryRegion: "gz", ExitRegion: "kz", Provider: "mixed", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	operation := Operation{ID: "op-failed-open", LineID: lineID, Kind: "line.open",
		RequestedBy: "operator", IdempotencyKey: "failed-open", Request: json.RawMessage(`{}`)}
	if _, _, err = store.CreateOperation(ctx, operation); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`UPDATE operations SET status='failed',result='{}' WHERE id=?`, operation.ID); err != nil {
		t.Fatal(err)
	}
	for _, query := range []string{`DROP TABLE latest_snapshots`, `DROP TABLE snapshots`, `DROP TABLE traffic_rollups`} {
		if _, err = store.db.Exec(query); err != nil {
			t.Fatal(err)
		}
	}
	store.dialect = "mysql"
	request := LineDeletionRequest{RequestedBy: "operator", Reason: "remove failed draft"}
	if err = store.DeleteLine(ctx, lineID, request); err != nil {
		t.Fatalf("delete split-database draft: %v", err)
	}
	if _, err = store.Line(ctx, lineID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("deleted line lookup error=%v", err)
	}
	var operations int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM operations WHERE line_id=?`, lineID).Scan(&operations); err != nil {
		t.Fatal(err)
	}
	if operations != 0 {
		t.Fatalf("operations=%d, want 0", operations)
	}
	var audit int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM line_deletion_audit WHERE line_id=?`, lineID).Scan(&audit); err != nil {
		t.Fatal(err)
	}
	if audit != 1 {
		t.Fatalf("audit=%d, want 1", audit)
	}
}

func TestDeleteReleasesReservedDirectionalCapacity(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	for _, id := range []string{"delete-entry", "delete-relay"} {
		if _, err = store.UpsertDevice(ctx, Device{ID: id, Name: id, Status: "ready",
			Environment: "production", Host: "192.0.2.1", SSHPort: 22, SSHUser: "root",
			Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = store.UpsertLine(ctx, Line{ID: "delete-capacity", Name: "delete capacity",
		Status: "draft", Environment: "production", EntryRegion: "gz", ExitRegion: "hk",
		Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	if _, err = store.UpsertNetworkLink(ctx, NetworkLink{ID: "delete-link", FromDeviceID: "delete-entry",
		FromRole: "entry", ToDeviceID: "delete-relay", ToRole: "relay", ForwardCapacityMbps: 10,
		ReverseCapacityMbps: 10, BillingMode: LinkBillingIndependent, Environment: "production",
		Status: "ready"}); err != nil {
		t.Fatal(err)
	}
	stamp := now()
	if _, err = store.db.ExecContext(ctx, `INSERT INTO line_capacity_reservations
 (line_id,link_id,forward_mbps,reverse_mbps,state,operation_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?)`, "delete-capacity", "delete-link", 10, 10, "reserved", "open-delete", stamp, stamp); err != nil {
		t.Fatal(err)
	}
	if err = store.DeleteLine(ctx, "delete-capacity", LineDeletionRequest{RequestedBy: "operator",
		Reason: "discard unqualified draft"}); err != nil {
		t.Fatal(err)
	}
	links, err := store.NetworkLinks(ctx)
	if err != nil || len(links) != 1 || links[0].ForwardAvailableMbps != 10 ||
		links[0].ReverseAvailableMbps != 10 || links[0].ForwardReservedMbps != 0 ||
		links[0].ReverseReservedMbps != 0 {
		t.Fatalf("released links=%+v err=%v", links, err)
	}
}

func TestScheduledDeletionKeepsDraftUntilCleanupSucceeds(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	ctx := t.Context()
	lineID := "draft-cleanup"
	if _, err = store.UpsertLine(ctx, Line{ID: lineID, Name: "draft", Status: "draft", Provider: "test"}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"entry-cleanup", "relay-cleanup", "exit-cleanup"} {
		if _, err = store.UpsertDevice(ctx, Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.1",
			SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id, Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	spec := LineSpec{LineID: lineID, ResourceGroup: "shared", InstanceID: lineID + "_1", BandwidthMbps: 5,
		SocksPort: 1091, RelayPort: 4459, ExitPort: 4459, UDPPortMin: 30240, UDPPortMax: 31263,
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto",
		Nodes: []LineNode{{DeviceID: "entry-cleanup", Role: "entry"}, {DeviceID: "relay-cleanup", Role: "relay"}, {DeviceID: "exit-cleanup", Role: "exit"}}}
	if _, err = store.SaveLineSpec(ctx, spec); err != nil {
		t.Fatal(err)
	}
	if _, err = store.db.Exec(`PRAGMA foreign_keys=OFF`); err != nil {
		t.Fatal(err)
	}
	store.dialect = "mysql"
	request := LineDeletionRequest{RequestedBy: "operator", Reason: "remove failed draft"}
	operation, err := store.ScheduleLineDeletion(ctx, lineID, "op-delete-cleanup", request)
	if err != nil {
		t.Fatal(err)
	}
	if operation.Kind != "line.disable" || operation.Status != "queued" || !strings.Contains(string(operation.Request), `"delete_after_cleanup":true`) {
		t.Fatalf("cleanup operation=%+v", operation)
	}
	if line, lineErr := store.Line(ctx, lineID); lineErr != nil || line.Status != "deleting" {
		t.Fatalf("pending line=%+v err=%v", line, lineErr)
	}
	if _, err = store.db.Exec(`UPDATE operations SET status='running' WHERE id=?`, operation.ID); err != nil {
		t.Fatal(err)
	}
	if err = store.CompleteOperation(ctx, operation.ID, lineID, "succeeded", json.RawMessage(`{}`)); err != nil {
		t.Fatal(err)
	}
	if err = store.CompleteOperation(ctx, operation.ID, lineID, "succeeded", json.RawMessage(`{}`)); err != nil {
		t.Fatalf("idempotent cleanup completion failed: %v", err)
	}
	if _, err = store.Line(ctx, lineID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("line still exists after cleanup: %v", err)
	}
	var audit int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM line_deletion_audit WHERE line_id=?`, lineID).Scan(&audit); err != nil || audit != 1 {
		t.Fatalf("audit=%d err=%v", audit, err)
	}
	var nodes int
	if err = store.db.QueryRow(`SELECT COUNT(*) FROM line_nodes WHERE line_id=?`, lineID).Scan(&nodes); err != nil || nodes != 0 {
		t.Fatalf("line_nodes=%d err=%v", nodes, err)
	}
}
