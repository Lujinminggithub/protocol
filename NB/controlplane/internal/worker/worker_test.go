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

func TestRegistryRejectsSharedPortCollision(t *testing.T) {
	registry := testRegistry(t, nil)
	second := registry.Lines[0]
	second.LineID = "line-2"
	second.InstanceID = "line-2"
	registry.Lines = append(registry.Lines, second)
	if err := registry.Validate(); err == nil {
		t.Fatal("registry accepted colliding shared entry and middle ports")
	}
	registry.Lines[1].SocksPort = 1081
	registry.Lines[1].MiddlePort = 4444
	registry.Lines[1].UDPPortMin, registry.Lines[1].UDPPortMax = 21024, 22047
	if err := registry.Validate(); err != nil {
		t.Fatalf("registry rejected isolated ports: %v", err)
	}
}

func TestRunnerInjectsInstanceRouting(t *testing.T) {
	registry := testRegistry(t, nil)
	line := registry.Lines[0]
	line.InstanceID, line.SocksPort, line.MiddlePort, line.ExitPort = "kz", 1081, 4444, 4443
	line.UDPPortMin, line.UDPPortMax = 21024, 22047
	values, err := NewRunner(registry).environment(line)
	if err != nil {
		t.Fatal(err)
	}
	for key, expected := range map[string]string{"NB_DEPLOY_INSTANCE": "kz", "NB_SOCKS_PORT": "1081", "NB_SOCKS_UDP_PORT_MIN": "21024", "NB_SOCKS_UDP_PORT_MAX": "22047", "NB_MIDDLE_PORT": "4444", "NB_EXIT_PORT": "4443"} {
		if values[key] != expected {
			t.Fatalf("%s=%q, want %q", key, values[key], expected)
		}
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
