package worker

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"
)

func testRegistry(t *testing.T, operations []string) Registry {
	t.Helper()
	directory := t.TempDir()
	for _, name := range []string{"hosts.json", "profile.json", "known_hosts", "machines.json"} {
		data := []byte(`{}`)
		if name == "profile.json" {
			data = []byte(`{"schema_version":3}`)
		}
		if err := os.WriteFile(filepath.Join(directory, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	registry := Registry{SchemaVersion: 1, WorkerID: "worker-1", Root: directory, Python: "python",
		StateDir: filepath.Join(directory, "state"), Lines: []LineSpec{{LineID: "line-1", ResourceGroup: "shared-1",
			HostsFile: filepath.Join(directory, "hosts.json"), SourceMachinesFile: filepath.Join(directory, "machines.json"),
			LineProfileFile: filepath.Join(directory, "profile.json"), KnownHostsFile: filepath.Join(directory, "known_hosts"),
			SecurityDir: directory, PackageMbps: 10, SocksPort: 1080, EnabledOperations: operations}}}
	if err := registry.Validate(); err != nil {
		t.Fatal(err)
	}
	return registry
}

func TestRegistryRejectsNonAllowlistedOperation(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines[0].EnabledOperations = []string{"shell.run"}
	if err := registry.Validate(); err == nil {
		t.Fatal("registry accepted arbitrary operation")
	}
}

func TestRunnerRefusesDisabledOperation(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines[0].DisabledReason = "shared resource is active"
	_, err := NewRunner(registry).Run(context.Background(), Operation{ID: "op-1", LineID: "line-1", Kind: "line.open"})
	if err == nil || err.Error() != "shared resource is active" {
		t.Fatalf("unexpected error: %v", err)
	}
}

type fakeRunner struct{ calls atomic.Int64 }

func (runner *fakeRunner) Run(_ context.Context, operation Operation) (Result, error) {
	runner.calls.Add(1)
	return Result{Deployment: "dep-2", Profile: operation.LineID + ":1", LogFile: "worker.log", Message: "ok"}, nil
}

func TestClientHeartbeatsClaimsAndPersistsResult(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate"})
	var heartbeat, completed atomic.Int64
	var claimed atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.Header.Get("Authorization") != "Bearer agent-secret" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/agent/v1/executors/heartbeat":
			heartbeat.Add(1)
			_, _ = w.Write([]byte(`{"status":"ready"}`))
		case "/agent/v1/operations":
			if claimed.CompareAndSwap(false, true) {
				_, _ = w.Write([]byte(`{"operations":[{"id":"op-1","line_id":"line-1","kind":"line.validate","request":{}}]}`))
			} else {
				_, _ = w.Write([]byte(`{"operations":[]}`))
			}
		case "/agent/v1/operations/op-1/result":
			var payload map[string]any
			_ = json.NewDecoder(request.Body).Decode(&payload)
			if payload["status"] == "succeeded" {
				completed.Add(1)
			}
			_, _ = w.Write([]byte(`{"status":"accepted"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	runner := &fakeRunner{}
	client, err := NewClient(registry, runner, ClientConfig{BaseURL: server.URL, Token: "agent-secret",
		Version: "test", PollEvery: 10 * time.Millisecond, HeartbeatEvery: 10 * time.Millisecond, OperationTimeout: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
	defer cancel()
	if err = client.Run(ctx); err != nil {
		t.Fatal(err)
	}
	if heartbeat.Load() < 1 || completed.Load() != 1 || runner.calls.Load() != 1 {
		t.Fatalf("heartbeat=%d completed=%d calls=%d", heartbeat.Load(), completed.Load(), runner.calls.Load())
	}
	if _, err = os.Stat(filepath.Join(registry.StateDir, "op-1", "result.json")); err != nil {
		t.Fatal("operation result was not persisted")
	}
}
