package webapp

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"nb-controlplane/internal/central"
)

func call(t *testing.T, client *http.Client, method, url, token, key string, body any) (*http.Response, []byte) {
	t.Helper()
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			t.Fatal(err)
		}
		reader = bytes.NewReader(data)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatal(err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if key != "" {
		req.Header.Set("Idempotency-Key", key)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	response, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(response.Body)
	response.Body.Close()
	if err != nil {
		t.Fatal(err)
	}
	return response, data
}

func TestCentralWebWorkflow(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin-secret", AgentToken: "agent-secret"}).Handler())
	defer server.Close()

	line := map[string]any{"id": "gz-hk-us", "name": "GZ / HK / US", "status": "active", "entry_region": "Guangzhou",
		"exit_region": "United States", "provider": "multi", "capacity_mbps": 10, "active_deployment": "dep-1", "profile": "gz-hk-us:1", "secret_ref": "vault/nb/lines/gz-hk-us"}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin-secret", "", line)
	if response.StatusCode != 201 {
		t.Fatalf("line status=%d body=%s", response.StatusCode, body)
	}

	snapshot := map[string]any{"line_id": "gz-hk-us", "node_id": "entry-1", "role": "entry", "worker_id": "0",
		"observed_at": "2026-07-28T02:00:00Z", "health": "ok", "deployment": "dep-1", "profile": "gz-hk-us:1",
		"sessions": 4, "throughput_mbps": 3.8, "queue_age_p95_us": 120, "effective_loss_pct": 0, "fec_observe": true, "fec_active": false}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/snapshots", "agent-secret", "", snapshot)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("snapshot status=%d body=%s", response.StatusCode, body)
	}
	legacy := map[string]any{"node_id": "nb-exit-1", "line_id": "gz-hk-us", "observed_at": "2026-07-28T02:00:01Z",
		"health": map[string]any{"status": "ok", "worker": 1, "release_id": "dep-1", "line_profile": "gz-hk-us:1"},
		"metrics": map[string]any{"sessions_inuse": 2, "throughput_mbps": 1.2,
			"queue_age_max_us": map[string]any{"down": 40, "up": 20, "q2t": 10},
			"link":             map[string]any{"effective_loss_max_pct": 0}, "fec": map[string]any{"observe": 1, "active": 0}}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/nb/v1/node-snapshots", "agent-secret", "", legacy)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("legacy snapshot status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/dashboard", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"lines_healthy":1`)) ||
		!bytes.Contains(body, []byte(`"throughput_mbps":3.8`)) || !bytes.Contains(body, []byte(`"sessions":4`)) {
		t.Fatalf("dashboard status=%d body=%s", response.StatusCode, body)
	}
	heartbeat := map[string]any{"worker_id": "windows-1", "status": "ready", "version": "test",
		"observed_at": time.Now().UTC().Format(time.RFC3339Nano),
		"lines":       []map[string]any{{"line_id": "gz-hk-us", "operations": []string{"line.validate", "line.upgrade"}}}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/executors/heartbeat", "agent-secret", "", heartbeat)
	if response.StatusCode != 200 {
		t.Fatalf("heartbeat status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/executors", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"online":true`)) {
		t.Fatalf("executors status=%d body=%s", response.StatusCode, body)
	}
	usage := map[string]any{"line_id": "gz-hk-us", "bytes": 1234}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/nb/v1/usage-events", "agent-secret", "usage-1", usage)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("raw event status=%d body=%s", response.StatusCode, body)
	}

	operation := map[string]any{"line_id": "gz-hk-us", "kind": "line.upgrade", "requested_by": "operator", "request": map[string]any{"deployment": "dep-2"}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-1", operation)
	if response.StatusCode != 201 || !bytes.Contains(body, []byte(`"status":"queued"`)) {
		t.Fatalf("operation status=%d body=%s", response.StatusCode, body)
	}
	var created central.Operation
	if err = json.Unmarshal(body, &created); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-1", operation)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(created.ID)) {
		t.Fatalf("operation replay status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/operations?line_id=gz-hk-us&limit=5", "agent-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"dispatched"`)) {
		t.Fatalf("claim status=%d body=%s", response.StatusCode, body)
	}
	result := map[string]any{"line_id": "gz-hk-us", "status": "succeeded", "result": map[string]any{"deployment": "dep-2"}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/operations/"+created.ID+"/result", "agent-secret", "", result)
	if response.StatusCode != 200 {
		t.Fatalf("complete status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"succeeded"`)) {
		t.Fatalf("operations status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"active_deployment":"dep-2"`)) {
		t.Fatalf("operation did not update line status=%d body=%s", response.StatusCode, body)
	}
	cancellable := map[string]any{"line_id": "gz-hk-us", "kind": "line.validate", "requested_by": "operator", "request": map[string]any{}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-cancel", cancellable)
	var queued central.Operation
	if response.StatusCode != 201 || json.Unmarshal(body, &queued) != nil {
		t.Fatalf("cancellable operation status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations/"+queued.ID+"/cancel", "admin-secret", "", map[string]string{"reason": "duplicate"})
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"cancelled"`)) {
		t.Fatalf("cancel status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPatch, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", map[string]string{"status": "archived"})
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"archived"`)) {
		t.Fatalf("archive status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines", "admin-secret", "", nil)
	if response.StatusCode != 200 || bytes.Contains(body, []byte(`"id":"gz-hk-us"`)) {
		t.Fatalf("archived line remained visible status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", nil)
	if response.StatusCode != 200 {
		t.Fatalf("archived line history is unavailable status=%d body=%s", response.StatusCode, body)
	}
}

func TestCentralWebSeparatesAdminAndAgentTokens(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, _ := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines", "agent", "", nil)
	if response.StatusCode != 401 {
		t.Fatalf("agent accessed admin API: %d", response.StatusCode)
	}
	response, _ = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/operations?line_id=test", "admin", "", nil)
	if response.StatusCode != 401 {
		t.Fatalf("admin accessed agent API: %d", response.StatusCode)
	}
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/", "", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte("NB 运营控制台")) {
		t.Fatalf("static UI status=%d", response.StatusCode)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/app.js", "", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`operationAvailability`)) ||
		!bytes.Contains(body, []byte(`disabled title=`)) ||
		bytes.Contains(body, []byte(`event.currentTarget.reset()`)) {
		t.Fatalf("line lifecycle actions missing from UI status=%d", response.StatusCode)
	}
}
