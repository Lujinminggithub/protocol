package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestWritePrivateJSONSupportsConcurrentWriters(t *testing.T) {
	path := filepath.Join(t.TempDir(), "line", "source-machines.json")
	var wait sync.WaitGroup
	errors := make(chan error, 32)
	for index := 0; index < 32; index++ {
		wait.Add(1)
		go func(value int) {
			defer wait.Done()
			errors <- writePrivateJSON(path, map[string]int{"value": value})
		}(index)
	}
	wait.Wait()
	close(errors)
	for err := range errors {
		if err != nil {
			t.Fatal(err)
		}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var value map[string]int
	if err = json.Unmarshal(data, &value); err != nil || value["value"] < 0 || value["value"] >= 32 {
		t.Fatalf("invalid final JSON: %s err=%v", data, err)
	}
	if matches, _ := filepath.Glob(filepath.Join(filepath.Dir(path), ".source-machines.json.new-*")); len(matches) != 0 {
		t.Fatalf("temporary files leaked: %v", matches)
	}
}

func TestSummarizeTransportRolloutOmitsPrivatePaths(t *testing.T) {
	line := LineSpec{LineID: "line-1", StateDir: t.TempDir()}
	state := profileRolloutState{LineID: line.LineID, Generation: 7, Status: "committed",
		Prepared:     map[string]bool{"entry": true, "middle": true, "exit": true},
		Committed:    map[string]bool{"entry": true, "middle": true, "exit": true},
		Fingerprints: map[string]string{"entry": "entry-fp", "middle": "middle-fp", "exit": "exit-fp"},
		Profiles:     map[string]string{"entry": "/private/entry-7.conf"}}
	if err := atomicJSON(filepath.Join(line.StateDir, "transport", "rollout.json"), state); err != nil {
		t.Fatal(err)
	}
	summary := summarizeTransportRollout(line, "")
	if summary == nil || summary.Generation != 7 || summary.Status != "committed" ||
		strings.Join(summary.CommitOrder, ",") != "exit,middle,entry" || !summary.Roles["middle"].Readback {
		t.Fatalf("unexpected rollout summary: %+v", summary)
	}
	encoded, err := json.Marshal(summary)
	if err != nil || bytes.Contains(encoded, []byte("/private/")) || !bytes.Contains(encoded, []byte("entry-fp")) {
		t.Fatalf("rollout JSON leaks paths or omits fingerprint: %s err=%v", encoded, err)
	}
}

func testRegistry(t *testing.T, operations []string) Registry {
	t.Helper()
	directory := t.TempDir()
	if err := os.MkdirAll(filepath.Join(directory, "tools"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, "tools", "whitelist.local.conf"), []byte("domain default.example\ndomain_exact odr.itunes.apple.com\nport 443\n"), 0600); err != nil {
		t.Fatal(err)
	}
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
	if err := registry.Validate(); err == nil {
		t.Fatal("registry accepted overlapping adjacent middle worker lanes")
	}
	registry.Lines[1].MiddlePort = 4445
	if err := registry.Validate(); err != nil {
		t.Fatalf("registry rejected isolated ports: %v", err)
	}
}

func TestRegistryQuarantinesHistoricalAdjacentWorkerLanes(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate", "line.upgrade"})
	registry.Lines[0].MiddlePort = 4443
	second := registry.Lines[0]
	second.LineID, second.InstanceID = "line-2", "line-2"
	second.SocksPort, second.MiddlePort = 1081, 4444
	second.UDPPortMin, second.UDPPortMax = 21024, 22047
	registry.Lines = append(registry.Lines, second)
	registry.quarantineLegacyTransportCollisions()
	if err := registry.Validate(); err != nil {
		t.Fatalf("quarantined legacy registry did not start: %v", err)
	}
	for _, line := range registry.Lines {
		if line.DisabledReason == "" || len(line.EnabledOperations) != 0 {
			t.Fatalf("legacy collision was not quarantined: %+v", line)
		}
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
	var events []OperationEvent
	ctx := context.WithValue(context.Background(), eventContextKey{}, eventEmitter(func(_ context.Context, event OperationEvent) error {
		events = append(events, event)
		return nil
	}))
	_, err := NewRunner(registry).Run(ctx, Operation{ID: "op-1", LineID: "line-1", Kind: "line.open"})
	if err == nil || err.Error() != "shared resource is active" {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(events) != 2 || events[0].Stage != "prepare" || events[0].Status != "running" || events[1].Status != "failed" {
		t.Fatalf("preparation failure events=%+v", events)
	}
}

func TestCommandFailureSummaryReportsCauseAndRedactsSecrets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "worker.log")
	content := "Traceback\nRuntimeError: connect entry failed: token=do-not-return password=hidden\n"
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	summary := commandFailureSummary(os.ErrInvalid, path)
	if !strings.Contains(summary, "connect entry failed") || strings.Contains(summary, "do-not-return") || strings.Contains(summary, "hidden") {
		t.Fatalf("unsafe or incomplete failure summary: %s", summary)
	}
}

func TestCommandFailureSummaryPrefersProvisionRejection(t *testing.T) {
	path := filepath.Join(t.TempDir(), "worker.log")
	content := "RuntimeError: local source differs from the binary build inputs\n" +
		"[line-1] 拒绝开通: 主动探针失败 rc=1: connection aborted\n" +
		"subprocess.CalledProcessError: command returned non-zero exit status 2\n"
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	summary := commandFailureSummary(os.ErrInvalid, path)
	if summary != "主动探针失败 rc=1: connection aborted" {
		t.Fatalf("wrong root cause selected: %s", summary)
	}
}

func TestCommandFailureSummaryReportsReleaseValidationCause(t *testing.T) {
	path := filepath.Join(t.TempDir(), "worker.log")
	content := "发布清单校验失败: topology 已变化，请重新生成发布清单\n" +
		"subprocess.CalledProcessError: command returned non-zero exit status 1\n"
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	summary := commandFailureSummary(os.ErrInvalid, path)
	if summary != "发布清单校验失败: topology 已变化，请重新生成发布清单" {
		t.Fatalf("wrong release validation cause: %s", summary)
	}
}

func TestClientURLReadsQualifiedPrivateArtifact(t *testing.T) {
	registry := testRegistry(t, nil)
	line := registry.Lines[0]
	line.StateDir = t.TempDir()
	directory := filepath.Join(line.StateDir, "provision", line.LineID)
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	expected := "socks5://user:password@192.0.2.10:1080#line-1"
	if err := os.WriteFile(filepath.Join(directory, "client.json"), []byte(`{"shadowrocket_url":"`+expected+`"}`), 0600); err != nil {
		t.Fatal(err)
	}
	actual, err := clientURL(line, filepath.Join(t.TempDir(), "operation"))
	if err != nil || actual != expected {
		t.Fatalf("client URL=%q err=%v", actual, err)
	}
	if err = os.WriteFile(filepath.Join(directory, "client.json"), []byte(`{"shadowrocket_url":"https://example.invalid"}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err = clientURL(line, filepath.Join(t.TempDir(), "operation")); err == nil {
		t.Fatal("accepted a non-SOCKS client URL")
	}
}

func TestProgressWriterStreamsBoundedRedactedLines(t *testing.T) {
	var destination bytes.Buffer
	var messages []string
	writer := newProgressWriter(&destination, 0, 2, func(message string) { messages = append(messages, message) })
	_, err := writer.Write([]byte("building\npassword=hidden token=private\nthird line\n"))
	if err != nil {
		t.Fatal(err)
	}
	writer.Flush()
	if destination.String() != "building\npassword=hidden token=private\nthird line\n" {
		t.Fatalf("full local log was changed: %q", destination.String())
	}
	if len(messages) != 2 || messages[0] != "building" || strings.Contains(messages[1], "hidden") || strings.Contains(messages[1], "private") {
		t.Fatalf("unexpected streamed messages: %#v", messages)
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
	device := func(id, role, host, secret string) dynamicNode {
		return dynamicNode{DeviceID: id, Role: role, Device: dynamicDevice{ID: id, Name: id, Host: host,
			SSHPort: 22, SSHUser: "root", SecretRef: "env:" + secret}}
	}
	plan := dynamicPlan{LineID: "line-new", ResourceGroup: "shared-1", InstanceID: "new", BandwidthMbps: 20,
		UpstreamMbps: 6, DownstreamMbps: 14,
		SocksPort: 1082, RelayPort: 4445, ExitPort: 4443, ExitBindIP: "192.0.2.30", UDPPortMin: 22048, UDPPortMax: 23071,
		Whitelist: []string{"domain example.com"}, BuildMode: "auto", JumpPolicy: "auto", SRSRef: "https://example.invalid/whitelist.srs?key=private-key",
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
	if bytes.Contains(data, []byte("private-password")) || line.WhitelistSourceEnv != "NB_WHITELIST_SOURCE_URL" ||
		line.ExtraEnvironment[line.WhitelistSourceEnv] != plan.SRSRef {
		t.Fatal("runtime plan persisted a secret value or lost the SRS source reference")
	}
	if line.UpstreamMbps != 6 || line.DownstreamMbps != 14 {
		t.Fatalf("dynamic line lost asymmetric rates: %+v", line)
	}
	sourceData, err := os.ReadFile(filepath.Join(line.StateDir, "source-machines.json"))
	if err != nil {
		t.Fatal(err)
	}
	var source struct {
		Entry struct {
			Name string `json:"name"`
		} `json:"entry"`
		Middle struct {
			Name string `json:"name"`
		} `json:"middle"`
		Exit struct {
			Name  string `json:"name"`
			OutIP string `json:"outip"`
		} `json:"exit"`
		Exits []struct {
			Capacity int    `json:"capacity"`
			Name     string `json:"name"`
			Fixed    string `json:"fixed_exit"`
		} `json:"exits"`
	}
	if err := json.Unmarshal(sourceData, &source); err != nil {
		t.Fatal(err)
	}
	if source.Exit.OutIP != plan.ExitBindIP || len(source.Exits) != 1 || source.Exits[0].Capacity != 0 {
		t.Fatalf("package bandwidth leaked into route session capacity: %#v", source.Exits)
	}
	if source.Entry.Name != "entry-1" || source.Middle.Name != "relay-1" || source.Exit.Name != "exit-1" ||
		source.Exits[0].Name != "exit-1" || source.Exits[0].Fixed != "exit-1" {
		t.Fatalf("protocol topology used mutable display names: %#v", source)
	}
	invalidPlan := plan
	invalidPlan.ExitBindIP = "2001:db8::1"
	if _, err = runner.dynamicLine(Operation{ID: "op-invalid", LineID: invalidPlan.LineID, Kind: "line.open"}, requestValues{Plan: invalidPlan}, t.TempDir()); err == nil {
		t.Fatal("dynamic line accepted an IPv6 exit bind address")
	}
	invalidPlan = plan
	invalidPlan.SocksPort = 1080
	if _, err = runner.dynamicLine(Operation{ID: "op-invalid-port", LineID: invalidPlan.LineID, Kind: "line.open"}, requestValues{Plan: invalidPlan}, t.TempDir()); err == nil || !strings.Contains(err.Error(), "线路端口超出") {
		t.Fatalf("dynamic line did not reject an out-of-range port in Chinese: %v", err)
	}
	steps, err := runner.steps(line, Operation{ID: "op-new", LineID: plan.LineID, Kind: "line.open"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil || len(steps) < 3 || steps[0].Stage != "whitelist-fetch" || steps[len(steps)-1].Stage != "whitelist" {
		t.Fatalf("unexpected dynamic open steps: %#v err=%v", steps, err)
	}
	for _, argument := range steps[0].Args {
		if strings.Contains(argument, "private-key") {
			t.Fatal("whitelist URL leaked into the command line")
		}
	}
	provisionArgs := strings.Join(steps[1].Args, " ")
	if !strings.Contains(provisionArgs, "--upstream-mbps 6") || !strings.Contains(provisionArgs, "--downstream-mbps 14") {
		t.Fatalf("provision command lost directional rates: %s", provisionArgs)
	}
	upgradeSteps, err := runner.steps(line, Operation{ID: "op-upgrade", LineID: plan.LineID, Kind: "line.upgrade"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil || len(upgradeSteps) != 4 || upgradeSteps[0].Stage != "build" ||
		upgradeSteps[1].Stage != "whitelist-fetch" || upgradeSteps[2].Stage != "whitelist" || upgradeSteps[3].Stage != "deploy" {
		t.Fatalf("upgrade does not deploy whitelist before binary: %#v err=%v", upgradeSteps, err)
	}
	defaultPlan := plan
	defaultPlan.LineID, defaultPlan.InstanceID = "line-default", "line-default_1"
	defaultPlan.SRSRef, defaultPlan.Whitelist = "", nil
	defaultLine, err := runner.dynamicLine(Operation{ID: "op-default", LineID: defaultPlan.LineID, Kind: "line.open"}, requestValues{Plan: defaultPlan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defaultRules, err := os.ReadFile(defaultLine.WhitelistFile)
	if err != nil || !bytes.Contains(defaultRules, []byte("domain default.example")) ||
		!bytes.Contains(defaultRules, []byte("domain_exact odr.itunes.apple.com")) {
		t.Fatalf("empty plan did not receive default whitelist: %q err=%v", defaultRules, err)
	}
}

type fakeRunner struct {
	calls atomic.Int64
	delay time.Duration
}

func (runner *fakeRunner) Run(_ context.Context, operation Operation) (Result, error) {
	runner.calls.Add(1)
	if runner.delay > 0 {
		time.Sleep(runner.delay)
	}
	return Result{Deployment: "dep-2", Profile: operation.LineID + ":1", LogFile: "worker.log", Message: "ok"}, nil
}

type fakeSnapshotRunner struct{ fakeRunner }

func (runner *fakeSnapshotRunner) CollectSnapshots(_ context.Context, line LineSpec) ([]Snapshot, error) {
	return []Snapshot{{LineID: line.LineID, NodeID: "entry-1", Role: "entry", WorkerID: "0",
		ObservedAt: time.Now().UTC().Format(time.RFC3339Nano), Health: "ok",
		Deployment: "dep-1", Profile: line.LineID + ":1"}}, nil
}

type fakeDynamicSnapshotRunner struct{ fakeSnapshotRunner }

func (runner *fakeDynamicSnapshotRunner) resolveSnapshotLine(plan dynamicPlan) (LineSpec, error) {
	return LineSpec{LineID: plan.LineID}, nil
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

func TestClientCollectsDynamicLineSnapshots(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Dynamic.Enabled = true
	registry.Dynamic.ResourceGroups = []string{"test-group"}
	registry.Dynamic.Operations = []string{"line.validate"}
	var lines sync.Map
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.Header.Get("Authorization") != "Bearer agent-secret" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/agent/v1/line-plans":
			_, _ = w.Write([]byte(`{"plans":[{"line_id":"line-dynamic","resource_group":"test-group"}]}`))
		case "/agent/v1/snapshots":
			var snapshot Snapshot
			if json.NewDecoder(request.Body).Decode(&snapshot) != nil {
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			lines.Store(snapshot.LineID, true)
			_, _ = w.Write([]byte(`{"status":"accepted"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeDynamicSnapshotRunner{}, ClientConfig{BaseURL: server.URL, Token: "agent-secret"})
	if err != nil {
		t.Fatal(err)
	}
	client.collectSnapshots(context.Background())
	for _, lineID := range []string{"line-1", "line-dynamic"} {
		if _, ok := lines.Load(lineID); !ok {
			t.Fatalf("snapshot missing for %s", lineID)
		}
	}
}

func TestClientSyncsExistingClientConfiguration(t *testing.T) {
	registry := testRegistry(t, nil)
	lineID := "line-existing"
	directory := filepath.Join(registry.StateDir, "lines", lineID, "provision", lineID)
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	clientURL := "socks5://user:password@192.0.2.10:1080#line-existing"
	if err := os.WriteFile(filepath.Join(directory, "client.json"), []byte(`{"shadowrocket_url":"`+clientURL+`"}`), 0600); err != nil {
		t.Fatal(err)
	}
	var received atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/agent/v1/lines/line-existing/client-config" || request.Header.Get("Authorization") != "Bearer agent-secret" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		var payload map[string]string
		if json.NewDecoder(request.Body).Decode(&payload) != nil || payload["client_url"] != clientURL {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		received.Store(true)
		_, _ = w.Write([]byte(`{"status":"attached"}`))
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeRunner{}, ClientConfig{BaseURL: server.URL, Token: "agent-secret"})
	if err != nil {
		t.Fatal(err)
	}
	if err = client.syncClientConfigs(context.Background()); err != nil || !received.Load() {
		t.Fatalf("sync received=%v err=%v", received.Load(), err)
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
	runner := &fakeRunner{delay: 70 * time.Millisecond}
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
	if heartbeat.Load() < 3 || completed.Load() != 1 || runner.calls.Load() != 1 {
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

func TestWorkerLocalSecretReusesStaticMachineCredentials(t *testing.T) {
	registry := testRegistry(t, nil)
	machines := `{"edges":[{"name":"entry-1","host":"192.0.2.1","port":22,"user":"root","password":"entry-secret"}],` +
		`"relays":[{"name":"relay-1","host":"192.0.2.2","port":22,"user":"root","password":"relay-secret"}],` +
		`"terminals":[{"name":"exit-1","host":"192.0.2.3","port":2273,"user":"root","password":"exit-secret"}]}`
	if err := os.WriteFile(registry.Lines[0].SourceMachinesFile, []byte(machines), 0600); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(registry)
	for role, expected := range map[string]string{"entry": "entry-secret", "relay": "relay-secret", "exit": "exit-secret"} {
		secret, err := runner.resolveSecret("worker-local:line-1:" + role)
		if err != nil || secret.Password != expected {
			t.Fatalf("resolve %s password=%q err=%v", role, secret.Password, err)
		}
	}
}
