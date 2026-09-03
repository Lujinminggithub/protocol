package worker

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"nb-controlplane/internal/transportprofile"
)

type profileRolloutState struct {
	LineID             string            `json:"line_id"`
	DeploymentID       string            `json:"deployment_id,omitempty"`
	Generation         uint64            `json:"generation"`
	PreviousGeneration uint64            `json:"previous_generation"`
	Status             string            `json:"status"`
	Prepared           map[string]bool   `json:"prepared"`
	Committed          map[string]bool   `json:"committed"`
	Profiles           map[string]string `json:"profiles"`
	Fingerprints       map[string]string `json:"fingerprints"`
	UpdatedAt          string            `json:"updated_at"`
}

func atomicJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	temporary := path + ".tmp"
	if err = os.WriteFile(temporary, data, 0600); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

func loadRollout(path string) profileRolloutState {
	var state profileRolloutState
	data, err := os.ReadFile(path)
	if err == nil {
		_ = json.Unmarshal(data, &state)
	}
	return state
}

func reconcileRolloutState(lineID, deploymentID string, previous profileRolloutState) (profileRolloutState, bool) {
	if previous.Generation == 0 {
		return previous, false
	}
	if previous.LineID != lineID || deploymentID == "" || previous.DeploymentID != deploymentID {
		return profileRolloutState{}, true
	}
	return previous, false
}

func nextTransportGeneration(requested, floor uint64) uint64 {
	if requested <= floor {
		return floor + 1
	}
	return requested
}

func summarizeTransportRollout(line LineSpec, registryStateDir, deploymentID string) *transportRolloutResult {
	lineState := line.StateDir
	if lineState == "" {
		lineState = filepath.Join(registryStateDir, "lines", line.LineID)
	}
	state := loadRollout(filepath.Join(lineState, "transport", "rollout.json"))
	state, stale := reconcileRolloutState(line.LineID, deploymentID, state)
	if stale || state.Generation == 0 {
		return nil
	}
	result := &transportRolloutResult{Status: state.Status, Generation: state.Generation,
		PrepareOrder: []string{"entry", "middle", "exit"}, CommitOrder: []string{"exit", "middle", "entry"},
		Roles: map[string]transportRolloutRoleResult{}}
	for _, role := range result.PrepareOrder {
		result.Roles[role] = transportRolloutRoleResult{Prepared: state.Prepared[role],
			Committed: state.Committed[role], Readback: state.Status == "committed" && state.Committed[role],
			Fingerprint: state.Fingerprints[role]}
	}
	return result
}

func (r *Runner) profileCommand(ctx context.Context, line LineSpec, environment map[string]string,
	output io.Writer, phase, role string, generation uint64, fingerprint uint64, profile string) error {
	args := []string{filepath.Join(r.registry.Root, "tools", "transport_profile_apply.py"), phase,
		"--role", role, "--line-id", line.LineID, "--generation", strconv.FormatUint(generation, 10)}
	if phase == "prepare" {
		args = append(args, "--fingerprint", fmt.Sprintf("%016x", fingerprint), "--profile", profile)
	} else if phase == "status" && fingerprint != 0 {
		args = append(args, "--fingerprint", fmt.Sprintf("%016x", fingerprint))
	}
	step := commandStep{Name: r.registry.Python, Stage: "profile-" + phase, Args: args}
	var last error
	for attempt := 0; attempt < 8; attempt++ {
		if last = r.execute(ctx, step, environment, output); last == nil {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(time.Duration(min(attempt+1, 3)) * time.Second):
		}
	}
	return last
}

func (r *Runner) applyPlannedProfile(ctx context.Context, line LineSpec, deploymentID string, profile transportprofile.Profile,
	environment map[string]string, output io.Writer, sequence *int) (uint64, string, error) {
	lineState := line.StateDir
	if lineState == "" {
		lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
	}
	rolloutPath := filepath.Join(lineState, "transport", "rollout.json")
	previous := loadRollout(rolloutPath)
	var stale bool
	previous, stale = reconcileRolloutState(line.LineID, deploymentID, previous)
	if stale {
		stalePath := rolloutPath + ".stale-" + time.Now().UTC().Format("20060102T150405Z")
		if err := os.Rename(rolloutPath, stalePath); err != nil && !errors.Is(err, os.ErrNotExist) {
			return 0, "", fmt.Errorf("quarantine stale rollout state: %w", err)
		}
	}
	generation := nextTransportGeneration(profile.Generation, previous.Generation)
	if profile.LineID != line.LineID || generation == 0 {
		return 0, "", errors.New("central transport profile generation is missing, stale, or belongs to another line")
	}
	profile.Generation = generation
	profilePath := filepath.Join(lineState, "transport", fmt.Sprintf("profile-%d.json", generation))
	if err := atomicJSON(profilePath, profile); err != nil {
		return 0, "", err
	}
	state := profileRolloutState{LineID: line.LineID, DeploymentID: deploymentID, Generation: generation,
		PreviousGeneration: previous.Generation, Status: "preparing", Prepared: map[string]bool{},
		Committed: map[string]bool{}, Profiles: map[string]string{}, Fingerprints: map[string]string{}}
	var err error
	fingerprints := map[string]uint64{}
	for _, role := range []string{"entry", "middle", "exit"} {
		roleProfile, roleErr := profile.Role(role)
		if roleErr != nil {
			return 0, "", roleErr
		}
		contents, fingerprint, renderErr := transportprofile.Render(roleProfile)
		if renderErr != nil {
			return 0, "", renderErr
		}
		path := filepath.Join(lineState, "transport", fmt.Sprintf("%s-%d.conf", role, generation))
		if writeErr := os.WriteFile(path, contents, 0600); writeErr != nil {
			return 0, "", writeErr
		}
		state.Profiles[role], fingerprints[role] = path, fingerprint
		state.Fingerprints[role] = fmt.Sprintf("%016x", fingerprint)
	}
	save := func(status string) {
		state.Status, state.UpdatedAt = status, time.Now().UTC().Format(time.RFC3339Nano)
		_ = atomicJSON(rolloutPath, state)
	}
	save("preparing")
	for _, role := range []string{"entry", "middle", "exit"} {
		emitProgress(ctx, sequence, "profile-prepare", role+" preparing generation "+strconv.FormatUint(generation, 10))
		if err = r.profileCommand(ctx, line, environment, output, "prepare", role, generation, fingerprints[role], state.Profiles[role]); err != nil {
			for prepared := range state.Prepared {
				_ = r.profileCommand(ctx, line, environment, output, "abort", prepared, generation, 0, "")
			}
			save("prepare-failed")
			return generation, profilePath, fmt.Errorf("profile prepare failed on %s: %w", role, err)
		}
		state.Prepared[role] = true
		save("preparing")
	}
	save("committing")
	for _, role := range []string{"exit", "middle", "entry"} {
		emitProgress(ctx, sequence, "profile-commit", role+" committing generation "+strconv.FormatUint(generation, 10))
		if err = r.profileCommand(ctx, line, environment, output, "commit", role, generation, 0, ""); err != nil {
			if state.PreviousGeneration > 0 {
				for committed := range state.Committed {
					_ = r.profileCommand(ctx, line, environment, output, "rollback", committed, state.PreviousGeneration, 0, "")
				}
			}
			for prepared := range state.Prepared {
				if !state.Committed[prepared] {
					_ = r.profileCommand(ctx, line, environment, output, "abort", prepared, generation, 0, "")
				}
			}
			save("commit-failed")
			return generation, profilePath, fmt.Errorf("profile commit failed on %s: %w", role, err)
		}
		state.Committed[role] = true
		save("committing")
	}
	for _, role := range []string{"entry", "middle", "exit"} {
		if err = r.profileCommand(ctx, line, environment, output, "status", role, generation, fingerprints[role], ""); err != nil {
			save("readback-failed")
			return generation, profilePath, errors.New("profile status readback failed on " + role)
		}
	}
	save("committed")
	return generation, profilePath, nil
}
