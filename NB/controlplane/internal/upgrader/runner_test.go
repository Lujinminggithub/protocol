package upgrader

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"nb-controlplane/internal/worker"
)

func TestRunnerExecutesPlatformManifestAndReturnsEvidence(t *testing.T) {
	root := t.TempDir()
	candidate := filepath.Join(filepath.Dir(root), "source-candidates", "ready-test")
	if err := os.MkdirAll(filepath.Join(candidate, "build"), 0700); err != nil {
		t.Fatal(err)
	}
	manifest := filepath.Join(candidate, "build", "platform-release.json")
	if err := os.WriteFile(manifest, []byte(`{"release_id":"release-1"}`), 0600); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(root, "python3", filepath.Join(root, "state"))
	runner.execute = func(_ context.Context, name string, args []string, _ *os.File) ([]byte, error) {
		joined := strings.Join(append([]string{name}, args...), " ")
		if !strings.Contains(joined, "platform_upgrade.py upgrade") || !strings.Contains(joined, "--line-id line-a") {
			t.Fatalf("command=%s", joined)
		}
		return []byte(`PLATFORM_UPGRADE_JSON={"status":"succeeded","sessions_interrupted":7}` + "\n"), nil
	}
	request, _ := json.Marshal(map[string]any{
		"release": map[string]any{"candidate_root": candidate},
		"impact":  map[string]any{"seed_line_id": "line-a"},
	})
	result, err := runner.Run(t.Context(), worker.Operation{ID: "op-upgrade", LineID: "__platform__",
		Kind: "platform.upgrade", Request: request})
	if err != nil || !strings.Contains(string(result.Evidence), `"sessions_interrupted":7`) {
		t.Fatalf("result=%+v err=%v", result, err)
	}
}

func TestRunnerExecutesPlatformRollback(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "tools"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "tools", "platform_upgrade.py"), []byte("# test\n"), 0600); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(root, "python3", filepath.Join(root, "state"))
	runner.execute = func(_ context.Context, _ string, args []string, _ *os.File) ([]byte, error) {
		if !strings.Contains(strings.Join(args, " "), "platform_upgrade.py rollback --release-id release-1") {
			t.Fatalf("args=%v", args)
		}
		return []byte(`PLATFORM_UPGRADE_JSON={"status":"rolled_back"}` + "\n"), nil
	}
	result, err := runner.Run(t.Context(), worker.Operation{ID: "op-rollback", LineID: "__platform__",
		Kind: "platform.rollback", Request: json.RawMessage(`{"release_id":"release-1"}`)})
	if err != nil || !strings.Contains(string(result.Evidence), "rolled_back") {
		t.Fatalf("result=%+v err=%v", result, err)
	}
}
