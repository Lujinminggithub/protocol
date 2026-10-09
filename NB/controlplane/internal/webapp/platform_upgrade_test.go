package webapp

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"nb-controlplane/internal/central"
)

func TestPlatformUpgradeAPIUsesPreviewAndRejectsLineUpgrade(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	for _, deviceID := range []string{"entry-line-a", "entry-line-b", "middle-shared", "exit-line-a", "exit-line-b"} {
		if _, err = database.UpsertDevice(t.Context(), central.Device{ID: deviceID, Name: deviceID, Status: "ready",
			Host: "127.0.0.1", SSHPort: 22, SSHUser: "root", Environment: "production"}); err != nil {
			t.Fatal(err)
		}
	}
	for index, lineID := range []string{"line-a", "line-b"} {
		if _, err = database.UpsertLine(t.Context(), central.Line{ID: lineID, Name: lineID, Status: "active", Environment: "production", Provider: "test"}); err != nil {
			t.Fatal(err)
		}
		nodes := []central.LineNode{{DeviceID: "entry-" + lineID, Role: "entry"},
			{DeviceID: "middle-shared", Role: "relay"}, {DeviceID: "exit-" + lineID, Role: "exit"}}
		if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: lineID, Environment: "production",
			ResourceGroup: "renamable", InstanceID: lineID + "_1", BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10,
			SocksPort: 1082 + index, UDPPortMin: 22048 + index*1024, UDPPortMax: 23071 + index*1024,
			RelayPort: 4445 + index*2, ExitPort: 4443 + index*2,
			BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Whitelist: json.RawMessage(`[]`),
			DNSServers: json.RawMessage(`[]`), Nodes: nodes}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "__node_release__", Name: "Node Release", Status: "archived", Environment: "production", Provider: "controlplane"}); err != nil {
		t.Fatal(err)
	}
	releaseOperation := central.Operation{ID: "op-release", LineID: "__node_release__", Kind: "node.release.build",
		RequestedBy: "test", IdempotencyKey: "release-test", Request: json.RawMessage(`{"upload_id":"source-test"}`)}
	if _, _, err = database.CreateOperation(t.Context(), releaseOperation); err != nil {
		t.Fatal(err)
	}
	if _, err = database.ClaimOperations(t.Context(), "__node_release__", 1); err != nil {
		t.Fatal(err)
	}
	result := json.RawMessage(`{"node_release":{"release_id":"node-release","platform_release":{"release_id":"platform-release","candidate_root":"/opt/nb/source-candidates/ready-test","nb_node":{"sha256":"abc"}}}}`)
	if err = database.CompleteOperation(t.Context(), "op-release", "__node_release__", "succeeded", result); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()

	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/platform-releases/status", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"release_id":"platform-release"`)) {
		t.Fatalf("status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet,
		server.URL+"/api/v1/platform-upgrades/preview?release_id=platform-release&line_id=line-a", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"line-b"`)) {
		t.Fatalf("preview status=%d body=%s", response.StatusCode, body)
	}
	var preview struct {
		Digest string `json:"preview_digest"`
	}
	if json.Unmarshal(body, &preview) != nil || preview.Digest == "" {
		t.Fatalf("preview digest body=%s", body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/platform-upgrades", "admin", "bad-key", map[string]any{
		"release_id": "platform-release", "line_id": "line-a", "preview_digest": preview.Digest,
		"requested_by": "operator", "confirm_interrupt": true, "unknown": true,
	})
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("unknown field")) {
		t.Fatalf("strict JSON status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/platform-upgrades", "admin", "upgrade-key", map[string]any{
		"release_id": "platform-release", "line_id": "line-a", "preview_digest": preview.Digest,
		"requested_by": "operator", "confirm_interrupt": true,
	})
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"kind":"platform.upgrade"`)) ||
		!bytes.Contains(body, []byte(`"line_id":"__platform__"`)) {
		t.Fatalf("create status=%d body=%s", response.StatusCode, body)
	}
	var upgrade central.Operation
	if json.Unmarshal(body, &upgrade) != nil {
		t.Fatalf("upgrade body=%s", body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin", "blocked-during-upgrade", map[string]any{
		"line_id": "line-a", "kind": "line.validate", "requested_by": "operator", "request": map[string]any{},
	})
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("平台升级")) {
		t.Fatalf("upgrade write lock status=%d body=%s", response.StatusCode, body)
	}
	if _, err = database.ClaimOperations(t.Context(), "__platform__", 1); err != nil {
		t.Fatal(err)
	}
	if err = database.CompleteOperation(t.Context(), upgrade.ID, "__platform__", "succeeded", json.RawMessage(`{"status":"succeeded"}`)); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPost,
		server.URL+"/api/v1/platform-upgrades/"+upgrade.ID+"/rollback", "admin", "rollback-key", map[string]any{
			"requested_by": "operator", "confirmation": upgrade.ID,
		})
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"kind":"platform.rollback"`)) {
		t.Fatalf("rollback status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin", "line-upgrade-key", map[string]any{
		"line_id": "line-a", "kind": "line.upgrade", "requested_by": "operator", "request": map[string]any{},
	})
	if response.StatusCode != http.StatusConflict ||
		(!bytes.Contains(body, []byte("版本升级")) && !bytes.Contains(body, []byte("平台升级"))) {
		t.Fatalf("line upgrade status=%d body=%s", response.StatusCode, body)
	}
}

func TestPlatformUpgradeAssetsExposeSingleTopLevelEntry(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, err := server.Client().Get(server.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(response.Body)
	_ = response.Body.Close()
	for _, marker := range []string{`data-view="upgrades"`, `id="upgradesView"`, `id="platformUpgradePreview"`,
		`id="confirmPlatformInterrupt"`, `id="startPlatformUpgrade"`} {
		if !bytes.Contains(body, []byte(marker)) {
			t.Fatalf("missing upgrade asset %s", marker)
		}
	}
	response, err = server.Client().Get(server.URL + "/app.js")
	if err != nil {
		t.Fatal(err)
	}
	script, _ := io.ReadAll(response.Body)
	_ = response.Body.Close()
	if bytes.Contains(script, []byte(`["line.upgrade","升级"`)) ||
		!bytes.Contains(script, []byte(`/api/v1/platform-upgrades/preview`)) ||
		!bytes.Contains(script, []byte(`/api/v1/platform-upgrades`)) {
		t.Fatalf("upgrade JavaScript contract missing or line upgrade remains")
	}
}
