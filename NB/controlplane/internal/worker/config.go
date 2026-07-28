package worker

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
)

var (
	safeID         = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,64}$`)
	safeDeployment = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,128}$`)
)

var allowedKinds = map[string]bool{
	"line.open": true, "line.validate": true, "line.upgrade": true,
	"line.rollback": true, "line.disable": true,
}

type Registry struct {
	SchemaVersion int        `json:"schema_version"`
	WorkerID      string     `json:"worker_id"`
	Root          string     `json:"root"`
	Python        string     `json:"python"`
	StateDir      string     `json:"state_dir"`
	Lines         []LineSpec `json:"lines"`
}

type LineSpec struct {
	LineID             string   `json:"line_id"`
	ResourceGroup      string   `json:"resource_group"`
	HostsFile          string   `json:"hosts_file"`
	SourceMachinesFile string   `json:"source_machines_file"`
	LineProfileFile    string   `json:"line_profile_file"`
	KnownHostsFile     string   `json:"known_hosts_file"`
	SecurityDir        string   `json:"security_dir"`
	ClientSecretFile   string   `json:"client_secret_file"`
	PackageMbps        float64  `json:"package_mbps"`
	SocksPort          int      `json:"socks_port"`
	EnabledOperations  []string `json:"enabled_operations"`
	DisabledReason     string   `json:"disabled_reason"`
}

func LoadRegistry(path string) (Registry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Registry{}, err
	}
	var registry Registry
	decoderErr := json.Unmarshal(data, &registry)
	if decoderErr != nil {
		return Registry{}, decoderErr
	}
	base := filepath.Dir(path)
	resolve := func(value string) string {
		if value == "" || filepath.IsAbs(value) {
			return value
		}
		absolute, _ := filepath.Abs(filepath.Join(base, value))
		return absolute
	}
	registry.Root, registry.StateDir = resolve(registry.Root), resolve(registry.StateDir)
	if registry.Python == "" {
		registry.Python = "python"
	}
	for index := range registry.Lines {
		line := &registry.Lines[index]
		line.HostsFile = resolve(line.HostsFile)
		line.SourceMachinesFile = resolve(line.SourceMachinesFile)
		line.LineProfileFile = resolve(line.LineProfileFile)
		line.KnownHostsFile = resolve(line.KnownHostsFile)
		line.SecurityDir = resolve(line.SecurityDir)
		line.ClientSecretFile = resolve(line.ClientSecretFile)
	}
	if err = registry.Validate(); err != nil {
		return Registry{}, err
	}
	return registry, nil
}

func (r Registry) Validate() error {
	if r.SchemaVersion != 1 || !safeID.MatchString(r.WorkerID) {
		return errors.New("invalid worker registry identity")
	}
	if r.Root == "" || r.StateDir == "" {
		return errors.New("worker root and state_dir are required")
	}
	if info, err := os.Stat(r.Root); err != nil || !info.IsDir() {
		return fmt.Errorf("worker root is unavailable: %s", r.Root)
	}
	seen := map[string]bool{}
	for _, line := range r.Lines {
		if !safeID.MatchString(line.LineID) || !safeID.MatchString(line.ResourceGroup) || seen[line.LineID] {
			return fmt.Errorf("invalid or duplicate worker line: %s", line.LineID)
		}
		seen[line.LineID] = true
		if line.SocksPort < 1 || line.SocksPort > 65535 || (line.PackageMbps != 5 && line.PackageMbps != 10 && line.PackageMbps != 15) {
			return fmt.Errorf("invalid package or SOCKS port for %s", line.LineID)
		}
		operationSeen := map[string]bool{}
		for _, kind := range line.EnabledOperations {
			if !allowedKinds[kind] || operationSeen[kind] {
				return fmt.Errorf("invalid operation %q for %s", kind, line.LineID)
			}
			operationSeen[kind] = true
		}
		for _, path := range []string{line.HostsFile, line.LineProfileFile, line.KnownHostsFile} {
			if len(line.EnabledOperations) > 0 {
				if info, err := os.Stat(path); err != nil || info.IsDir() {
					return fmt.Errorf("required worker path is unavailable: %s", path)
				}
			}
		}
		if operationSeen["line.open"] {
			if info, err := os.Stat(line.SourceMachinesFile); err != nil || info.IsDir() {
				return fmt.Errorf("source machine file is unavailable for %s", line.LineID)
			}
		}
	}
	return nil
}

func (r Registry) Line(lineID string) (LineSpec, bool) {
	for _, line := range r.Lines {
		if line.LineID == lineID {
			return line, true
		}
	}
	return LineSpec{}, false
}

func (line LineSpec) Allows(kind string) bool {
	for _, allowed := range line.EnabledOperations {
		if allowed == kind {
			return true
		}
	}
	return false
}

func (line LineSpec) SortedOperations() []string {
	result := append([]string(nil), line.EnabledOperations...)
	sort.Strings(result)
	return result
}
