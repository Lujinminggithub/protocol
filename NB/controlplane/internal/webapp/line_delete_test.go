package webapp

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"nb-controlplane/internal/central"
)

func TestForceDeleteLineAPIRequiresManualConfirmation(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-unreachable", Name: "unreachable", Status: "active",
		EntryRegion: "gz", ExitRegion: "kz", Provider: "mixed", CapacityMbps: 5}); err != nil {
		t.Fatal(err)
	}
	if _, err = database.UpsertDevice(t.Context(), central.Device{ID: "kz-exit", Name: "KZ Exit", Status: "ready",
		Host: "192.0.2.80", SSHPort: 5222, SSHUser: "root", SecretRef: "device:kz-exit", Labels: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	if err = database.UpdateDeviceHealth(t.Context(), "kz-exit", "unreachable"); err != nil {
		t.Fatal(err)
	}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: "line-unreachable", ResourceGroup: "gz-hk",
		InstanceID: "kz-1", BandwidthMbps: 5, SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071,
		RelayPort: 4445, ExitPort: 4443, BuildMode: "auto", SourceRef: "repo://current",
		Nodes: []central.LineNode{{DeviceID: "kz-exit", Role: "exit", Ordinal: 0}}}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()

	response, body := call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/lines/line-unreachable", "admin", "",
		map[string]any{"requested_by": "operator", "reason": "exit unavailable", "force": true,
			"confirmation": "line-unreachable", "acknowledge_orphans": false})
	if response.StatusCode != http.StatusBadRequest {
		t.Fatalf("missing acknowledgement status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/lines/line-unreachable", "admin", "",
		map[string]any{"requested_by": "operator", "reason": "exit unavailable", "force": true,
			"confirmation": "wrong-line", "acknowledge_orphans": true})
	if response.StatusCode != http.StatusBadRequest {
		t.Fatalf("wrong confirmation status=%d body=%s", response.StatusCode, body)
	}
	operation := central.Operation{ID: "op-active-delete", LineID: "line-unreachable", Kind: "line.disable",
		RequestedBy: "operator", IdempotencyKey: "active-delete", Request: json.RawMessage(`{}`)}
	if _, _, err = database.CreateOperation(t.Context(), operation); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/lines/line-unreachable", "admin", "",
		map[string]any{"requested_by": "operator", "reason": "exit unavailable", "force": true,
			"confirmation": "line-unreachable", "acknowledge_orphans": true})
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("active operation status=%d body=%s", response.StatusCode, body)
	}
	if err = database.CancelOperation(t.Context(), operation.ID, "test completed"); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/lines/line-unreachable", "admin", "",
		map[string]any{"requested_by": "operator", "reason": "exit unavailable", "force": true,
			"confirmation": "line-unreachable", "acknowledge_orphans": true})
	if response.StatusCode != http.StatusNoContent {
		t.Fatalf("force deletion status=%d body=%s", response.StatusCode, body)
	}
}

func TestDeleteDraftWithSpecQueuesCleanup(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	ctx := t.Context()
	lineID := "draft-cleanup"
	if _, err = database.UpsertLine(ctx, central.Line{ID: lineID, Name: "draft", Status: "draft", Provider: "test"}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"entry-cleanup", "relay-cleanup", "exit-cleanup"} {
		if _, err = database.UpsertDevice(ctx, central.Device{ID: id, Name: id, Status: "ready", Host: "192.0.2.1",
			SSHPort: 22, SSHUser: "root", SecretRef: "device:" + id, Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = database.SaveLineSpec(ctx, central.LineSpec{LineID: lineID, ResourceGroup: "shared", InstanceID: lineID + "_1",
		BandwidthMbps: 5, UpstreamMbps: 5, DownstreamMbps: 5, SocksPort: 1091, RelayPort: 4459, ExitPort: 4459,
		UDPPortMin: 30240, UDPPortMax: 31263, Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`),
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Nodes: []central.LineNode{
			{DeviceID: "entry-cleanup", Role: "entry"}, {DeviceID: "relay-cleanup", Role: "relay"}, {DeviceID: "exit-cleanup", Role: "exit"}}}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/lines/"+lineID, "admin", "",
		map[string]any{"requested_by": "operator", "reason": "remove failed draft"})
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"kind":"line.disable"`)) {
		t.Fatalf("delete status=%d body=%s", response.StatusCode, body)
	}
	line, err := database.Line(ctx, lineID)
	if err != nil || line.Status != "deleting" {
		t.Fatalf("line=%+v err=%v", line, err)
	}
	var cleanup central.Operation
	if err = json.Unmarshal(body, &cleanup); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/operations?line_id="+lineID+"&limit=1", "agent", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(cleanup.ID)) {
		t.Fatalf("claim status=%d body=%s", response.StatusCode, body)
	}
	completion := map[string]any{"line_id": lineID, "status": "succeeded", "result": map[string]any{}}
	for attempt := 0; attempt < 2; attempt++ {
		response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/operations/"+cleanup.ID+"/result", "agent", "", completion)
		if response.StatusCode != http.StatusOK {
			t.Fatalf("completion attempt=%d status=%d body=%s", attempt+1, response.StatusCode, body)
		}
	}
}
