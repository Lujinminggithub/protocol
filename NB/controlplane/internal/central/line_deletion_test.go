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
