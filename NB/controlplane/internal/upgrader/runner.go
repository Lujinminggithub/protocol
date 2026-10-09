package upgrader

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"nb-controlplane/internal/worker"
)

type commandExecutor func(context.Context, string, []string, *os.File) ([]byte, error)

type Runner struct {
	root    string
	python  string
	state   string
	execute commandExecutor
}

func NewRunner(root, python, state string) *Runner {
	if python == "" {
		python = "python3"
	}
	runner := &Runner{root: filepath.Clean(root), python: python, state: filepath.Clean(state)}
	runner.execute = func(ctx context.Context, name string, args []string, logFile *os.File) ([]byte, error) {
		command := exec.CommandContext(ctx, name, args...)
		command.Dir = runner.root
		var captured bytes.Buffer
		command.Stdout = &captured
		command.Stderr = logFile
		err := command.Run()
		return captured.Bytes(), err
	}
	return runner
}

func (r *Runner) Run(ctx context.Context, operation worker.Operation) (worker.Result, error) {
	if operation.LineID != "__platform__" || (operation.Kind != "platform.upgrade" && operation.Kind != "platform.rollback") {
		return worker.Result{}, errors.New("upgrader only accepts platform operations")
	}
	var script string
	var args []string
	if operation.Kind == "platform.rollback" {
		var request struct {
			ReleaseID          string `json:"release_id"`
			UpgradeOperationID string `json:"upgrade_operation_id"`
		}
		if json.Unmarshal(operation.Request, &request) != nil || request.ReleaseID == "" || request.UpgradeOperationID == "" {
			return worker.Result{}, errors.New("platform rollback request is invalid")
		}
		script = filepath.Join(r.root, "tools", "platform_upgrade.py")
		args = []string{script, "rollback", "--release-id", request.ReleaseID,
			"--upgrade-operation-id", request.UpgradeOperationID, "--operation-id", operation.ID}
	} else {
		var request struct {
			Release struct {
				CandidateRoot string `json:"candidate_root"`
			} `json:"release"`
			Impact struct {
				SeedLineID    string   `json:"seed_line_id"`
				AffectedLines []string `json:"affected_lines"`
			} `json:"impact"`
		}
		if json.Unmarshal(operation.Request, &request) != nil || request.Release.CandidateRoot == "" || request.Impact.SeedLineID == "" {
			return worker.Result{}, errors.New("platform upgrade request is invalid")
		}
		candidate := filepath.Clean(request.Release.CandidateRoot)
		manifest := filepath.Join(candidate, "build", "platform-release.json")
		script = filepath.Join(candidate, "tools", "platform_upgrade.py")
		controlledParent := filepath.Join(filepath.Dir(r.root), "source-candidates") + string(os.PathSeparator)
		if !filepath.IsAbs(candidate) || !strings.HasPrefix(candidate, controlledParent) {
			return worker.Result{}, errors.New("platform candidate is outside the controlled release directory")
		}
		if _, err := os.Stat(manifest); err != nil {
			return worker.Result{}, errors.New("platform candidate manifest is unavailable")
		}
		lines := request.Impact.AffectedLines
		if len(lines) == 0 {
			lines = []string{request.Impact.SeedLineID}
		}
		args = []string{script, "upgrade", "--manifest", manifest}
		for _, lineID := range lines {
			if strings.TrimSpace(lineID) == "" {
				return worker.Result{}, errors.New("platform upgrade impact contains an invalid line")
			}
			args = append(args, "--line-id", lineID)
		}
		args = append(args, "--operation-id", operation.ID)
	}
	operationDir := filepath.Join(r.state, operation.ID)
	if err := os.MkdirAll(operationDir, 0700); err != nil {
		return worker.Result{}, err
	}
	logPath := filepath.Join(operationDir, "upgrader.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return worker.Result{}, err
	}
	defer logFile.Close()
	_ = worker.EmitOperationEvent(ctx, worker.OperationEvent{Sequence: 1, Stage: "platform-upgrade", Status: "running", Message: "平台升级事务开始"})
	output, runErr := r.execute(ctx, r.python, args, logFile)
	if runErr != nil {
		return worker.Result{LogFile: logPath, Message: "平台升级失败"}, fmt.Errorf("platform upgrade failed: %w", runErr)
	}
	marker := []byte("PLATFORM_UPGRADE_JSON=")
	index := bytes.LastIndex(output, marker)
	if index < 0 {
		return worker.Result{LogFile: logPath}, errors.New("platform upgrade returned no evidence")
	}
	evidence := bytes.SplitN(output[index+len(marker):], []byte("\n"), 2)[0]
	if !json.Valid(evidence) {
		return worker.Result{LogFile: logPath}, errors.New("platform upgrade evidence is invalid")
	}
	_ = worker.EmitOperationEvent(ctx, worker.OperationEvent{Sequence: 2, Stage: "platform-upgrade", Status: "succeeded", Message: "平台升级事务完成"})
	return worker.Result{LogFile: logPath, Message: "平台升级完成", Evidence: json.RawMessage(evidence)}, nil
}
