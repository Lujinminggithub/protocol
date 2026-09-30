package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func fileFingerprint(path string) (uint64, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	hash := fnv.New64a()
	_, _ = hash.Write(data)
	return hash.Sum64(), nil
}

func (r *Runner) maintainTransportProfile(ctx context.Context, line LineSpec) error {
	lineState := line.StateDir
	if lineState == "" {
		lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
	}
	state := loadRollout(filepath.Join(lineState, "transport", "rollout.json"))
	if state.Status != "committed" || state.Generation == 0 {
		return nil
	}
	environment, err := r.environment(line)
	if err != nil {
		return err
	}
	logPath := filepath.Join(lineState, "transport", "maintenance.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer logFile.Close()
	converged := true
	for _, role := range []string{"entry", "middle", "exit"} {
		fingerprint, _ := strconv.ParseUint(state.Fingerprints[role], 16, 64)
		if fingerprint == 0 || r.profileCommand(ctx, line, environment, logFile, "status", role, state.Generation, fingerprint, "") != nil {
			converged = false
		}
	}
	if converged {
		return nil
	}
	for _, role := range []string{"entry", "middle", "exit"} {
		profile := state.Profiles[role]
		fingerprint, hashErr := fileFingerprint(profile)
		if hashErr != nil {
			return hashErr
		}
		if err = r.profileCommand(ctx, line, environment, logFile, "prepare", role, state.Generation, fingerprint, profile); err != nil {
			return fmt.Errorf("profile reconciliation prepare failed for %s/%s: %w", line.LineID, role, err)
		}
	}
	for _, role := range []string{"exit", "middle", "entry"} {
		if err = r.profileCommand(ctx, line, environment, logFile, "commit", role, state.Generation, 0, ""); err != nil {
			return fmt.Errorf("profile reconciliation commit failed for %s/%s: %w", line.LineID, role, err)
		}
	}
	return nil
}

func (r *Runner) maintainWhitelist(ctx context.Context, line LineSpec) error {
	environment, err := r.environment(line)
	if err != nil {
		return fmt.Errorf("whitelist maintenance environment %s: %w", line.LineID, err)
	}
	if err = os.MkdirAll(line.StateDir, 0700); err != nil {
		return err
	}
	logPath := filepath.Join(line.StateDir, "whitelist-maintenance.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	var captured bytes.Buffer
	syncStep := commandStep{Name: r.registry.Python, Stage: "whitelist-fetch", Args: []string{
		filepath.Join(r.registry.Root, "tools", "whitelist_sync.py"), "--source-env", line.WhitelistSourceEnv,
		"--mode", "auto", "--sing-box", line.SingBox, "--state-dir", filepath.Join(line.StateDir, "whitelist-sync"),
		"--output", line.WhitelistFile}}
	syncErr := r.execute(ctx, syncStep, environment, io.MultiWriter(logFile, &captured))
	_ = logFile.Close()
	if syncErr != nil {
		return fmt.Errorf("whitelist refresh failed for %s: %w", line.LineID, syncErr)
	}
	if !strings.Contains(captured.String(), "WHITELIST_SYNC_CHANGED=1") {
		return nil
	}
	if _, statErr := os.Stat(line.HostsFile); errors.Is(statErr, os.ErrNotExist) {
		return nil
	}
	deploy := commandStep{Name: r.registry.Python, Stage: "whitelist-deploy", Args: []string{
		filepath.Join(r.registry.Root, "tools", "deploy.py"), "wl-push", "--whitelist", line.WhitelistFile}}
	logFile, err = os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	deployErr := r.execute(ctx, deploy, environment, logFile)
	_ = logFile.Close()
	if deployErr != nil {
		_ = os.Remove(filepath.Join(line.StateDir, "whitelist-sync", "whitelist.md5"))
		return fmt.Errorf("whitelist deploy failed for %s: %w", line.LineID, deployErr)
	}
	return nil
}

func (r *Runner) maintainQualification(ctx context.Context, line LineSpec) error {
	lineState := line.StateDir
	if lineState == "" {
		lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
	}
	pending := filepath.Join(lineState, "qualification-pending.json")
	if _, err := os.Stat(pending); errors.Is(err, os.ErrNotExist) {
		return nil
	}
	lockPath := pending + ".lock"
	lock, err := os.OpenFile(lockPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		if errors.Is(err, os.ErrExist) {
			return nil
		}
		return err
	}
	_ = lock.Close()
	defer os.Remove(lockPath)

	environment, err := r.environment(line)
	if err != nil {
		return err
	}
	inventory := filepath.Join(lineState, "provision-inventory.json")
	provision := commandStep{Name: r.registry.Python, Stage: "qualification",
		Args: []string{filepath.Join(r.registry.Root, "tools", "line_provision.py"), inventory,
			"--output-dir", filepath.Join(lineState, "provision"), "--line", line.LineID}}
	logPath := filepath.Join(lineState, "qualification-maintenance.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer logFile.Close()
	if err = r.execute(ctx, provision, environment, logFile); err != nil {
		return fmt.Errorf("qualification failed for %s: %w", line.LineID, err)
	}
	qualifiedHosts := filepath.Join(lineState, "provision", line.LineID, "deployment-hosts.json")
	qualifiedProfile := filepath.Join(lineState, "provision", line.LineID, "stable-profile.json")
	if _, err = os.Stat(qualifiedHosts); err != nil {
		return err
	}
	if _, err = os.Stat(qualifiedProfile); err != nil {
		return err
	}
	environment["NB_HOSTS_FILE"] = qualifiedHosts
	environment["NB_LINE_PROFILE_FILE"] = qualifiedProfile
	deploy := commandStep{Name: r.registry.Python, Stage: "qualification-deploy",
		Args: []string{filepath.Join(r.registry.Root, "tools", "deploy.py"), "deploy-socks",
			"--socks-port", strconv.Itoa(line.SocksPort)}}
	if err = r.execute(ctx, deploy, environment, logFile); err != nil {
		return fmt.Errorf("qualified profile deploy failed for %s: %w", line.LineID, err)
	}
	return os.Rename(pending, filepath.Join(lineState, "qualification-complete.json"))
}

func (r *Runner) Maintain(ctx context.Context) error {
	for _, line := range r.registry.Lines {
		if err := r.maintainQualification(ctx, line); err != nil {
			return err
		}
		if err := r.maintainTransportProfile(ctx, line); err != nil {
			return err
		}
		if line.WhitelistSourceEnv != "" {
			if err := r.maintainWhitelist(ctx, line); err != nil {
				return err
			}
		}
	}
	if !r.registry.Dynamic.Enabled {
		return nil
	}
	paths, err := filepath.Glob(filepath.Join(r.registry.StateDir, "lines", "*", "runtime-plan.json"))
	if err != nil {
		return err
	}
	for _, path := range paths {
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return readErr
		}
		var plan dynamicPlan
		if json.Unmarshal(data, &plan) != nil {
			continue
		}
		operation := Operation{ID: "maintenance", LineID: plan.LineID, Kind: "line.open"}
		line, lineErr := r.dynamicLine(operation, requestValues{Plan: plan}, filepath.Dir(path))
		if lineErr != nil {
			return fmt.Errorf("whitelist maintenance plan %s: %w", plan.LineID, lineErr)
		}
		if lineErr = r.maintainTransportProfile(ctx, line); lineErr != nil {
			return lineErr
		}
		if plan.SRSRef != "" {
			if lineErr = r.maintainWhitelist(ctx, line); lineErr != nil {
				return lineErr
			}
		}
	}
	return nil
}
