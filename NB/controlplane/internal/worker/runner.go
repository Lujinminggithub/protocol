package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"nb-controlplane/internal/transportprofile"
)

type Operation struct {
	ID      string          `json:"id"`
	LineID  string          `json:"line_id"`
	Kind    string          `json:"kind"`
	Request json.RawMessage `json:"request"`
}

type Result struct {
	Deployment          string                    `json:"deployment,omitempty"`
	Profile             string                    `json:"profile,omitempty"`
	ClientURL           string                    `json:"client_url,omitempty"`
	LogFile             string                    `json:"log_file"`
	Message             string                    `json:"message"`
	Evidence            json.RawMessage           `json:"evidence,omitempty"`
	TransportGeneration uint64                    `json:"transport_generation,omitempty"`
	TransportProfile    *transportprofile.Profile `json:"transport_profile,omitempty"`
	TransportRollout    *transportRolloutResult   `json:"transport_rollout,omitempty"`
}

type transportRolloutRoleResult struct {
	Prepared    bool   `json:"prepared"`
	Committed   bool   `json:"committed"`
	Readback    bool   `json:"readback"`
	Fingerprint string `json:"fingerprint,omitempty"`
}

type transportRolloutResult struct {
	Status       string                                `json:"status"`
	Generation   uint64                                `json:"generation"`
	PrepareOrder []string                              `json:"prepare_order"`
	CommitOrder  []string                              `json:"commit_order"`
	Roles        map[string]transportRolloutRoleResult `json:"roles"`
}

type requestValues struct {
	Deployment       string                   `json:"deployment"`
	Note             string                   `json:"note"`
	Plan             dynamicPlan              `json:"plan"`
	TransportProfile transportprofile.Profile `json:"transport_profile"`
}

type commandStep struct {
	Name  string
	Args  []string
	Stage string
}

type Runner struct{ registry Registry }

func NewRunner(registry Registry) *Runner { return &Runner{registry: registry} }

var sensitiveErrorValue = regexp.MustCompile(`(?i)(password|passwd|token|secret|api[_-]?key)(\s*[:=]\s*)("[^"]*"|'[^']*'|[^\s,;]+)`)

func commandFailureSummary(err error, logPath string) string {
	summary := err.Error()
	if data, readErr := os.ReadFile(logPath); readErr == nil {
		lines := strings.Split(string(data), "\n")
		for _, marker := range []string{"拒绝开通:", "发布清单校验失败:", "ConnectionAbortedError:", "ConnectionResetError:", "TimeoutError:", "RuntimeError:", "ValueError:", "SystemExit:"} {
			for index := len(lines) - 1; index >= 0; index-- {
				if strings.Contains(lines[index], marker) {
					summary = strings.TrimSpace(lines[index])
					if marker == "拒绝开通:" {
						summary = strings.TrimSpace(strings.SplitN(summary, marker, 2)[1])
					}
					break
				}
			}
			if summary != err.Error() {
				break
			}
		}
	}
	if strings.Contains(summary, "PRIVATE KEY") {
		summary = "sensitive command failure details were redacted"
	} else {
		summary = sensitiveErrorValue.ReplaceAllString(summary, "$1$2[REDACTED]")
	}
	runes := []rune(summary)
	if len(runes) > 240 {
		summary = string(runes[:240]) + "..."
	}
	return summary
}

func roleObject(source map[string]any, role string) map[string]any {
	if value, ok := source[role].(map[string]any); ok {
		return value
	}
	keys := map[string]string{"entry": "edges", "middle": "relays", "exit": "terminals"}
	if values, ok := source[keys[role]].([]any); ok && len(values) == 1 {
		value, _ := values[0].(map[string]any)
		return value
	}
	if values, ok := source["machines"].([]any); ok {
		for _, candidate := range values {
			value, _ := candidate.(map[string]any)
			if text(value["role"]) == role {
				return value
			}
		}
	}
	return nil
}

func text(value any) string {
	result, _ := value.(string)
	return result
}

func loadJSON(path string) (map[string]any, error) {
	if path == "" {
		return nil, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var result map[string]any
	if err = json.Unmarshal(data, &result); err != nil {
		return nil, err
	}
	return result, nil
}

func clientURLFromFile(path string) (string, error) {
	config, err := loadJSON(path)
	if err != nil {
		return "", err
	}
	value := text(config["shadowrocket_url"])
	parsed, err := url.Parse(value)
	password, hasPassword := "", false
	if parsed != nil && parsed.User != nil {
		password, hasPassword = parsed.User.Password()
	}
	if err != nil || parsed == nil || parsed.Scheme != "socks5" || parsed.Host == "" || parsed.User == nil ||
		parsed.User.Username() == "" || !hasPassword || password == "" || len(value) > 4096 {
		return "", errors.New("生成的客户端配置无效")
	}
	return value, nil
}

func clientURL(line LineSpec, operationDir string) (string, error) {
	outputDir := filepath.Join(operationDir, "line-open")
	if line.StateDir != "" {
		outputDir = line.StateDir
	}
	return clientURLFromFile(filepath.Join(outputDir, "provision", line.LineID, "client.json"))
}

func (r *Runner) environment(line LineSpec) (map[string]string, error) {
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
	values := map[string]string{
		"NB_HOSTS_FILE":         line.HostsFile,
		"NB_LINE_PROFILE_FILE":  line.LineProfileFile,
		"NB_SECURITY_DIR":       line.SecurityDir,
		"NB_KNOWN_HOSTS":        line.KnownHostsFile,
		"NB_DEPLOY_INSTANCE":    line.InstanceID,
		"NB_SOCKS_PORT":         strconv.Itoa(line.SocksPort),
		"NB_SOCKS_UDP_PORT_MIN": strconv.Itoa(udpPortMin),
		"NB_SOCKS_UDP_PORT_MAX": strconv.Itoa(udpPortMax),
		"NB_MIDDLE_PORT":        strconv.Itoa(middlePort),
		"NB_EXIT_PORT":          strconv.Itoa(exitPort),
	}
	for key, value := range line.ExtraEnvironment {
		values[key] = value
	}
	credentials, err := loadJSON(line.SourceMachinesFile)
	if err != nil && line.SourceMachinesFile != "" {
		return nil, err
	}
	for _, role := range []string{"entry", "middle", "exit"} {
		host := roleObject(credentials, role)
		if host == nil {
			continue
		}
		envName := text(host["password_env"])
		if envName == "" {
			envName = "NB_SSH_PASSWORD_" + strings.ToUpper(role)
		}
		if password := text(host["password"]); password != "" {
			values[envName] = password
		}
	}
	if line.ClientSecretFile != "" {
		if _, statErr := os.Stat(line.ClientSecretFile); errors.Is(statErr, os.ErrNotExist) {
			return values, nil
		}
		secret, loadErr := loadJSON(line.ClientSecretFile)
		if loadErr != nil {
			return nil, loadErr
		}
		values["NB_SOCKS_USERNAME"], values["NB_SOCKS_PASSWORD"] = text(secret["username"]), text(secret["password"])
	}
	if values["NB_KNOWN_HOSTS"] == "" {
		return nil, errors.New("缺少 known_hosts 主机密钥文件")
	}
	return values, nil
}

func (r *Runner) steps(line LineSpec, operation Operation, request requestValues, operationDir string) ([]commandStep, error) {
	python, tools := r.registry.Python, filepath.Join(r.registry.Root, "tools")
	deploy := filepath.Join(tools, "deploy.py")
	socks := strconv.Itoa(line.SocksPort)
	switch operation.Kind {
	case "line.open":
		outputDir := filepath.Join(operationDir, "line-open")
		if line.StateDir != "" {
			outputDir = line.StateDir
		}
		result := []commandStep{}
		if line.WhitelistSourceEnv != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist-fetch", Args: []string{filepath.Join(tools, "whitelist_sync.py"), "--source-env", line.WhitelistSourceEnv, "--mode", "auto", "--sing-box", line.SingBox, "--state-dir", filepath.Join(line.StateDir, "whitelist-sync"), "--output", line.WhitelistFile}})
		}
		result = append(result, commandStep{Name: python, Stage: "provision", Args: []string{filepath.Join(tools, "line_open.py"), line.SourceMachinesFile,
			"--line-id", line.LineID, "--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64),
			"--upstream-mbps", strconv.FormatFloat(line.UpstreamMbps, 'f', -1, 64), "--downstream-mbps", strconv.FormatFloat(line.DownstreamMbps, 'f', -1, 64),
			"--socks-port", socks, "--middle-port", strconv.Itoa(line.MiddlePort), "--exit-port", strconv.Itoa(line.ExitPort),
			"--udp-port-min", strconv.Itoa(line.UDPPortMin), "--udp-port-max", strconv.Itoa(line.UDPPortMax),
			"--output-dir", outputDir, "--build-mode", line.BuildMode, "--execute"}})
		if line.WhitelistFile != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist", Args: []string{deploy, "wl-push", "--whitelist", line.WhitelistFile}})
		}
		return result, nil
	case "line.validate":
		return []commandStep{{Name: python, Stage: "validate", Args: []string{filepath.Join(tools, "line_probe.py"),
			"--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64), "--active",
			"--upstream-mbps", strconv.FormatFloat(line.UpstreamMbps, 'f', -1, 64),
			"--downstream-mbps", strconv.FormatFloat(line.DownstreamMbps, 'f', -1, 64),
			"--socks-port", socks, "--via-entry-ssh", "--output", filepath.Join(operationDir, "validation.json")}}}, nil
	case "line.tune":
		return nil, nil
	case "line.upgrade":
		result := []commandStep{{Name: python, Stage: "build", Args: []string{deploy, "build"}}}
		if line.WhitelistSourceEnv != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist-fetch", Args: []string{filepath.Join(tools, "whitelist_sync.py"), "--source-env", line.WhitelistSourceEnv, "--mode", "auto", "--sing-box", line.SingBox, "--state-dir", filepath.Join(line.StateDir, "whitelist-sync"), "--output", line.WhitelistFile}})
		}
		if line.WhitelistFile != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist", Args: []string{deploy, "wl-push", "--whitelist", line.WhitelistFile}})
		}
		result = append(result, commandStep{Name: python, Stage: "deploy", Args: []string{deploy, "deploy-socks", "--socks-port", socks}})
		return result, nil
	case "line.rollback":
		if !safeDeployment.MatchString(request.Deployment) {
			return nil, errors.New("回滚操作必须指定有效的 deployment")
		}
		return []commandStep{{Name: python, Stage: "rollback", Args: []string{deploy, "rollback-socks", "--deployment-id", request.Deployment,
			"--socks-port", socks}}}, nil
	case "line.disable":
		return []commandStep{{Name: python, Stage: "stop", Args: []string{deploy, "stop"}}}, nil
	default:
		return nil, errors.New("该任务类型不在 worker 白名单中")
	}
}

func envList(extra map[string]string) []string {
	result := os.Environ()
	for key, value := range extra {
		result = append(result, key+"="+value)
	}
	return result
}

func (r *Runner) execute(ctx context.Context, step commandStep, environment map[string]string, output io.Writer) error {
	command := exec.CommandContext(ctx, step.Name, step.Args...)
	command.Dir = r.registry.Root
	command.Env = envList(environment)
	command.Stdout, command.Stderr = output, output
	_, _ = fmt.Fprintf(output, "+ %s %s\n", filepath.Base(step.Name), strings.Join(step.Args, " "))
	return command.Run()
}

func (r *Runner) currentDeployment(ctx context.Context, line LineSpec, environment map[string]string, output io.Writer) (string, error) {
	step := commandStep{Name: r.registry.Python, Args: []string{filepath.Join(r.registry.Root, "tools", "deploy.py"), "current"}}
	var captured bytes.Buffer
	if err := r.execute(ctx, step, environment, io.MultiWriter(output, &captured)); err != nil {
		return "", err
	}
	marker := "CURRENT_JSON="
	index := strings.LastIndex(captured.String(), marker)
	if index < 0 {
		return "", errors.New("部署状态查询未返回 CURRENT_JSON")
	}
	lineText := strings.SplitN(captured.String()[index+len(marker):], "\n", 2)[0]
	var roles map[string]string
	if err := json.Unmarshal([]byte(lineText), &roles); err != nil {
		return "", err
	}
	deployment := ""
	for _, role := range []string{"entry", "middle", "exit"} {
		if roles[role] == "" || (deployment != "" && roles[role] != deployment) {
			return "", errors.New("各节点角色的 deployment 不一致")
		}
		deployment = roles[role]
	}
	return deployment, nil
}

func (r *Runner) Run(ctx context.Context, operation Operation) (Result, error) {
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: 1, Stage: "prepare", Status: "running", Message: "正在准备任务"})
	failPreparation := func(err error, result Result) (Result, error) {
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: 2, Stage: "prepare", Status: "failed", Message: err.Error()})
		return result, err
	}
	var request requestValues
	if len(operation.Request) > 0 && json.Unmarshal(operation.Request, &request) != nil {
		return failPreparation(errors.New("任务请求无效"), Result{})
	}
	operationDir := filepath.Join(r.registry.StateDir, operation.ID)
	line, ok := r.registry.Line(operation.LineID)
	dynamicResolved := false
	if !ok {
		var resolveErr error
		line, resolveErr = r.dynamicLine(operation, request, operationDir)
		if resolveErr != nil {
			return failPreparation(resolveErr, Result{})
		}
		dynamicResolved = true
	}
	if dynamicResolved && operation.Kind == "line.open" {
		if verifyErr := r.verifyDynamicSSHAccess(ctx, request.Plan); verifyErr != nil {
			return failPreparation(verifyErr, Result{})
		}
	}
	if !line.Allows(operation.Kind) {
		reason := line.DisabledReason
		if reason == "" {
			reason = "operation is disabled by the worker registry"
		}
		return failPreparation(errors.New(reason), Result{})
	}
	if err := os.MkdirAll(operationDir, 0700); err != nil {
		return failPreparation(err, Result{})
	}
	logPath := filepath.Join(operationDir, "worker.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return failPreparation(err, Result{})
	}
	defer logFile.Close()
	_, _ = fmt.Fprintf(logFile, "started_at=%s operation=%s line=%s kind=%s\n",
		time.Now().UTC().Format(time.RFC3339Nano), operation.ID, operation.LineID, operation.Kind)
	environment, err := r.environment(line)
	if err != nil {
		return failPreparation(err, Result{LogFile: logPath})
	}
	steps, err := r.steps(line, operation, request, operationDir)
	if err != nil {
		return failPreparation(err, Result{LogFile: logPath})
	}
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: 2, Stage: "prepare", Status: "succeeded", Message: "任务准备完成"})
	sequence := 3
	for _, step := range steps {
		stage := step.Stage
		if stage == "" {
			stage = "execute"
		}
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "running", Message: "步骤开始执行", Parameters: map[string]any{"program": filepath.Base(step.Name)}})
		sequence++
		progress := newProgressWriter(logFile, 1500*time.Millisecond, 160, func(message string) {
			emitProgress(ctx, &sequence, stage, message)
		})
		err = r.execute(ctx, step, environment, progress)
		progress.Flush()
		if err != nil {
			summary := commandFailureSummary(err, logPath)
			_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "failed", Message: summary})
			return Result{LogFile: logPath}, fmt.Errorf("%s 执行失败：%s", filepath.Base(step.Args[0]), summary)
		}
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "succeeded", Message: "步骤执行完成"})
		sequence++
	}
	result := Result{LogFile: logPath, Profile: line.LineID, Message: "任务执行完成"}
	if operation.Kind == "line.tune" {
		result.TransportProfile = &request.TransportProfile
		generation, profilePath, rolloutErr := r.applyPlannedProfile(ctx, line, request.TransportProfile, environment, logFile, &sequence)
		result.TransportGeneration = generation
		if profilePath != "" {
			result.Profile = fmt.Sprintf("%s:%d", line.LineID, generation)
		}
		result.TransportRollout = summarizeTransportRollout(line, r.registry.StateDir)
		if rolloutErr != nil {
			return result, rolloutErr
		}
	}
	if operation.Kind == "line.validate" {
		if evidence, readErr := os.ReadFile(filepath.Join(operationDir, "validation.json")); readErr == nil && json.Valid(evidence) {
			result.Evidence = json.RawMessage(evidence)
		}
	}
	if operation.Kind == "line.rollback" {
		result.Deployment = request.Deployment
	}
	if operation.Kind != "line.validate" && operation.Kind != "line.disable" && operation.Kind != "line.tune" {
		result.Deployment, err = r.currentDeployment(ctx, line, environment, logFile)
		if err != nil {
			return result, err
		}
	}
	if operation.Kind != "line.tune" {
		if profile, loadErr := loadJSON(line.LineProfileFile); loadErr == nil {
			if schema, ok := profile["schema_version"].(float64); ok {
				result.Profile = fmt.Sprintf("%s:%d", line.LineID, int(schema))
			}
		}
	}
	if operation.Kind == "line.open" {
		result.ClientURL, err = clientURL(line, operationDir)
		if err != nil {
			return result, err
		}
	}
	return result, nil
}
