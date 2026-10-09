package worker

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestDedicatedClientAdvertisesAndPollsOnlyPlatformCapability(t *testing.T) {
	var heartbeat map[string]any
	claimed := []string{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/agent/v1/executors/heartbeat":
			_ = json.NewDecoder(r.Body).Decode(&heartbeat)
			w.WriteHeader(http.StatusAccepted)
		case "/agent/v1/operations":
			claimed = append(claimed, r.URL.Query().Get("line_id"))
			_, _ = w.Write([]byte(`{"operations":[]}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	client, err := NewClient(Registry{WorkerID: "platform-upgrader", StateDir: t.TempDir()}, &fakeRunner{}, ClientConfig{
		BaseURL: server.URL, Token: "agent", Version: "test", PollEvery: time.Millisecond,
		HeartbeatEvery: time.Hour, MaintenanceEvery: time.Hour, SnapshotEvery: time.Hour,
		Capabilities: []ExecutorCapability{{LineID: "__platform__", Operations: []string{"platform.upgrade", "platform.rollback"}}},
		PollLineIDs:  []string{"__platform__"}, DisableBackground: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err = client.heartbeat(t.Context()); err != nil {
		t.Fatal(err)
	}
	if err = client.poll(t.Context()); err != nil {
		t.Fatal(err)
	}
	lines := heartbeat["lines"].([]any)
	if len(lines) != 1 || lines[0].(map[string]any)["line_id"] != "__platform__" {
		t.Fatalf("heartbeat=%v", heartbeat)
	}
	if len(claimed) != 1 || claimed[0] != "__platform__" {
		t.Fatalf("claimed=%v", claimed)
	}
}
