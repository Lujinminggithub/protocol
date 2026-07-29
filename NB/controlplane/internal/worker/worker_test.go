package worker

import (
	"bytes"
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

func TestDynamicLineBuildsTopologyWithoutPersistingSecretValues(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines = nil
	registry.Dynamic = DynamicConfig{Enabled: true, ResourceGroups: []string{"shared-1"},
		Operations: []string{"line.open"}, SingBox: "sing-box", SocksPortMin: 1082, SocksPortMax: 1199,
		RelayPortMin: 4445, RelayPortMax: 4599, UDPPortMin: 22048, UDPPortMax: 65535}
	for _, name := range []string{"ENTRY", "RELAY", "EXIT"} {
		t.Setenv("NB_TEST_"+name, "private-password-"+name)
	}
	t.Setenv("NB_TEST_SRS_URL", "https://example.invalid/whitelist.srs")
	device := func(id, role, host, secret string) dynamicNode {
		return dynamicNode{DeviceID: id, Role: role, Device: dynamicDevice{ID: id, Name: id, Host: host,
			SSHPort: 22, SSHUser: "root", SecretRef: "env:" + secret}}
	}
	plan := dynamicPlan{LineID: "line-new", ResourceGroup: "shared-1", InstanceID: "new", BandwidthMbps: 20,
		SocksPort: 1082, RelayPort: 4445, ExitPort: 4443, UDPPortMin: 22048, UDPPortMax: 23071,
		Whitelist: []string{"domain example.com"}, BuildMode: "auto", JumpPolicy: "auto", SRSRef: "env:NB_TEST_SRS_URL",
		Nodes: []dynamicNode{device("entry-1", "entry", "192.0.2.1", "NB_TEST_ENTRY"),
			device("relay-1", "relay", "192.0.2.2", "NB_TEST_RELAY"), device("exit-1", "exit", "192.0.2.3", "NB_TEST_EXIT")}}
	runner := NewRunner(registry)
	line, err := runner.dynamicLine(Operation{ID: "op-new", LineID: plan.LineID, Kind: "line.open"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(line.StateDir, "runtime-plan.json"))
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(data, []byte("private-password")) || line.WhitelistSourceEnv != "NB_TEST_SRS_URL" {
		t.Fatal("runtime plan persisted a secret value or lost the SRS source reference")
	}
	steps, err := runner.steps(line, Operation{ID: "op-new", LineID: plan.LineID, Kind: "line.open"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil || len(steps) < 3 || steps[0].Stage != "whitelist-fetch" || steps[len(steps)-1].Stage != "whitelist" {
		t.Fatalf("unexpected dynamic open steps: %#v err=%v", steps, err)
	}
}

type fakeRunner struct{ calls atomic.Int64 }

func (runner *fakeRunner) Run(_ context.Context, operation Operation) (Result, error) {
	runner.calls.Add(1)
	return Result{Deployment: "dep-2", Profile: operation.LineID + ":1", LogFile: "worker.log", Message: "ok"}, nil
}

type fakeSnapshotRunner struct{ fakeRunner }

func (runner *fakeSnapshotRunner) CollectSnapshots(_ context.Context, line LineSpec) ([]Snapshot, error) {
	return []Snapshot{{LineID: line.LineID, NodeID: "entry-1", Role: "entry", WorkerID: "0",
		ObservedAt: time.Now().UTC().Format(time.RFC3339Nano), Health: "ok",
		Deployment: "dep-1", Profile: line.LineID + ":1"}}, nil
}

func TestClientCollectsAndDeliversSnapshots(t *testing.T) {
	registry := testRegistry(t, nil)
	var delivered atomic.Int64
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.Header.Get("Authorization") != "Bearer agent-secret" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		switch request.URL.Path {
		case "/agent/v1/snapshots":
			var snapshot Snapshot
			if json.NewDecoder(request.Body).Decode(&snapshot) != nil || snapshot.LineID != "line-1" || snapshot.Health != "ok" {
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			delivered.Add(1)
		case "/agent/v1/executors/heartbeat":
		case "/agent/v1/operations":
			_, _ = w.Write([]byte(`{"operations":[]}`))
		default:
			w.WriteHeader(http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"accepted"}`))
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeSnapshotRunner{}, ClientConfig{BaseURL: server.URL,
		Token: "agent-secret", SnapshotEvery: 10 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	client.collectSnapshots(context.Background())
	if delivered.Load() != 1 {
		t.Fatalf("delivered snapshots=%d", delivered.Load())
	}
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

func TestInventoryDiscoveryOmitsCredentials(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines[0].InstanceID = "test"
	registry.Lines[0].MiddlePort, registry.Lines[0].ExitPort = 4444, 4443
	registry.Lines[0].UDPPortMin, registry.Lines[0].UDPPortMax = 21024, 22047
	machines := `{"edges":[{"name":"entry-1","host":"192.0.2.1","port":22,"user":"root","password":"entry-secret"}],` +
		`"relays":[{"name":"relay-1","host":"192.0.2.2","port":22,"user":"root","password":"relay-secret"}],` +
		`"terminals":[{"name":"exit-1","host":"192.0.2.3","port":2273,"user":"root","password":"exit-secret"}]}`
	if err := os.WriteFile(registry.Lines[0].SourceMachinesFile, []byte(machines), 0600); err != nil {
		t.Fatal(err)
	}
	client := &Client{registry: registry}
	lines := client.inventoryLines()
	encoded, err := json.Marshal(lines)
	if err != nil || len(lines) != 1 {
		t.Fatalf("inventory lines=%d err=%v", len(lines), err)
	}
	if bytes.Contains(encoded, []byte("entry-secret")) || bytes.Contains(encoded, []byte("relay-secret")) || bytes.Contains(encoded, []byte("exit-secret")) {
		t.Fatal("inventory payload contains a password")
	}
	if !bytes.Contains(encoded, []byte(`"worker-local:line-1:entry"`)) || !bytes.Contains(encoded, []byte(`"ssh_port":2273`)) {
		t.Fatalf("inventory omitted secret reference or SSH port: %s", encoded)
	}
}
