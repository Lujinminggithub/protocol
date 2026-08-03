package worker

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

var privateFileRenameMu sync.Mutex

type dynamicDevice struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Host      string `json:"host"`
	SSHPort   int    `json:"ssh_port"`
	SSHUser   string `json:"ssh_user"`
	PrivateIP string `json:"private_ip"`
	SecretRef string `json:"secret_ref"`
}
type dynamicNode struct {
	DeviceID       string        `json:"device_id"`
	Role           string        `json:"role"`
	Ordinal        int           `json:"ordinal"`
	NextHopDevice  string        `json:"next_hop_device_id"`
	JumpCandidates []string      `json:"jump_candidates"`
	Device         dynamicDevice `json:"device"`
}
type dynamicPlan struct {
	LineID        string        `json:"line_id"`
	ResourceGroup string        `json:"resource_group"`
	InstanceID    string        `json:"instance_id"`
	BandwidthMbps int           `json:"bandwidth_mbps"`
	SocksPort     int           `json:"socks_port"`
	UDPPortMin    int           `json:"udp_port_min"`
	UDPPortMax    int           `json:"udp_port_max"`
	RelayPort     int           `json:"relay_port"`
	ExitPort      int           `json:"exit_port"`
	ExitBindIP    string        `json:"exit_bind_ip"`
	Whitelist     []string      `json:"whitelist"`
	BuildMode     string        `json:"build_mode"`
	ArtifactRef   string        `json:"artifact_ref"`
	SourceRef     string        `json:"source_ref"`
	SRSRef        string        `json:"srs_ref"`
	JumpPolicy    string        `json:"jump_policy"`
	Nodes         []dynamicNode `json:"nodes"`
}
type localSecret struct {
	Password    string `json:"password"`
	PasswordEnv string `json:"password_env"`
	URL         string `json:"url"`
	URLEnv      string `json:"url_env"`
}

func contains(value string, values []string) bool {
	for _, item := range values {
		if item == value {
			return true
		}
	}
	return false
}
func dynamicHostValid(value string) bool {
	if net.ParseIP(value) != nil {
		return true
	}
	if len(value) < 1 || len(value) > 253 {
		return false
	}
	for _, part := range strings.Split(value, ".") {
		if !safeID.MatchString(part) {
			return false
		}
	}
	return true
}

func (r *Runner) resolveSecret(ref string) (localSecret, error) {
	if strings.HasPrefix(ref, "env:") {
		name := strings.TrimPrefix(ref, "env:")
		if !safeID.MatchString(name) {
			return localSecret{}, errors.New("invalid environment secret reference")
		}
		if os.Getenv(name) == "" {
			return localSecret{}, fmt.Errorf("secret environment variable is unavailable: %s", name)
		}
		return localSecret{PasswordEnv: name}, nil
	}
	if strings.HasPrefix(ref, "worker-local:") {
		parts := strings.Split(ref, ":")
		if len(parts) != 3 || !safeID.MatchString(parts[1]) {
			return localSecret{}, errors.New("invalid worker-local secret reference")
		}
		role := parts[2]
		if role == "relay" {
			role = "middle"
		}
		if role != "entry" && role != "middle" && role != "exit" {
			return localSecret{}, errors.New("invalid worker-local secret role")
		}
		line, ok := r.registry.Line(parts[1])
		if !ok {
			return localSecret{}, errors.New("worker-local source line is unavailable")
		}
		source, err := loadJSON(line.SourceMachinesFile)
		if err != nil || len(source) == 0 {
			source, err = loadJSON(line.HostsFile)
		}
		if err != nil {
			return localSecret{}, err
		}
		host := roleObject(source, role)
		if host == nil {
			return localSecret{}, errors.New("worker-local source role is unavailable")
		}
		secret := localSecret{Password: text(host["password"]), PasswordEnv: text(host["password_env"])}
		if secret.Password == "" && (secret.PasswordEnv == "" || os.Getenv(secret.PasswordEnv) == "") {
			return localSecret{}, errors.New("resolved worker-local device secret is empty")
		}
		return secret, nil
	}
	if r.registry.Dynamic.SecretsFile == "" {
		return localSecret{}, errors.New("dynamic secrets file is not configured")
	}
	data, err := os.ReadFile(r.registry.Dynamic.SecretsFile)
	if err != nil {
		return localSecret{}, err
	}
	values := map[string]localSecret{}
	if err = json.Unmarshal(data, &values); err != nil {
		return localSecret{}, err
	}
	secret, ok := values[ref]
	if !ok {
		return localSecret{}, fmt.Errorf("secret reference is not available: %s", ref)
	}
	if secret.Password == "" && (secret.PasswordEnv == "" || os.Getenv(secret.PasswordEnv) == "") {
		return localSecret{}, errors.New("resolved device secret is empty")
	}
	return secret, nil
}

func (r *Runner) resolveWhitelistSource(ref string) (string, map[string]string, error) {
	if ref == "" {
		return "", nil, nil
	}
	if strings.HasPrefix(ref, "env:") {
		name := strings.TrimPrefix(ref, "env:")
		if !safeID.MatchString(name) || os.Getenv(name) == "" {
			return "", nil, errors.New("whitelist source environment variable is unavailable")
		}
		return name, nil, nil
	}
	if r.registry.Dynamic.SecretsFile == "" {
		return "", nil, errors.New("dynamic secrets file is not configured")
	}
	data, err := os.ReadFile(r.registry.Dynamic.SecretsFile)
	if err != nil {
		return "", nil, err
	}
	values := map[string]localSecret{}
	if err = json.Unmarshal(data, &values); err != nil {
		return "", nil, err
	}
	secret, ok := values[ref]
	if !ok {
		return "", nil, errors.New("whitelist source reference is unavailable")
	}
	if secret.URLEnv != "" && os.Getenv(secret.URLEnv) != "" {
		return secret.URLEnv, nil, nil
	}
	if secret.URL == "" {
		return "", nil, errors.New("whitelist source URL is empty")
	}
	name := "NB_WHITELIST_SOURCE_URL"
	return name, map[string]string{name: secret.URL}, nil
}

func (r *Runner) validateDynamicPlan(operation Operation, plan dynamicPlan) error {
	cfg := r.registry.Dynamic
	if !cfg.Enabled || !contains(operation.Kind, cfg.Operations) {
		return errors.New("dynamic operation is not enabled")
	}
	if plan.LineID != operation.LineID || !safeID.MatchString(plan.LineID) || !contains(plan.ResourceGroup, cfg.ResourceGroups) {
		return errors.New("dynamic line identity or resource group is not allowed")
	}
	if plan.InstanceID == "" || !safeID.MatchString(plan.InstanceID) || plan.BandwidthMbps < 1 || plan.BandwidthMbps > 1000 {
		return errors.New("invalid dynamic instance or bandwidth")
	}
	if plan.BuildMode != "auto" && plan.BuildMode != "binary" && plan.BuildMode != "source" {
		return errors.New("invalid dynamic build mode")
	}
	if plan.ArtifactRef != "" && filepath.ToSlash(plan.ArtifactRef) != "build/nb_node" {
		return errors.New("dynamic artifact must use the verified local build/nb_node")
	}
	if plan.SourceRef != "" && plan.SourceRef != "repo://current" {
		return errors.New("dynamic source must use the current repository")
	}
	if plan.SocksPort < cfg.SocksPortMin || plan.SocksPort > cfg.SocksPortMax || plan.RelayPort < cfg.RelayPortMin || plan.RelayPort+transportWorkerLanes-1 > cfg.RelayPortMax || plan.UDPPortMin < cfg.UDPPortMin || plan.UDPPortMax > cfg.UDPPortMax || plan.UDPPortMin > plan.UDPPortMax || plan.ExitPort < 1 || plan.ExitPort+transportWorkerLanes-1 > 65535 {
		return errors.New("dynamic plan exceeds assigned port limits")
	}
	if plan.ExitBindIP != "" {
		parsed := net.ParseIP(plan.ExitBindIP)
		if parsed == nil || parsed.To4() == nil {
			return errors.New("dynamic exit_bind_ip must be an IPv4 address")
		}
	}
	roles := map[string]int{}
	for _, node := range plan.Nodes {
		if node.Role != "entry" && node.Role != "relay" && node.Role != "exit" {
			return errors.New("invalid dynamic node role")
		}
		if !safeID.MatchString(node.Device.ID) || node.Device.ID != node.DeviceID || !dynamicHostValid(node.Device.Host) || node.Device.SSHPort < 1 || node.Device.SSHPort > 65535 || node.Device.SSHUser == "" {
			return errors.New("invalid dynamic device")
		}
		roles[node.Role]++
	}
	if roles["entry"] != 1 || roles["relay"] != 1 || roles["exit"] != 1 {
		return errors.New("current NB topology requires one entry, relay and exit")
	}
	return nil
}

func writePrivateJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return writePrivateFile(path, append(data, '\n'))
}

func writePrivateFile(path string, data []byte) (err error) {
	if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".new-*")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		if err != nil {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err = temporary.Chmod(0600); err != nil {
		return err
	}
	if _, err = temporary.Write(data); err != nil {
		return err
	}
	if err = temporary.Sync(); err != nil {
		return err
	}
	if err = temporary.Close(); err != nil {
		return err
	}
	privateFileRenameMu.Lock()
	defer privateFileRenameMu.Unlock()
	if err = os.Rename(temporaryPath, path); err != nil && runtime.GOOS == "windows" {
		if removeErr := os.Remove(path); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			return removeErr
		}
		err = os.Rename(temporaryPath, path)
	}
	return err
}

func (r *Runner) ensureDynamicKnownHosts(lineState string) (string, error) {
	target := filepath.Join(lineState, "security", "known_hosts")
	if _, err := os.Stat(target); err == nil {
		return target, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	if r.registry.Dynamic.KnownHostsFile == "" {
		return target, nil
	}
	contents, err := os.ReadFile(r.registry.Dynamic.KnownHostsFile)
	if err != nil {
		return "", err
	}
	if err = writePrivateFile(target, contents); err != nil {
		return "", err
	}
	return target, nil
}

func (r *Runner) dynamicLine(operation Operation, request requestValues, operationDir string) (LineSpec, error) {
	plan := request.Plan
	if err := r.validateDynamicPlan(operation, plan); err != nil {
		return LineSpec{}, err
	}
	lineState := filepath.Join(r.registry.StateDir, "lines", operation.LineID)
	whitelistSourceEnv, extraEnvironment, err := r.resolveWhitelistSource(plan.SRSRef)
	if err != nil {
		return LineSpec{}, err
	}
	if err = writePrivateJSON(filepath.Join(lineState, "runtime-plan.json"), plan); err != nil {
		return LineSpec{}, err
	}
	knownHosts, err := r.ensureDynamicKnownHosts(lineState)
	if err != nil {
		return LineSpec{}, fmt.Errorf("initialize dynamic known_hosts: %w", err)
	}
	sourcePath := filepath.Join(lineState, "source-machines.json")
	roles := map[string]any{}
	for _, node := range plan.Nodes {
		secret, err := r.resolveSecret(node.Device.SecretRef)
		if err != nil {
			return LineSpec{}, err
		}
		name := node.Role
		if name == "relay" {
			name = "middle"
		}
		host := map[string]any{"name": node.Device.Name, "host": node.Device.Host, "port": node.Device.SSHPort, "user": node.Device.SSHUser, "private_ip": node.Device.PrivateIP}
		if secret.Password != "" {
			envName := "NB_SSH_PASSWORD_" + strings.ToUpper(name)
			host["password"] = secret.Password
			host["password_env"] = envName
		} else {
			host["password_env"] = secret.PasswordEnv
		}
		if plan.JumpPolicy == "auto" {
			candidates := make([]string, 0, len(node.JumpCandidates))
			for _, candidate := range node.JumpCandidates {
				for _, candidateNode := range plan.Nodes {
					if candidateNode.DeviceID == candidate {
						role := candidateNode.Role
						if role == "relay" {
							role = "middle"
						}
						candidates = append(candidates, role)
					}
				}
			}
			host["jump_policy"] = "auto"
			host["jump_candidates"] = candidates
		}
		if name == "middle" && plan.JumpPolicy != "direct" {
			host["jump_via"] = "entry"
			host["jump_target_host"] = node.Device.PrivateIP
		}
		roles[name] = host
	}
	exit := roles["exit"].(map[string]any)
	if plan.ExitBindIP != "" {
		exit["outip"] = plan.ExitBindIP
	}
	source := map[string]any{"entry": roles["entry"], "middle": roles["middle"], "exit": exit, "build_host": "entry", "release_retention": 5, "workers": map[string]int{"entry": transportWorkerLanes, "middle": transportWorkerLanes, "exit": transportWorkerLanes}, "exits": []map[string]any{{"name": exit["name"], "host": exit["host"], "port": plan.ExitPort, "weight": 1, "capacity": 0, "fixed_exit": exit["name"]}}, "transport": map[string]any{"entry": map[string]any{"cc": "cubic", "cwin_max_bytes": 524288, "mtu_max": 1452, "udp_gso": false, "udp_port_min": plan.UDPPortMin, "udp_port_max": plan.UDPPortMax, "reorder_gap": 128, "reorder_delay_us": 450000}, "middle": map[string]any{"cc": "bbr", "bbr_options": "Q0.0001:F0.25:", "mtu_max": 1452, "udp_gso": false, "reorder_gap": 128, "reorder_delay_us": 462000}, "exit": map[string]any{"cc": "bbr", "bbr_options": "Q0.0001:", "mtu_max": 1452, "udp_gso": false}}}
	if err := writePrivateJSON(sourcePath, source); err != nil {
		return LineSpec{}, err
	}
	whitelistPath := filepath.Join(lineState, "whitelist.conf")
	lines := []string{"# generated by nb-web-worker", "domain_exact odr.itunes.apple.com"}
	for _, value := range plan.Whitelist {
		value = strings.TrimSpace(value)
		if strings.HasPrefix(value, "domain ") || strings.HasPrefix(value, "domain_suffix ") || strings.HasPrefix(value, "domain_exact ") || strings.HasPrefix(value, "domain_keyword ") || strings.HasPrefix(value, "ip ") || strings.HasPrefix(value, "port ") {
			lines = append(lines, value)
		} else {
			return LineSpec{}, errors.New("invalid whitelist rule")
		}
	}
	if err := os.MkdirAll(lineState, 0700); err != nil {
		return LineSpec{}, err
	}
	if plan.SRSRef == "" {
		contents := []byte(strings.Join(lines, "\n") + "\n")
		if len(plan.Whitelist) == 0 {
			contents, err = os.ReadFile(filepath.Join(r.registry.Root, "tools", "whitelist.local.conf"))
			if err != nil {
				return LineSpec{}, fmt.Errorf("default whitelist is unavailable: %w", err)
			}
		}
		if err := os.WriteFile(whitelistPath, contents, 0600); err != nil {
			return LineSpec{}, err
		}
	} else if _, statErr := os.Stat(whitelistPath); errors.Is(statErr, os.ErrNotExist) {
		if err := os.WriteFile(whitelistPath, []byte(strings.Join(lines, "\n")+"\n"), 0600); err != nil {
			return LineSpec{}, err
		}
	}
	profile := filepath.Join(lineState, "provision", operation.LineID, "stable-profile.json")
	hosts := filepath.Join(lineState, "provision", operation.LineID, "deployment-hosts.json")
	return LineSpec{LineID: operation.LineID, ResourceGroup: plan.ResourceGroup, InstanceID: plan.InstanceID, HostsFile: hosts, SourceMachinesFile: sourcePath, LineProfileFile: profile, KnownHostsFile: knownHosts, SecurityDir: filepath.Join(lineState, "security"), ClientSecretFile: filepath.Join(lineState, "bootstrap-client-secret.json"), PackageMbps: float64(plan.BandwidthMbps), SocksPort: plan.SocksPort, UDPPortMin: plan.UDPPortMin, UDPPortMax: plan.UDPPortMax, MiddlePort: plan.RelayPort, ExitPort: plan.ExitPort, ExitBindIP: plan.ExitBindIP, EnabledOperations: append([]string(nil), r.registry.Dynamic.Operations...), StateDir: lineState, WhitelistFile: whitelistPath, WhitelistSourceEnv: whitelistSourceEnv, SingBox: r.registry.Dynamic.SingBox, ExtraEnvironment: extraEnvironment, BuildMode: plan.BuildMode}, nil
}

func (r *Runner) resolveSnapshotLine(plan dynamicPlan) (LineSpec, error) {
	line, err := r.dynamicLine(Operation{ID: "snapshot", LineID: plan.LineID, Kind: "line.validate"}, requestValues{Plan: plan}, filepath.Join(r.registry.StateDir, "snapshot"))
	if err != nil {
		return LineSpec{}, err
	}
	if _, statErr := os.Stat(line.HostsFile); errors.Is(statErr, os.ErrNotExist) {
		line.HostsFile = line.SourceMachinesFile
	}
	return line, nil
}
