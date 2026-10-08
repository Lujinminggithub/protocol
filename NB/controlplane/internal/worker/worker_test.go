package worker

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

func trustedDynamicNode(t *testing.T, id, role, host string, port int, secretRef string) dynamicNode {
	t.Helper()
	_, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	key := signer.PublicKey()
	return dynamicNode{DeviceID: id, Role: role, Device: dynamicDevice{ID: id, Name: id, Host: host,
		SSHPort: port, SSHUser: "root", SecretRef: secretRef,
		SSHHostKey: base64.StdEncoding.EncodeToString(key.Marshal()), SSHHostKeyType: key.Type(),
		SSHHostKeySHA256: ssh.FingerprintSHA256(key), SSHHostKeyStatus: "trusted"}}
}

func startWorkerSSHServer(t *testing.T, password string) (string, int, ssh.PublicKey) {
	t.Helper()
	_, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	configuration := &ssh.ServerConfig{PasswordCallback: func(metadata ssh.ConnMetadata, supplied []byte) (*ssh.Permissions, error) {
		if metadata.User() == "root" && string(supplied) == password {
			return nil, nil
		}
		return nil, errors.New("password rejected")
	}}
	configuration.AddHostKey(signer)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	go func() {
		for {
			connection, acceptErr := listener.Accept()
			if acceptErr != nil {
				return
			}
			go func() {
				server, _, _, _ := ssh.NewServerConn(connection, configuration)
				if server != nil {
					_ = server.Close()
				} else {
					_ = connection.Close()
				}
			}()
		}
	}()
	address := listener.Addr().(*net.TCPAddr)
	return "127.0.0.1", address.Port, signer.PublicKey()
}

func TestDynamicSSHAccessRejectsInvalidPasswordDuringPreparation(t *testing.T) {
	host, port, publicKey := startWorkerSSHServer(t, "correct-password")
	t.Setenv("NB_TEST_EXIT_PASSWORD", "wrong-password")
	node := trustedDynamicNode(t, "exit-1", "exit", host, port, "env:NB_TEST_EXIT_PASSWORD")
	node.Device.SSHHostKey = base64.StdEncoding.EncodeToString(publicKey.Marshal())
	node.Device.SSHHostKeyType = publicKey.Type()
	node.Device.SSHHostKeySHA256 = ssh.FingerprintSHA256(publicKey)
	err := NewRunner(testRegistry(t, nil)).verifyDynamicSSHAccess(context.Background(), dynamicPlan{Nodes: []dynamicNode{node}})
	if err == nil || !strings.Contains(err.Error(), "设备 exit-1 SSH 密码认证失败") {
		t.Fatalf("unexpected SSH credential preflight error: %v", err)
	}
}

func TestOptimizeCapabilityIsDerivedWithoutRegistryMigration(t *testing.T) {
	line := LineSpec{EnabledOperations: []string{"line.open", "line.validate", "line.tune"}}
	if !line.Allows("line.optimize") || !slices.Contains(line.SortedOperations(), "line.optimize") {
		t.Fatalf("derived operations=%v", line.SortedOperations())
	}
	line.EnabledOperations = []string{"line.validate"}
	if line.Allows("line.optimize") || slices.Contains(line.SortedOperations(), "line.optimize") {
		t.Fatalf("optimize advertised without tune: %v", line.SortedOperations())
	}
}

func TestOptimizeUsesValidationStep(t *testing.T) {
	runner := NewRunner(testRegistry(t, []string{"line.validate", "line.tune"}))
	line := runner.registry.Lines[0]
	steps, err := runner.steps(line, Operation{ID: "op-optimize", LineID: line.LineID, Kind: "line.optimize"}, requestValues{}, t.TempDir())
	if err != nil || len(steps) != 2 || steps[0].Stage != "probe-preflight" ||
		!slices.Contains(steps[0].Args, "--preflight") || steps[1].Stage != "validate" ||
		!slices.Contains(steps[1].Args, "--active") {
		t.Fatalf("optimize steps=%+v err=%v", steps, err)
	}
	if !slices.Contains(steps[1].Args, "--minimum-throughput-ratio") ||
		!slices.Contains(steps[1].Args, "0.95") || !slices.Contains(steps[1].Args, "--duration") ||
		!slices.Contains(steps[1].Args, "90") {
		t.Fatalf("optimize qualification contract=%v", steps[1].Args)
	}
	for _, kind := range []string{"line.open", "line.validate", "line.optimize"} {
		if !operationNeedsProbeCleanup(kind) {
			t.Fatalf("%s did not require probe cleanup", kind)
		}
	}
	if operationNeedsProbeCleanup("line.tune") {
		t.Fatal("profile-only tune unexpectedly requires probe cleanup")
	}
}

func TestOptimizeStopsAtRejectedValidationAdmission(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate", "line.tune"})
	script := `import json,sys
output=sys.argv[sys.argv.index("--output")+1]
with open(output,"w",encoding="utf-8") as handle:
 json.dump({"schema_version":2,"admission":{"status":"rejected","reasons":["insufficient-downlink"],"achieved_upstream_mbps":10,"achieved_downstream_mbps":6.5},"segments":{}},handle)
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "line_probe.py"), []byte(script), 0600); err != nil {
		t.Fatal(err)
	}
	cleanupMarker := filepath.Join(registry.StateDir, "probe-cleanup-rejected")
	cleanupScript := `import pathlib
pathlib.Path(` + strconv.Quote(cleanupMarker) + `).write_text("cleaned",encoding="utf-8")
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "probe_cleanup.py"), []byte(cleanupScript), 0600); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(registry)
	result, err := runner.Run(t.Context(), Operation{ID: "op-rejected", LineID: "line-1", Kind: "line.optimize"})
	if err == nil || !strings.Contains(err.Error(), "insufficient-downlink") {
		t.Fatalf("rejected validation error=%v result=%+v", err, result)
	}
	if result.Failure == nil || result.Failure.Stage != "validation" ||
		!bytes.Contains(result.Evidence, []byte(`"status": "rejected"`)) {
		t.Fatalf("rejected validation was not preserved: %+v", result)
	}
	if _, statErr := os.Stat(filepath.Join(registry.StateDir, "op-rejected", "optimize-checkpoint.json")); !os.IsNotExist(statErr) {
		t.Fatalf("rejected validation created a tune checkpoint: %v", statErr)
	}
	if _, statErr := os.Stat(cleanupMarker); statErr != nil {
		t.Fatalf("rejected validation did not clean probe sessions: %v", statErr)
	}
}

func TestValidateCleansProbeSessionsAfterSuccess(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate"})
	probe := `import json,sys
output=sys.argv[sys.argv.index("--output")+1]
with open(output,"w",encoding="utf-8") as handle: json.dump({"schema_version":2},handle)
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "line_probe.py"), []byte(probe), 0600); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(registry.StateDir, "probe-cleanup-success")
	cleanup := `import pathlib
pathlib.Path(` + strconv.Quote(marker) + `).write_text("cleaned",encoding="utf-8")
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "probe_cleanup.py"), []byte(cleanup), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewRunner(registry).Run(t.Context(), Operation{ID: "op-validate-cleanup", LineID: "line-1", Kind: "line.validate"}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(marker); err != nil {
		t.Fatalf("successful validation did not clean probe sessions: %v", err)
	}
}

func TestValidateFailsWhenProbeCleanupCannotBeConfirmed(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate"})
	probe := `import json,sys
output=sys.argv[sys.argv.index("--output")+1]
with open(output,"w",encoding="utf-8") as handle: json.dump({"schema_version":2},handle)
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "line_probe.py"), []byte(probe), 0600); err != nil {
		t.Fatal(err)
	}
	cleanup := `import sys
if "--preflight" not in sys.argv:
 raise RuntimeError("probe sessions remain")
`
	if err := os.WriteFile(filepath.Join(registry.Root, "tools", "probe_cleanup.py"), []byte(cleanup), 0600); err != nil {
		t.Fatal(err)
	}
	result, err := NewRunner(registry).Run(t.Context(), Operation{ID: "op-cleanup-failed", LineID: "line-1", Kind: "line.validate"})
	if err == nil || result.Failure == nil || result.Failure.Stage != "probe-cleanup" ||
		!strings.Contains(result.Failure.LogExcerpt, "probe sessions remain") {
		t.Fatalf("cleanup failure was not enforced: err=%v result=%+v", err, result)
	}
}

func TestOperationLocksActualDevices(t *testing.T) {
	client := &Client{}
	request := func(line, entry, relay, exit string) json.RawMessage {
		data, _ := json.Marshal(requestValues{Plan: dynamicPlan{LineID: line, Nodes: []dynamicNode{
			{DeviceID: entry, Role: "entry"}, {DeviceID: relay, Role: "relay"}, {DeviceID: exit, Role: "exit"},
		}}})
		return data
	}
	first := Operation{ID: "op-1", LineID: "line-1", Kind: "line.optimize", Request: request("line-1", "entry-a", "relay-a", "exit-a")}
	shared := Operation{ID: "op-2", LineID: "line-2", Kind: "line.open", Request: request("line-2", "entry-b", "relay-a", "exit-b")}
	disjoint := Operation{ID: "op-3", LineID: "line-3", Kind: "line.open", Request: request("line-3", "entry-c", "relay-c", "exit-c")}
	unlockFirst := client.lockOperation(first)
	sharedAcquired := make(chan func(), 1)
	go func() { sharedAcquired <- client.lockOperation(shared) }()
	select {
	case unlock := <-sharedAcquired:
		unlock()
		t.Fatal("operation sharing relay acquired lock")
	case <-time.After(30 * time.Millisecond):
	}
	disjointAcquired := make(chan func(), 1)
	go func() { disjointAcquired <- client.lockOperation(disjoint) }()
	select {
	case unlock := <-disjointAcquired:
		unlock()
	case <-time.After(time.Second):
		t.Fatal("disjoint topology did not execute concurrently")
	}
	unlockFirst()
	select {
	case unlock := <-sharedAcquired:
		unlock()
	case <-time.After(time.Second):
		t.Fatal("shared topology did not resume after lock release")
	}
	build := Operation{ID: "build-1", LineID: "__node_release__", Kind: "node.release.build"}
	if keys := client.operationLockKeys(build); !slices.Equal(keys, []string{"global:node-build"}) {
		t.Fatalf("build lock keys=%v", keys)
	}
}

func TestDynamicLineRejectsUntrustedDeviceBeforePreparingDeployment(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines = nil
	registry.Dynamic = DynamicConfig{Enabled: true, ResourceGroups: []string{"shared-1"}, Operations: []string{"line.open"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599, UDPPortMin: 22048, UDPPortMax: 65535}
	for _, name := range []string{"ENTRY", "RELAY", "EXIT"} {
		t.Setenv("NB_TEST_"+name, "private-password")
	}
	nodes := []dynamicNode{
		trustedDynamicNode(t, "entry-1", "entry", "192.0.2.1", 22, "env:NB_TEST_ENTRY"),
		trustedDynamicNode(t, "relay-1", "relay", "192.0.2.2", 22, "env:NB_TEST_RELAY"),
		trustedDynamicNode(t, "exit-1", "exit", "192.0.2.3", 5222, "env:NB_TEST_EXIT"),
	}
	nodes[2].Device.SSHHostKeyStatus = "pending"
	plan := dynamicPlan{LineID: "line-new", ResourceGroup: "shared-1", InstanceID: "line-new_1", BandwidthMbps: 5,
		UpstreamMbps: 5, DownstreamMbps: 5, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
		UDPPortMin: 22048, UDPPortMax: 23071, BuildMode: "auto", JumpPolicy: "auto", Nodes: nodes}
	lineState := filepath.Join(registry.StateDir, "lines", plan.LineID)
	_, err := NewRunner(registry).dynamicLine(Operation{ID: "op-new", LineID: plan.LineID, Kind: "line.open"}, requestValues{Plan: plan}, t.TempDir())
	if err == nil || err.Error() != "设备 exit-1 尚未完成 SSH 主机密钥登记" {
		t.Fatalf("unexpected untrusted-device error: %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(lineState, "source-machines.json")); !os.IsNotExist(statErr) {
		t.Fatalf("untrusted plan wrote deployment inputs: %v", statErr)
	}
}

func TestDynamicLineReplacesKnownHostsWithConfirmedPlanDevices(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines = nil
	registry.Dynamic = DynamicConfig{Enabled: true, ResourceGroups: []string{"shared-1"}, Operations: []string{"line.open"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599, UDPPortMin: 22048, UDPPortMax: 65535}
	for _, name := range []string{"ENTRY", "RELAY", "EXIT"} {
		t.Setenv("NB_TEST_"+name, "private-password")
	}
	nodes := []dynamicNode{
		trustedDynamicNode(t, "entry-1", "entry", "192.0.2.1", 22, "env:NB_TEST_ENTRY"),
		trustedDynamicNode(t, "relay-1", "relay", "192.0.2.2", 22, "env:NB_TEST_RELAY"),
		trustedDynamicNode(t, "exit-1", "exit", "192.0.2.3", 5222, "env:NB_TEST_EXIT"),
	}
	plan := dynamicPlan{LineID: "line-new", ResourceGroup: "shared-1", InstanceID: "line-new_1", BandwidthMbps: 5,
		UpstreamMbps: 5, DownstreamMbps: 5, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
		UDPPortMin: 22048, UDPPortMax: 23071, BuildMode: "auto", JumpPolicy: "auto", Nodes: nodes}
	lineState := filepath.Join(registry.StateDir, "lines", plan.LineID)
	knownHosts := filepath.Join(lineState, "security", "known_hosts")
	if err := os.MkdirAll(filepath.Dir(knownHosts), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(knownHosts, []byte("stale.example ssh-ed25519 AAAA\n"), 0600); err != nil {
		t.Fatal(err)
	}
	line, err := NewRunner(registry).dynamicLine(Operation{ID: "op-new", LineID: plan.LineID, Kind: "line.open"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	contents, err := os.ReadFile(line.KnownHostsFile)
	if err != nil {
		t.Fatal(err)
	}
	text := string(contents)
	if strings.Contains(text, "stale.example") || !strings.Contains(text, "[192.0.2.3]:5222 "+nodes[2].Device.SSHHostKeyType+" "+nodes[2].Device.SSHHostKey) {
		t.Fatalf("known_hosts was not rebuilt from confirmed devices: %q", text)
	}
	if len(strings.Split(strings.TrimSpace(text), "\n")) != 3 {
		t.Fatalf("known_hosts contains unexpected entries: %q", text)
	}
}

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

func TestRequiredPublicDNSWhitelistRules(t *testing.T) {
	unrestricted := withRequiredPublicDNSWhitelistRules([]string{"domain example.com"})
	for _, rule := range []string{"ip 1.1.1.1/32", "ip 8.8.8.8/32", "ip 9.9.9.9/32", "ip 114.114.114.114/32"} {
		if !slices.Contains(unrestricted, rule) {
			t.Fatalf("unrestricted whitelist is missing %q: %#v", rule, unrestricted)
		}
	}
	if slices.Contains(unrestricted, "port 53") {
		t.Fatalf("unrestricted whitelist unexpectedly gained a global port restriction: %#v", unrestricted)
	}

	restricted := withRequiredPublicDNSWhitelistRules([]string{"domain example.com", "port 443"})
	if !slices.Contains(restricted, "port 53") {
		t.Fatalf("restricted whitelist did not preserve DNS access: %#v", restricted)
	}
}

func TestSummarizeTransportRolloutOmitsPrivatePaths(t *testing.T) {
	line := LineSpec{LineID: "line-1", StateDir: t.TempDir()}
	state := profileRolloutState{LineID: line.LineID, DeploymentID: "deployment-1", Generation: 7, Status: "committed",
		Prepared:     map[string]bool{"entry": true, "middle": true, "exit": true},
		Committed:    map[string]bool{"entry": true, "middle": true, "exit": true},
		Fingerprints: map[string]string{"entry": "entry-fp", "middle": "middle-fp", "exit": "exit-fp"},
		Profiles:     map[string]string{"entry": "/private/entry-7.conf"}}
	if err := atomicJSON(filepath.Join(line.StateDir, "transport", "rollout.json"), state); err != nil {
		t.Fatal(err)
	}
	summary := summarizeTransportRollout(line, "", "deployment-1")
	if summary == nil || summary.Generation != 7 || summary.Status != "committed" ||
		strings.Join(summary.CommitOrder, ",") != "exit,middle,entry" || !summary.Roles["middle"].Readback {
		t.Fatalf("unexpected rollout summary: %+v", summary)
	}
	encoded, err := json.Marshal(summary)
	if err != nil || bytes.Contains(encoded, []byte("/private/")) || !bytes.Contains(encoded, []byte("entry-fp")) {
		t.Fatalf("rollout JSON leaks paths or omits fingerprint: %s err=%v", encoded, err)
	}
}

func TestStaleRolloutFromAnotherDeploymentDoesNotBlockGeneration(t *testing.T) {
	previous := profileRolloutState{LineID: "line-1", DeploymentID: "old-deployment", Generation: 8, Status: "committed"}
	reconciled, stale := reconcileRolloutState("line-1", "new-deployment", previous)
	if !stale || reconciled.Generation != 0 {
		t.Fatalf("reconciled=%+v stale=%v", reconciled, stale)
	}
	if got := nextTransportGeneration(1, reconciled.Generation); got != 1 {
		t.Fatalf("generation=%d want 1", got)
	}
}

func TestSameDeploymentRolloutRebasesCentralGeneration(t *testing.T) {
	previous := profileRolloutState{LineID: "line-1", DeploymentID: "deployment-1", Generation: 8, Status: "committed"}
	reconciled, stale := reconcileRolloutState("line-1", "deployment-1", previous)
	if stale {
		t.Fatal("current deployment rollout was marked stale")
	}
	if got := nextTransportGeneration(2, reconciled.Generation); got != 9 {
		t.Fatalf("generation=%d want 9", got)
	}
}

func TestRemoteProfileStatusAdvancesGenerationWhenLocalStateWasLost(t *testing.T) {
	output := `PROFILE_RESULT={"role":"entry","phase":"status","generation":0,"workers":[{"worker":0,"response":{"active_generation":7,"active_fingerprint":"a"}},{"worker":1,"response":{"active_generation":7,"active_fingerprint":"a"}}]}`
	generation, err := parseActiveProfileGeneration(output)
	if err != nil {
		t.Fatal(err)
	}
	if got := nextTransportGeneration(1, generation); got != 8 {
		t.Fatalf("generation=%d want 8", got)
	}
}

func TestRemoteProfileStatusMustContainEveryWorkerGeneration(t *testing.T) {
	for _, output := range []string{
		`command completed without profile result`,
		`PROFILE_RESULT={"workers":[]}`,
		`PROFILE_RESULT={"workers":[{"worker":0,"response":{}}]}`,
	} {
		if _, err := parseActiveProfileGeneration(output); err == nil {
			t.Fatalf("invalid status accepted: %s", output)
		}
	}
}

func TestFailureDetailIncludesRedactedRootCauseAndExcerpt(t *testing.T) {
	logPath := filepath.Join(t.TempDir(), "worker.log")
	contents := "Traceback (most recent call last):\n  File \"deploy.py\", line 12\nRuntimeError: password=do-not-leak socket conflict\n"
	if err := os.WriteFile(logPath, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	detail := operationFailure("profile-rollout", errors.New("exit profile failed"), logPath)
	encoded, err := json.Marshal(detail)
	if err != nil {
		t.Fatal(err)
	}
	if detail.Stage != "profile-rollout" || !strings.Contains(detail.RootCause, "RuntimeError") ||
		!strings.Contains(detail.LogExcerpt, "deploy.py") || bytes.Contains(encoded, []byte("do-not-leak")) {
		t.Fatalf("unexpected failure detail: %s", encoded)
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

func TestCommandFailureSummaryPrefersLaterMissingBuildInput(t *testing.T) {
	path := filepath.Join(t.TempDir(), "worker.log")
	content := "RuntimeError: local source differs from the binary build inputs\n" +
		"Traceback (most recent call last):\n" +
		"FileNotFoundError: /opt/nb-controlplane/repo/src/nb_wait.c\n" +
		"subprocess.CalledProcessError: command returned non-zero exit status 1\n"
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	summary := commandFailureSummary(os.ErrInvalid, path)
	if summary != "FileNotFoundError: /opt/nb-controlplane/repo/src/nb_wait.c" {
		t.Fatalf("wrong root cause selected: %s", summary)
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
		return trustedDynamicNode(t, id, role, host, 22, "env:"+secret)
	}
	plan := dynamicPlan{LineID: "line-new", ResourceGroup: "renamed-production-group", InstanceID: "new", BandwidthMbps: 20,
		UpstreamMbps: 6, DownstreamMbps: 14,
		SocksPort: 1082, RelayPort: 4445, ExitPort: 4443, ExitBindIP: "192.0.2.30", UDPPortMin: 22048, UDPPortMax: 23071,
		DNSServers: []string{"9.9.9.9", "1.1.1.1"},
		Whitelist:  []string{"domain example.com"}, BuildMode: "auto", JumpPolicy: "auto", SRSRef: "https://example.invalid/whitelist.srs?key=private-key",
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
		Transport struct {
			Exit struct {
				DNSServers []string `json:"dns_servers"`
			} `json:"exit"`
		} `json:"transport"`
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
	if !slices.Equal(source.Transport.Exit.DNSServers, plan.DNSServers) {
		t.Fatalf("dynamic line lost exit DNS servers: %#v", source.Transport.Exit.DNSServers)
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
	if err != nil || len(steps) < 4 || steps[0].Stage != "probe-preflight" ||
		steps[1].Stage != "whitelist-fetch" || steps[len(steps)-1].Stage != "whitelist" {
		t.Fatalf("unexpected dynamic open steps: %#v err=%v", steps, err)
	}
	for _, argument := range steps[1].Args {
		if strings.Contains(argument, "private-key") {
			t.Fatal("whitelist URL leaked into the command line")
		}
	}
	provisionArgs := strings.Join(steps[2].Args, " ")
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

func TestDynamicLineBuildsSingleHKTopology(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines = nil
	registry.Dynamic = DynamicConfig{Enabled: true, Operations: []string{"line.open"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599,
		UDPPortMin: 22048, UDPPortMax: 65535}
	t.Setenv("NB_SINGLE_HK_PASSWORD", "single-hk-password")
	entry := trustedDynamicNode(t, "hk-1", "entry", "192.0.2.20", 22, "env:NB_SINGLE_HK_PASSWORD")
	exit := entry
	exit.Role = "exit"
	plan := dynamicPlan{LineID: "hk-single", ResourceGroup: "hk", InstanceID: "hk-single_1",
		TopologyMode: "single_hk", ServiceProfile: "general",
		BandwidthMbps: 5, UpstreamMbps: 5, DownstreamMbps: 5,
		SocksPort: 1082, RelayPort: 0, ExitPort: 4443, UDPPortMin: 22048, UDPPortMax: 23071,
		DNSServers: []string{"1.1.1.1"}, BuildMode: "auto", JumpPolicy: "direct",
		Nodes: []dynamicNode{entry, exit}}
	runner := NewRunner(registry)
	line, err := runner.dynamicLine(Operation{ID: "op-single", LineID: plan.LineID, Kind: "line.open"},
		requestValues{Plan: plan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if line.TopologyMode != "single_hk" || line.ServiceProfile != "general" || line.MiddlePort != 0 {
		t.Fatalf("single-HK line=%+v", line)
	}
	data, err := os.ReadFile(line.SourceMachinesFile)
	if err != nil {
		t.Fatal(err)
	}
	var source map[string]any
	if json.Unmarshal(data, &source) != nil || source["middle"] != nil || source["topology_mode"] != "single_hk" {
		t.Fatalf("single-HK source contains a Middle: %s", data)
	}
	steps, err := runner.steps(line, Operation{ID: "op-single", LineID: plan.LineID, Kind: "line.open"},
		requestValues{Plan: plan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	var provision commandStep
	for _, step := range steps {
		if step.Stage == "provision" {
			provision = step
			break
		}
	}
	args := strings.Join(provision.Args, " ")
	if !strings.Contains(args, "--topology-mode single_hk") || !strings.Contains(args, "--service-profile general") || strings.Contains(args, "--middle-port") {
		t.Fatalf("single-HK provision args=%s", args)
	}

	invalid := plan
	invalid.Nodes = append([]dynamicNode(nil), plan.Nodes...)
	invalid.Nodes[1].DeviceID, invalid.Nodes[1].Device.ID = "hk-2", "hk-2"
	if err = runner.validateDynamicPlan(Operation{LineID: plan.LineID, Kind: "line.open"}, invalid); err == nil || !strings.Contains(err.Error(), "同一台香港设备") {
		t.Fatalf("different single-HK devices error=%v", err)
	}
	invalid = plan
	invalid.Nodes = append(invalid.Nodes, trustedDynamicNode(t, "relay-1", "relay", "192.0.2.21", 22, "env:NB_SINGLE_HK_PASSWORD"))
	if err = runner.validateDynamicPlan(Operation{LineID: plan.LineID, Kind: "line.open"}, invalid); err == nil {
		t.Fatal("single-HK plan accepted a Relay")
	}
}

func TestDynamicDisableBeforeOpenUsesGeneratedSourceTopology(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines = nil
	registry.Dynamic = DynamicConfig{Enabled: true, Operations: []string{"line.disable"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599,
		UDPPortMin: 22048, UDPPortMax: 65535}
	for _, name := range []string{"ENTRY", "RELAY", "EXIT"} {
		t.Setenv("NB_DISABLE_"+name, "password-"+name)
	}
	node := func(id, role, host, secret string) dynamicNode {
		return trustedDynamicNode(t, id, role, host, 22, "env:"+secret)
	}
	plan := dynamicPlan{LineID: "line-never-opened", ResourceGroup: "renamed-group",
		InstanceID: "line-never-opened_1", BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10,
		SocksPort: 1082, RelayPort: 4445, ExitPort: 4443, UDPPortMin: 22048, UDPPortMax: 23071,
		DNSServers: []string{"1.1.1.1"}, BuildMode: "auto", JumpPolicy: "auto",
		Nodes: []dynamicNode{node("entry-1", "entry", "192.0.2.1", "NB_DISABLE_ENTRY"),
			node("relay-1", "relay", "192.0.2.2", "NB_DISABLE_RELAY"),
			node("exit-1", "exit", "192.0.2.3", "NB_DISABLE_EXIT")}}
	line, err := NewRunner(registry).dynamicLine(Operation{ID: "op-disable", LineID: plan.LineID,
		Kind: "line.disable"}, requestValues{Plan: plan}, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if line.HostsFile != line.SourceMachinesFile {
		t.Fatalf("disable hosts=%q source=%q", line.HostsFile, line.SourceMachinesFile)
	}
	data, err := os.ReadFile(line.SourceMachinesFile)
	if err != nil {
		t.Fatal(err)
	}
	var source struct {
		Paths map[string]string `json:"paths"`
	}
	if json.Unmarshal(data, &source) != nil || source.Paths["work_dir"] != "/etc/NB" {
		t.Fatalf("disable source topology lacks deployment paths: %s", data)
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

type blockingSnapshotRunner struct {
	fakeRunner
	started chan struct{}
	once    sync.Once
}

func (runner *blockingSnapshotRunner) CollectSnapshots(ctx context.Context, line LineSpec) ([]Snapshot, error) {
	if line.LineID == "line-blocked" {
		runner.once.Do(func() { close(runner.started) })
		<-ctx.Done()
		return nil, ctx.Err()
	}
	return []Snapshot{{LineID: line.LineID, NodeID: "entry-1", Role: "entry", WorkerID: "0",
		ObservedAt: time.Now().UTC().Format(time.RFC3339Nano), Health: "ok"}}, nil
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
			_, _ = w.Write([]byte(`{"plans":[{"line_id":"line-dynamic","resource_group":"renamed-group"}]}`))
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

func TestBlockedLineDoesNotSuppressLaterSnapshotCycles(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Lines[0].LineID = "line-blocked"
	var delivered atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/agent/v1/snapshots" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		var snapshot Snapshot
		if json.NewDecoder(request.Body).Decode(&snapshot) == nil && snapshot.LineID == "line-healthy" {
			delivered.Store(true)
		}
		_, _ = w.Write([]byte(`{"status":"accepted"}`))
	}))
	defer server.Close()
	runner := &blockingSnapshotRunner{started: make(chan struct{})}
	client, err := NewClient(registry, runner, ClientConfig{BaseURL: server.URL, Token: "agent-secret"})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go client.collectSnapshots(ctx)
	select {
	case <-runner.started:
	case <-time.After(time.Second):
		t.Fatal("blocked collector did not start")
	}
	client.registry.Lines = append(client.registry.Lines, LineSpec{LineID: "line-healthy"})
	client.collectSnapshots(context.Background())
	if !delivered.Load() {
		t.Fatal("healthy line was suppressed by an unrelated blocked collector")
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
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
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

func TestClientRetriesCompletionAfterTemporaryCentralBusy(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate"})
	var completeAttempts atomic.Int64
	var claimed atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/agent/v1/executors/heartbeat":
			_, _ = w.Write([]byte(`{"status":"ready"}`))
		case "/agent/v1/operations":
			if claimed.CompareAndSwap(false, true) {
				_, _ = w.Write([]byte(`{"operations":[{"id":"op-busy","line_id":"line-1","kind":"line.validate","request":{}}]}`))
			} else {
				_, _ = w.Write([]byte(`{"operations":[]}`))
			}
		case "/agent/v1/operations/op-busy/result":
			if completeAttempts.Add(1) < 3 {
				w.WriteHeader(http.StatusInternalServerError)
				_, _ = w.Write([]byte(`{"error":"database is locked (5) (SQLITE_BUSY)"}`))
				return
			}
			_, _ = w.Write([]byte(`{"status":"accepted"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeRunner{}, ClientConfig{BaseURL: server.URL, Token: "agent-secret",
		Version: "test", PollEvery: 10 * time.Millisecond, HeartbeatEvery: time.Hour, OperationTimeout: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err = client.Run(ctx); err != nil {
		t.Fatal(err)
	}
	if got := completeAttempts.Load(); got != 3 {
		t.Fatalf("completion attempts=%d, want 3", got)
	}
}

func TestClientHeartbeatsRecoverDuringLongOperation(t *testing.T) {
	registry := testRegistry(t, []string{"line.validate"})
	var heartbeat, completed atomic.Int64
	var claimed atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/agent/v1/executors/heartbeat":
			attempt := heartbeat.Add(1)
			if attempt == 2 {
				w.WriteHeader(http.StatusInternalServerError)
				_, _ = w.Write([]byte(`{"error":"temporary database busy"}`))
				return
			}
			_, _ = w.Write([]byte(`{"status":"ready"}`))
		case "/agent/v1/operations":
			if claimed.CompareAndSwap(false, true) {
				_, _ = w.Write([]byte(`{"operations":[{"id":"op-long","line_id":"line-1","kind":"line.validate","request":{}}]}`))
			} else {
				_, _ = w.Write([]byte(`{"operations":[]}`))
			}
		case "/agent/v1/operations/op-long/result":
			completed.Add(1)
			_, _ = w.Write([]byte(`{"status":"accepted"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeRunner{delay: 90 * time.Millisecond}, ClientConfig{
		BaseURL: server.URL, Token: "agent-secret", PollEvery: 5 * time.Millisecond,
		HeartbeatEvery: 10 * time.Millisecond, OperationTimeout: time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	if err = client.Run(ctx); err != nil {
		t.Fatal(err)
	}
	if heartbeat.Load() < 5 || completed.Load() != 1 {
		t.Fatalf("heartbeat did not recover during operation: heartbeat=%d completed=%d", heartbeat.Load(), completed.Load())
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

func TestParseRuntimeShardPortClaims(t *testing.T) {
	observed := time.Date(2026, 9, 7, 12, 0, 0, 0, time.UTC)
	entryConfig := "schema=1\ninstance_id=legacy-entry\narg=-r\narg=entry\narg=-l\narg=1083\nenv.NB_SOCKS_UDP_PORT_MIN=22048\nenv.NB_SOCKS_UDP_PORT_MAX=23071\n"
	entryClaims, err := parseRuntimeShardPortClaims("worker-1", "entry-device", "entry", map[string]string{
		"0/legacy.conf": entryConfig, "1/legacy.conf": entryConfig,
	}, observed)
	if err != nil {
		t.Fatal(err)
	}
	if len(entryClaims) != 2 || entryClaims[0].ResourceKind != "socks" || entryClaims[0].PortStart != 1083 ||
		entryClaims[1].ResourceKind != "udp" || entryClaims[1].PortStart != 22048 || entryClaims[1].PortEnd != 23071 {
		t.Fatalf("entry claims=%+v", entryClaims)
	}
	middleConfig := "schema=1\ninstance_id=legacy-middle\narg=-r\narg=middle\narg=-p\narg=4445\nenv.NB_WORKER_LANE_PORTS=on\n"
	middleClaims, err := parseRuntimeShardPortClaims("worker-1", "relay-device", "relay", map[string]string{
		"0/legacy.conf": middleConfig, "1/legacy.conf": middleConfig,
	}, observed)
	if err != nil {
		t.Fatal(err)
	}
	if len(middleClaims) != 1 || middleClaims[0].ResourceKind != "transport" ||
		middleClaims[0].PortStart != 4445 || middleClaims[0].PortEnd != 4446 {
		t.Fatalf("middle claims=%+v", middleClaims)
	}
}

type fakeRuntimePortRunner struct{}

func (*fakeRuntimePortRunner) Run(context.Context, Operation) (Result, error) { return Result{}, nil }
func (*fakeRuntimePortRunner) runtimePortBatches(_ context.Context, plans []dynamicPlan) ([]runtimePortClaimBatch, error) {
	return []runtimePortClaimBatch{{WorkerID: "worker-1", DeviceID: "entry-1", Role: "entry", ScanComplete: true,
		ObservedAt: "2026-09-07T12:00:00Z", Claims: []runtimePortClaim{{ResourceKind: "socks", InstanceID: "legacy", PortStart: 1082, PortEnd: 1082}}}}, nil
}

func TestPrepareRuntimePortsUploadsClaimsBeforePreflight(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Dynamic = DynamicConfig{Enabled: true, ResourceGroups: []string{"shared-1"}, Operations: []string{"line.open"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599, UDPPortMin: 22048, UDPPortMax: 65535}
	var claimsUploaded atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch request.URL.Path {
		case "/agent/v1/runtime-port-claims":
			claimsUploaded.Store(true)
			w.WriteHeader(http.StatusAccepted)
			_, _ = w.Write([]byte(`{"updated":1}`))
		case "/agent/v1/operations/op-runtime/port-preflight":
			if !claimsUploaded.Load() {
				w.WriteHeader(http.StatusConflict)
				return
			}
			_, _ = w.Write([]byte(`{"id":"op-runtime","line_id":"line-runtime","kind":"line.open","request":{"plan":{"line_id":"line-runtime","socks_port":1083}}}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	client, err := NewClient(registry, &fakeRuntimePortRunner{}, ClientConfig{BaseURL: server.URL, Token: "agent-secret"})
	if err != nil {
		t.Fatal(err)
	}
	request, _ := json.Marshal(map[string]any{"plan": dynamicPlan{LineID: "line-runtime", ResourceGroup: "shared-1"}})
	prepared, err := client.prepareRuntimePorts(context.Background(), Operation{ID: "op-runtime", LineID: "line-runtime", Kind: "line.open", Request: request})
	if err != nil || prepared.ID != "op-runtime" || !claimsUploaded.Load() {
		t.Fatalf("prepared=%+v uploaded=%v err=%v", prepared, claimsUploaded.Load(), err)
	}
}

func TestNodeReleaseBuildRejectsArchiveOutsideUploadDirectory(t *testing.T) {
	stateDir := filepath.Join(t.TempDir(), "data", "worker")
	root := filepath.Join(t.TempDir(), "repo")
	if err := os.MkdirAll(root, 0700); err != nil {
		t.Fatal(err)
	}
	runner := &Runner{registry: Registry{Root: root, StateDir: stateDir, Python: "python3"}}
	request := requestValues{UploadID: "source-test", Archive: filepath.Join(t.TempDir(), "source-test.tar.gz"),
		ArchiveSHA256: strings.Repeat("0", 64)}
	if _, err := runner.runNodeReleaseBuild(t.Context(), Operation{ID: "op-source", Kind: "node.release.build"}, request); err == nil || !strings.Contains(err.Error(), "受限上传目录") {
		t.Fatalf("outside archive was not rejected: %v", err)
	}
}

type fakeCleanupPortRunner struct{ residual bool }

func (*fakeCleanupPortRunner) Run(context.Context, Operation) (Result, error) { return Result{}, nil }
func (r *fakeCleanupPortRunner) runtimePortBatches(_ context.Context, plans []dynamicPlan) ([]runtimePortClaimBatch, error) {
	roles := []struct{ role, device string }{{"entry", "entry-1"}, {"relay", "relay-1"}, {"exit", "exit-1"}}
	result := make([]runtimePortClaimBatch, 0, len(roles))
	for _, item := range roles {
		claims := []runtimePortClaim{}
		if r.residual && item.role == "entry" {
			claims = append(claims, runtimePortClaim{ResourceKind: "socks", InstanceID: "line-cleanup_1-entry", PortStart: 1091, PortEnd: 1091})
		}
		result = append(result, runtimePortClaimBatch{WorkerID: "worker-1", DeviceID: item.device, Role: item.role,
			ObservedAt: "2026-09-07T12:00:00Z", ScanComplete: true, Claims: claims})
	}
	return result, nil
}

func TestConfirmRuntimeCleanupRequiresAllRolesAndNoResidualInstance(t *testing.T) {
	registry := testRegistry(t, nil)
	registry.Dynamic = DynamicConfig{Enabled: true, ResourceGroups: []string{"shared-1"}, Operations: []string{"line.disable"},
		SocksPortMin: 1082, SocksPortMax: 1199, RelayPortMin: 4445, RelayPortMax: 4599, UDPPortMin: 22048, UDPPortMax: 65535}
	posted := atomic.Int64{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/agent/v1/runtime-port-claims" {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		posted.Add(1)
		w.WriteHeader(http.StatusAccepted)
		_, _ = w.Write([]byte(`{"updated":0}`))
	}))
	defer server.Close()
	plan := dynamicPlan{LineID: "line-cleanup", ResourceGroup: "shared-1", InstanceID: "line-cleanup_1", Nodes: []dynamicNode{
		{DeviceID: "entry-1", Role: "entry"}, {DeviceID: "relay-1", Role: "relay"}, {DeviceID: "exit-1", Role: "exit"}}}
	request, _ := json.Marshal(map[string]any{"plan": plan, "delete_after_cleanup": true})
	operation := Operation{ID: "op-cleanup", LineID: plan.LineID, Kind: "line.disable", Request: request}
	client, err := NewClient(registry, &fakeCleanupPortRunner{}, ClientConfig{BaseURL: server.URL, Token: "agent"})
	if err != nil {
		t.Fatal(err)
	}
	if err = client.confirmRuntimeCleanup(context.Background(), operation); err != nil || posted.Load() != 3 {
		t.Fatalf("posted=%d err=%v", posted.Load(), err)
	}
	client.runner = &fakeCleanupPortRunner{residual: true}
	if err = client.confirmRuntimeCleanup(context.Background(), operation); err == nil || !strings.Contains(err.Error(), "仍存在") {
		t.Fatalf("residual cleanup error=%v", err)
	}
}
