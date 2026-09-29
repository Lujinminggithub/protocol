package worker

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"
)

var privateFileRenameMu sync.Mutex

type dynamicDevice struct {
	ID                    string `json:"id"`
	Name                  string `json:"name"`
	Host                  string `json:"host"`
	SSHPort               int    `json:"ssh_port"`
	SSHUser               string `json:"ssh_user"`
	SSHHostKey            string `json:"ssh_host_key"`
	SSHHostKeyType        string `json:"ssh_host_key_type"`
	SSHHostKeySHA256      string `json:"ssh_host_key_sha256"`
	SSHHostKeyStatus      string `json:"ssh_host_key_status"`
	SSHHostKeyConfirmedAt string `json:"ssh_host_key_confirmed_at"`
	PrivateIP             string `json:"private_ip"`
	SecretRef             string `json:"secret_ref"`
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
	LineID         string        `json:"line_id"`
	ResourceGroup  string        `json:"resource_group"`
	InstanceID     string        `json:"instance_id"`
	BandwidthMbps  int           `json:"bandwidth_mbps"`
	UpstreamMbps   int           `json:"upstream_mbps"`
	DownstreamMbps int           `json:"downstream_mbps"`
	SocksPort      int           `json:"socks_port"`
	SocksPortAuto  bool          `json:"socks_port_auto"`
	UDPPortMin     int           `json:"udp_port_min"`
	UDPPortMax     int           `json:"udp_port_max"`
	UDPPortsAuto   bool          `json:"udp_ports_auto"`
	RelayPort      int           `json:"relay_port"`
	RelayPortAuto  bool          `json:"relay_port_auto"`
	ExitPort       int           `json:"exit_port"`
	ExitPortAuto   bool          `json:"exit_port_auto"`
	ExitBindIP     string        `json:"exit_bind_ip"`
	DNSServers     []string      `json:"dns_servers"`
	Whitelist      []string      `json:"whitelist"`
	BuildMode      string        `json:"build_mode"`
	ArtifactRef    string        `json:"artifact_ref"`
	SourceRef      string        `json:"source_ref"`
	SRSRef         string        `json:"srs_ref"`
	JumpPolicy     string        `json:"jump_policy"`
	Nodes          []dynamicNode `json:"nodes"`
}

var requiredPublicDNSWhitelistRules = []string{
	"ip 1.1.1.1/32", "ip 8.8.8.8/32", "ip 9.9.9.9/32", "ip 114.114.114.114/32",
}

func withRequiredPublicDNSWhitelistRules(rules []string) []string {
	result := make([]string, 0, len(rules)+len(requiredPublicDNSWhitelistRules)+1)
	seen, restricted := map[string]bool{}, false
	appendRule := func(rule string) {
		if !seen[rule] {
			seen[rule] = true
			result = append(result, rule)
		}
	}
	for _, rule := range rules {
		appendRule(rule)
		restricted = restricted || strings.HasPrefix(rule, "port ")
	}
	for _, rule := range requiredPublicDNSWhitelistRules {
		appendRule(rule)
	}
	if restricted {
		appendRule("port 53")
	}
	return result
}

func normalizeDynamicDNSServers(servers []string) ([]string, error) {
	if len(servers) == 0 {
		servers = []string{"1.1.1.1", "8.8.8.8"}
	}
	if len(servers) > 3 {
		return nil, errors.New("线路 DNS 最多允许 3 个 IPv4 地址")
	}
	result, seen := make([]string, 0, len(servers)), map[string]bool{}
	for _, value := range servers {
		value = strings.TrimSpace(value)
		parsed := net.ParseIP(value)
		if parsed == nil || parsed.To4() == nil {
			return nil, errors.New("线路 DNS 必须是 IPv4 地址")
		}
		value = parsed.To4().String()
		if !seen[value] {
			seen[value] = true
			result = append(result, value)
		}
	}
	return result, nil
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

var (
	errDynamicSSHAuthentication = errors.New("SSH authentication failed")
	errDynamicSSHHostKey        = errors.New("SSH host key changed")
)

func resolvedPassword(secret localSecret) string {
	if secret.Password != "" {
		return secret.Password
	}
	return os.Getenv(secret.PasswordEnv)
}

func dialDynamicSSH(ctx context.Context, node dynamicNode, password string, jump *ssh.Client) (*ssh.Client, error) {
	raw, err := base64.StdEncoding.DecodeString(node.Device.SSHHostKey)
	if err != nil {
		return nil, errDynamicSSHHostKey
	}
	expected, err := ssh.ParsePublicKey(raw)
	if err != nil {
		return nil, errDynamicSSHHostKey
	}
	address := net.JoinHostPort(node.Device.Host, fmt.Sprintf("%d", node.Device.SSHPort))
	var connection net.Conn
	if jump == nil {
		dialer := net.Dialer{Timeout: 8 * time.Second}
		connection, err = dialer.DialContext(ctx, "tcp", address)
	} else {
		connection, err = jump.Dial("tcp", address)
	}
	if err != nil {
		return nil, err
	}
	deadline := time.Now().Add(10 * time.Second)
	if contextDeadline, ok := ctx.Deadline(); ok && contextDeadline.Before(deadline) {
		deadline = contextDeadline
	}
	_ = connection.SetDeadline(deadline)
	configuration := &ssh.ClientConfig{
		User:              node.Device.SSHUser,
		Auth:              []ssh.AuthMethod{ssh.Password(password)},
		HostKeyAlgorithms: []string{expected.Type()},
		HostKeyCallback: func(_ string, _ net.Addr, actual ssh.PublicKey) error {
			if actual.Type() != expected.Type() || !bytes.Equal(actual.Marshal(), expected.Marshal()) {
				return errDynamicSSHHostKey
			}
			return nil
		},
		Timeout: 10 * time.Second,
	}
	clientConnection, channels, requests, err := ssh.NewClientConn(connection, address, configuration)
	if err != nil {
		_ = connection.Close()
		if errors.Is(err, errDynamicSSHHostKey) {
			return nil, errDynamicSSHHostKey
		}
		if strings.Contains(err.Error(), "unable to authenticate") {
			return nil, errDynamicSSHAuthentication
		}
		return nil, err
	}
	_ = connection.SetDeadline(time.Time{})
	return ssh.NewClient(clientConnection, channels, requests), nil
}

func (r *Runner) verifyDynamicSSHAccess(ctx context.Context, plan dynamicPlan) error {
	clients := make(map[string]*ssh.Client, len(plan.Nodes))
	defer func() {
		for _, client := range clients {
			_ = client.Close()
		}
	}()
	for _, node := range plan.Nodes {
		secret, err := r.resolveSecret(node.Device.SecretRef)
		if err != nil {
			return err
		}
		password := resolvedPassword(secret)
		client, directErr := dialDynamicSSH(ctx, node, password, nil)
		if errors.Is(directErr, errDynamicSSHAuthentication) {
			return fmt.Errorf("设备 %s SSH 密码认证失败，请重新录入正确密码", node.Device.ID)
		}
		if errors.Is(directErr, errDynamicSSHHostKey) {
			return fmt.Errorf("设备 %s SSH 主机密钥已变化，请重新扫描并确认", node.Device.ID)
		}
		if directErr != nil {
			for _, candidate := range node.JumpCandidates {
				jump := clients[candidate]
				if jump == nil {
					continue
				}
				client, err = dialDynamicSSH(ctx, node, password, jump)
				if err == nil {
					break
				}
				if errors.Is(err, errDynamicSSHAuthentication) {
					return fmt.Errorf("设备 %s SSH 密码认证失败，请重新录入正确密码", node.Device.ID)
				}
				if errors.Is(err, errDynamicSSHHostKey) {
					return fmt.Errorf("设备 %s SSH 主机密钥已变化，请重新扫描并确认", node.Device.ID)
				}
			}
		}
		if client == nil {
			return fmt.Errorf("设备 %s SSH 管理端口不可达，请检查网络或跳板配置", node.Device.ID)
		}
		clients[node.Device.ID] = client
	}
	return nil
}

func (r *Runner) resolveSecret(ref string) (localSecret, error) {
	if strings.HasPrefix(ref, "env:") {
		name := strings.TrimPrefix(ref, "env:")
		if !safeID.MatchString(name) {
			return localSecret{}, errors.New("环境变量凭据引用无效")
		}
		if os.Getenv(name) == "" {
			return localSecret{}, fmt.Errorf("凭据环境变量不可用：%s", name)
		}
		return localSecret{PasswordEnv: name}, nil
	}
	if strings.HasPrefix(ref, "worker-local:") {
		parts := strings.Split(ref, ":")
		if len(parts) != 3 || !safeID.MatchString(parts[1]) {
			return localSecret{}, errors.New("worker 本地凭据引用无效")
		}
		role := parts[2]
		if role == "relay" {
			role = "middle"
		}
		if role != "entry" && role != "middle" && role != "exit" {
			return localSecret{}, errors.New("worker 本地凭据角色无效")
		}
		line, ok := r.registry.Line(parts[1])
		if !ok {
			return localSecret{}, errors.New("worker 本地来源线路不可用")
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
			return localSecret{}, errors.New("worker 本地来源角色不可用")
		}
		secret := localSecret{Password: text(host["password"]), PasswordEnv: text(host["password_env"])}
		if secret.Password == "" && (secret.PasswordEnv == "" || os.Getenv(secret.PasswordEnv) == "") {
			return localSecret{}, errors.New("解析后的 worker 本地设备凭据为空")
		}
		return secret, nil
	}
	if r.registry.Dynamic.SecretsFile == "" {
		return localSecret{}, errors.New("未配置动态凭据文件")
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
		return localSecret{}, fmt.Errorf("凭据引用不可用：%s", ref)
	}
	if secret.Password == "" && (secret.PasswordEnv == "" || os.Getenv(secret.PasswordEnv) == "") {
		return localSecret{}, errors.New("解析后的设备凭据为空")
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
			return "", nil, errors.New("白名单来源环境变量不可用")
		}
		return name, nil, nil
	}
	if strings.Contains(ref, "://") {
		parsed, err := url.ParseRequestURI(ref)
		if err != nil || parsed.Scheme != "https" || parsed.Host == "" || parsed.User != nil || parsed.Fragment != "" {
			return "", nil, errors.New("白名单来源地址必须使用 HTTPS")
		}
		const name = "NB_WHITELIST_SOURCE_URL"
		return name, map[string]string{name: ref}, nil
	}
	if r.registry.Dynamic.SecretsFile == "" {
		return "", nil, errors.New("未配置动态凭据文件")
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
		return "", nil, errors.New("白名单来源引用不可用")
	}
	if secret.URLEnv != "" && os.Getenv(secret.URLEnv) != "" {
		return secret.URLEnv, nil, nil
	}
	if secret.URL == "" {
		return "", nil, errors.New("白名单来源地址为空")
	}
	const name = "NB_WHITELIST_SOURCE_URL"
	return name, map[string]string{name: secret.URL}, nil
}

func (r *Runner) validateDynamicPlan(operation Operation, plan dynamicPlan) error {
	cfg := r.registry.Dynamic
	if !cfg.Enabled || !contains(operation.Kind, cfg.Operations) {
		return errors.New("当前动态操作未启用")
	}
	if plan.LineID != operation.LineID || !safeID.MatchString(plan.LineID) || !contains(plan.ResourceGroup, cfg.ResourceGroups) {
		return errors.New("线路标识或资源组不在 worker 授权范围内")
	}
	if plan.InstanceID == "" || !safeID.MatchString(plan.InstanceID) || plan.BandwidthMbps < 1 || plan.BandwidthMbps > 1000 ||
		plan.UpstreamMbps < 1 || plan.UpstreamMbps > 1000 || plan.DownstreamMbps < 1 || plan.DownstreamMbps > 1000 {
		return errors.New("线路实例或上下行平均速率无效")
	}
	if plan.BuildMode != "auto" && plan.BuildMode != "binary" && plan.BuildMode != "source" {
		return errors.New("线路构建策略无效")
	}
	if plan.ArtifactRef != "" && filepath.ToSlash(plan.ArtifactRef) != "build/nb_node" {
		return errors.New("线路二进制必须使用已校验的本地 build/nb_node")
	}
	if plan.SourceRef != "" && plan.SourceRef != "repo://current" {
		return errors.New("线路源码必须使用当前仓库")
	}
	if plan.SocksPort < cfg.SocksPortMin || plan.SocksPort > cfg.SocksPortMax || plan.RelayPort < cfg.RelayPortMin || plan.RelayPort+transportWorkerLanes-1 > cfg.RelayPortMax || plan.UDPPortMin < cfg.UDPPortMin || plan.UDPPortMax > cfg.UDPPortMax || plan.UDPPortMin > plan.UDPPortMax || plan.ExitPort < 1 || plan.ExitPort+transportWorkerLanes-1 > 65535 {
		return fmt.Errorf("线路端口超出 worker 授权范围：入口 %d-%d，Relay %d-%d，UDP %d-%d",
			cfg.SocksPortMin, cfg.SocksPortMax, cfg.RelayPortMin, cfg.RelayPortMax, cfg.UDPPortMin, cfg.UDPPortMax)
	}
	if plan.ExitBindIP != "" {
		parsed := net.ParseIP(plan.ExitBindIP)
		if parsed == nil || parsed.To4() == nil {
			return errors.New("线路出口 IP 必须是 IPv4 地址")
		}
	}
	roles := map[string]int{}
	for _, node := range plan.Nodes {
		if node.Role != "entry" && node.Role != "relay" && node.Role != "exit" {
			return errors.New("线路节点角色无效")
		}
		if !safeID.MatchString(node.Device.ID) || node.Device.ID != node.DeviceID || !dynamicHostValid(node.Device.Host) || node.Device.SSHPort < 1 || node.Device.SSHPort > 65535 || node.Device.SSHUser == "" {
			return errors.New("线路设备信息无效")
		}
		if node.Device.SSHHostKeyStatus != "trusted" || node.Device.SSHHostKey == "" ||
			node.Device.SSHHostKeyType == "" || node.Device.SSHHostKeySHA256 == "" {
			return fmt.Errorf("设备 %s 尚未完成 SSH 主机密钥登记", node.Device.ID)
		}
		rawKey, decodeErr := base64.StdEncoding.DecodeString(node.Device.SSHHostKey)
		key, parseErr := ssh.ParsePublicKey(rawKey)
		if decodeErr != nil || parseErr != nil || key.Type() != node.Device.SSHHostKeyType ||
			ssh.FingerprintSHA256(key) != node.Device.SSHHostKeySHA256 {
			return fmt.Errorf("设备 %s 的 SSH 主机密钥登记无效", node.Device.ID)
		}
		roles[node.Role]++
	}
	if roles["entry"] != 1 || roles["relay"] != 1 || roles["exit"] != 1 {
		return errors.New("当前 NB 拓扑必须且只能包含一个 Entry、Relay 和 Exit")
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

func (r *Runner) ensureDynamicKnownHosts(lineState string, nodes []dynamicNode) (string, error) {
	target := filepath.Join(lineState, "security", "known_hosts")
	var contents strings.Builder
	for _, node := range nodes {
		marker := node.Device.Host
		if node.Device.SSHPort != 22 {
			marker = fmt.Sprintf("[%s]:%d", node.Device.Host, node.Device.SSHPort)
		}
		_, _ = fmt.Fprintf(&contents, "%s %s %s\n", marker, node.Device.SSHHostKeyType, node.Device.SSHHostKey)
	}
	if err := writePrivateFile(target, []byte(contents.String())); err != nil {
		return "", err
	}
	return target, nil
}

func (r *Runner) dynamicLine(operation Operation, request requestValues, operationDir string) (LineSpec, error) {
	plan := request.Plan
	if plan.UpstreamMbps <= 0 {
		plan.UpstreamMbps = plan.BandwidthMbps
	}
	if plan.DownstreamMbps <= 0 {
		plan.DownstreamMbps = plan.BandwidthMbps
	}
	normalizedDNS, err := normalizeDynamicDNSServers(plan.DNSServers)
	if err != nil {
		return LineSpec{}, err
	}
	plan.DNSServers = normalizedDNS
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
	knownHosts, err := r.ensureDynamicKnownHosts(lineState, plan.Nodes)
	if err != nil {
		return LineSpec{}, fmt.Errorf("初始化动态 known_hosts 失败：%w", err)
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
		// Protocol identities must remain stable and safe even when operators localize display names.
		host := map[string]any{"name": node.DeviceID, "host": node.Device.Host, "port": node.Device.SSHPort, "user": node.Device.SSHUser, "private_ip": node.Device.PrivateIP}
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
	source := map[string]any{"entry": roles["entry"], "middle": roles["middle"], "exit": exit, "build_host": "entry", "release_retention": 5, "workers": map[string]int{"entry": transportWorkerLanes, "middle": transportWorkerLanes, "exit": transportWorkerLanes}, "exits": []map[string]any{{"name": exit["name"], "host": exit["host"], "port": plan.ExitPort, "weight": 1, "capacity": 0, "fixed_exit": exit["name"]}}, "transport": map[string]any{"entry": map[string]any{"cc": "cubic", "cwin_max_bytes": 524288, "mtu_max": 1452, "udp_gso": false, "udp_port_min": plan.UDPPortMin, "udp_port_max": plan.UDPPortMax, "reorder_gap": 128, "reorder_delay_us": 450000}, "middle": map[string]any{"cc": "bbr", "bbr_options": "Q0.0001:F0.25:", "mtu_max": 1452, "udp_gso": false, "reorder_gap": 128, "reorder_delay_us": 462000}, "exit": map[string]any{"cc": "bbr", "bbr_options": "Q0.0001:", "mtu_max": 1452, "udp_gso": false, "dns_servers": plan.DNSServers}}}
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
			return LineSpec{}, errors.New("白名单规则无效")
		}
	}
	lines = withRequiredPublicDNSWhitelistRules(lines)
	if err := os.MkdirAll(lineState, 0700); err != nil {
		return LineSpec{}, err
	}
	if plan.SRSRef == "" {
		contents := []byte(strings.Join(lines, "\n") + "\n")
		if len(plan.Whitelist) == 0 {
			contents, err = os.ReadFile(filepath.Join(r.registry.Root, "tools", "whitelist.local.conf"))
			if err != nil {
				return LineSpec{}, fmt.Errorf("默认白名单不可用：%w", err)
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
	return LineSpec{LineID: operation.LineID, ResourceGroup: plan.ResourceGroup, InstanceID: plan.InstanceID, HostsFile: hosts, SourceMachinesFile: sourcePath, LineProfileFile: profile, KnownHostsFile: knownHosts, SecurityDir: filepath.Join(lineState, "security"), ClientSecretFile: filepath.Join(lineState, "bootstrap-client-secret.json"), PackageMbps: float64(plan.BandwidthMbps), UpstreamMbps: float64(plan.UpstreamMbps), DownstreamMbps: float64(plan.DownstreamMbps), SocksPort: plan.SocksPort, UDPPortMin: plan.UDPPortMin, UDPPortMax: plan.UDPPortMax, MiddlePort: plan.RelayPort, ExitPort: plan.ExitPort, ExitBindIP: plan.ExitBindIP, EnabledOperations: append([]string(nil), r.registry.Dynamic.Operations...), StateDir: lineState, WhitelistFile: whitelistPath, WhitelistSourceEnv: whitelistSourceEnv, SingBox: r.registry.Dynamic.SingBox, ExtraEnvironment: extraEnvironment, BuildMode: plan.BuildMode}, nil
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
