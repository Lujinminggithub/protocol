package worker

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
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
	SchemaVersion int           `json:"schema_version"`
	WorkerID      string        `json:"worker_id"`
	Root          string        `json:"root"`
	Python        string        `json:"python"`
	StateDir      string        `json:"state_dir"`
	Lines         []LineSpec    `json:"lines"`
	Dynamic       DynamicConfig `json:"dynamic"`
}

type DynamicConfig struct {
	Enabled        bool     `json:"enabled"`
	ResourceGroups []string `json:"resource_groups"`
	Operations     []string `json:"operations"`
	KnownHostsFile string   `json:"known_hosts_file"`
	SecretsFile    string   `json:"secrets_file"`
	SingBox        string   `json:"sing_box"`
	SocksPortMin   int      `json:"socks_port_min"`
	SocksPortMax   int      `json:"socks_port_max"`
	RelayPortMin   int      `json:"relay_port_min"`
	RelayPortMax   int      `json:"relay_port_max"`
	UDPPortMin     int      `json:"udp_port_min"`
	UDPPortMax     int      `json:"udp_port_max"`
}

type LineSpec struct {
	LineID             string            `json:"line_id"`
	ResourceGroup      string            `json:"resource_group"`
	InstanceID         string            `json:"instance_id"`
	HostsFile          string            `json:"hosts_file"`
	SourceMachinesFile string            `json:"source_machines_file"`
	LineProfileFile    string            `json:"line_profile_file"`
	KnownHostsFile     string            `json:"known_hosts_file"`
	SecurityDir        string            `json:"security_dir"`
	ClientSecretFile   string            `json:"client_secret_file"`
	PackageMbps        float64           `json:"package_mbps"`
	SocksPort          int               `json:"socks_port"`
	UDPPortMin         int               `json:"udp_port_min"`
	UDPPortMax         int               `json:"udp_port_max"`
	MiddlePort         int               `json:"middle_port"`
	ExitPort           int               `json:"exit_port"`
	EnabledOperations  []string          `json:"enabled_operations"`
	DisabledReason     string            `json:"disabled_reason"`
	StateDir           string            `json:"-"`
	WhitelistFile      string            `json:"whitelist_file,omitempty"`
	WhitelistSourceEnv string            `json:"whitelist_source_env,omitempty"`
	SingBox            string            `json:"sing_box,omitempty"`
	ExtraEnvironment   map[string]string `json:"-"`
	BuildMode          string            `json:"build_mode,omitempty"`
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
	registry.Dynamic.KnownHostsFile = resolve(registry.Dynamic.KnownHostsFile)
	registry.Dynamic.SecretsFile = resolve(registry.Dynamic.SecretsFile)
	if registry.Dynamic.SingBox == "" {
		registry.Dynamic.SingBox = "sing-box"
	} else if strings.ContainsAny(registry.Dynamic.SingBox, `/\\`) {
		registry.Dynamic.SingBox = resolve(registry.Dynamic.SingBox)
	}
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
		line.WhitelistFile = resolve(line.WhitelistFile)
		if line.WhitelistSourceEnv != "" {
			line.StateDir = filepath.Join(registry.StateDir, "lines", line.LineID)
			if line.SingBox == "" {
				line.SingBox = registry.Dynamic.SingBox
			} else if strings.ContainsAny(line.SingBox, `/\\`) {
				line.SingBox = resolve(line.SingBox)
			}
		}
		if line.MiddlePort == 0 {
			line.MiddlePort = 4443
		}
		if line.UDPPortMin == 0 && line.UDPPortMax == 0 {
			line.UDPPortMin, line.UDPPortMax = 20000, 21023
		}
		if line.ExitPort == 0 {
			line.ExitPort = 4443
		}
		if line.BuildMode == "" {
			line.BuildMode = "source"
		}
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
	if r.Dynamic.Enabled {
		if len(r.Dynamic.ResourceGroups) == 0 || len(r.Dynamic.Operations) == 0 {
			return errors.New("dynamic worker requires resource groups and operations")
		}
		for _, group := range r.Dynamic.ResourceGroups {
			if !safeID.MatchString(group) {
				return fmt.Errorf("invalid dynamic resource group: %s", group)
			}
		}
		seenOperations := map[string]bool{}
		for _, kind := range r.Dynamic.Operations {
			if !allowedKinds[kind] || seenOperations[kind] {
				return fmt.Errorf("invalid dynamic operation: %s", kind)
			}
			seenOperations[kind] = true
		}
		for _, limits := range [][2]int{{r.Dynamic.SocksPortMin, r.Dynamic.SocksPortMax}, {r.Dynamic.RelayPortMin, r.Dynamic.RelayPortMax}, {r.Dynamic.UDPPortMin, r.Dynamic.UDPPortMax}} {
			if limits[0] < 1 || limits[1] > 65535 || limits[0] > limits[1] {
				return errors.New("invalid dynamic port limits")
			}
		}
		if r.Dynamic.KnownHostsFile != "" {
			if info, err := os.Stat(r.Dynamic.KnownHostsFile); err != nil || info.IsDir() {
				return errors.New("dynamic known_hosts is unavailable")
			}
		}
		if r.Dynamic.SecretsFile != "" {
			if info, err := os.Stat(r.Dynamic.SecretsFile); err != nil || info.IsDir() {
				return errors.New("dynamic secrets file is unavailable")
			}
		}
	}
	seen := map[string]bool{}
	entryPorts := map[string]string{}
	middlePorts := map[string]string{}
	udpRanges := map[string][]LineSpec{}
	for _, line := range r.Lines {
		if !safeID.MatchString(line.LineID) || !safeID.MatchString(line.ResourceGroup) || seen[line.LineID] {
			return fmt.Errorf("invalid or duplicate worker line: %s", line.LineID)
		}
		seen[line.LineID] = true
		if line.InstanceID != "" && !safeID.MatchString(line.InstanceID) {
			return fmt.Errorf("invalid instance_id for %s", line.LineID)
		}
		if line.BuildMode != "" && line.BuildMode != "source" && line.BuildMode != "auto" && line.BuildMode != "binary" {
			return fmt.Errorf("invalid build mode for %s", line.LineID)
		}
		middlePort, exitPort := line.MiddlePort, line.ExitPort
		udpPortMin, udpPortMax := line.UDPPortMin, line.UDPPortMax
		if middlePort == 0 {
			middlePort = 4443
		}
		if exitPort == 0 {
			exitPort = 4443
		}
		if udpPortMin == 0 && udpPortMax == 0 {
			udpPortMin, udpPortMax = 20000, 21023
		}
		if line.SocksPort < 1 || line.SocksPort > 65535 || line.PackageMbps < 1 || line.PackageMbps > 1000 {
			return fmt.Errorf("invalid package or SOCKS port for %s", line.LineID)
		}
		if middlePort < 1 || middlePort > 65535 || exitPort < 1 || exitPort > 65535 {
			return fmt.Errorf("invalid transport port for %s", line.LineID)
		}
		if udpPortMin < 1024 || udpPortMax > 65535 || udpPortMin > udpPortMax {
			return fmt.Errorf("invalid UDP relay range for %s", line.LineID)
		}
		entryKey := fmt.Sprintf("%s:%d", line.ResourceGroup, line.SocksPort)
		middleKey := fmt.Sprintf("%s:%d", line.ResourceGroup, middlePort)
		if other := entryPorts[entryKey]; other != "" {
			return fmt.Errorf("SOCKS port collision between %s and %s", other, line.LineID)
		}
		if other := middlePorts[middleKey]; other != "" {
			return fmt.Errorf("middle port collision between %s and %s", other, line.LineID)
		}
		entryPorts[entryKey], middlePorts[middleKey] = line.LineID, line.LineID
		for _, other := range udpRanges[line.ResourceGroup] {
			if udpPortMin <= other.UDPPortMax && other.UDPPortMin <= udpPortMax {
				return fmt.Errorf("UDP relay range collision between %s and %s", other.LineID, line.LineID)
			}
		}
		line.UDPPortMin, line.UDPPortMax = udpPortMin, udpPortMax
		udpRanges[line.ResourceGroup] = append(udpRanges[line.ResourceGroup], line)
		operationSeen := map[string]bool{}
		if line.WhitelistSourceEnv != "" && (!safeID.MatchString(line.WhitelistSourceEnv) || line.WhitelistFile == "") {
			return fmt.Errorf("fixed whitelist refresh is invalid for %s", line.LineID)
		}
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
