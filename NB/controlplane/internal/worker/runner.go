package worker

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
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
	Failure             *FailureDetail            `json:"failure,omitempty"`
	NodeRelease         json.RawMessage           `json:"node_release,omitempty"`
}

type FailureDetail struct {
	Stage      string `json:"stage"`
	Summary    string `json:"summary"`
	RootCause  string `json:"root_cause"`
	LogExcerpt string `json:"log_excerpt,omitempty"`
	LogFile    string `json:"log_file,omitempty"`
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
	Deployment         string                   `json:"deployment"`
	DeploymentID       string                   `json:"deployment_id"`
	Note               string                   `json:"note"`
	Plan               dynamicPlan              `json:"plan"`
	DeleteAfterCleanup bool                     `json:"delete_after_cleanup"`
	UploadID           string                   `json:"upload_id"`
	Archive            string                   `json:"archive"`
	ArchiveSHA256      string                   `json:"archive_sha256"`
	TransportProfile   transportprofile.Profile `json:"transport_profile"`
}

type commandStep struct {
	Name  string
	Args  []string
	Stage string
}

type Runner struct{ registry Registry }

func NewRunner(registry Registry) *Runner { return &Runner{registry: registry} }

var sensitiveErrorValue = regexp.MustCompile(`(?i)(password|passwd|token|secret|api[_-]?key)(\s*[:=]\s*)("[^"]*"|'[^']*'|[^\s,;]+)`)
var sensitiveURLUserInfo = regexp.MustCompile(`(?i)([a-z][a-z0-9+.-]*://)[^/@\s]+@`)
var privateKeyBlock = regexp.MustCompile(`(?s)-----BEGIN [^-]*PRIVATE KEY-----.*?-----END [^-]*PRIVATE KEY-----`)

func redactFailureText(value string) string {
	value = privateKeyBlock.ReplaceAllString(value, "[REDACTED PRIVATE KEY]")
	value = sensitiveURLUserInfo.ReplaceAllString(value, "$1[REDACTED]@")
	return sensitiveErrorValue.ReplaceAllString(value, "$1$2[REDACTED]")
}

func operationFailure(stage string, err error, logPath string) *FailureDetail {
	detail := &FailureDetail{Stage: stage, Summary: commandFailureSummary(err, logPath), RootCause: redactFailureText(err.Error()), LogFile: logPath}
	if data, readErr := os.ReadFile(logPath); readErr == nil {
		lines := strings.Split(strings.TrimSpace(string(data)), "\n")
		if len(lines) > 60 {
			lines = lines[len(lines)-60:]
		}
		excerpt := redactFailureText(strings.Join(lines, "\n"))
		if len(excerpt) > 12<<10 {
			excerpt = excerpt[len(excerpt)-(12<<10):]
		}
		detail.LogExcerpt = excerpt
		for index := len(lines) - 1; index >= 0; index-- {
			line := strings.TrimSpace(lines[index])
			for _, marker := range []string{"RuntimeError:", "ValueError:", "TimeoutError:", "FileNotFoundError:", "PermissionError:", "AssertionError:"} {
				if strings.Contains(line, marker) {
					detail.RootCause = redactFailureText(line)
					return detail
				}
			}
		}
	}
	return detail
}

func commandFailureSummary(err error, logPath string) string {
	summary := err.Error()
	if data, readErr := os.ReadFile(logPath); readErr == nil {
		lines := strings.Split(string(data), "\n")
		for _, marker := range []string{"拒绝开通:", "发布清单校验失败:"} {
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
		if summary == err.Error() {
			markers := []string{"FileNotFoundError:", "ConnectionAbortedError:", "ConnectionResetError:",
				"TimeoutError:", "RuntimeError:", "ValueError:", "SystemExit:"}
			for index := len(lines) - 1; index >= 0; index-- {
				for _, marker := range markers {
					if strings.Contains(lines[index], marker) {
						summary = strings.TrimSpace(lines[index])
						break
					}
				}
				if summary != err.Error() {
					break
				}
			}
		}
	}
	summary = redactFailureText(summary)
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
	if err := line.normalizeTopology(); err != nil {
		return nil, err
	}
	values["NB_TOPOLOGY_MODE"] = line.TopologyMode
	values["NB_SERVICE_PROFILE"] = line.ServiceProfile
	if line.TopologyMode == "single_hk" {
		values["NB_FEC_V15_ACTIVE"] = "off"
	}
	for key, value := range line.ExtraEnvironment {
		values[key] = value
	}
	values["NB_LINE_OPEN_FAST"] = "1"
	values["NB_BUILD_LOCAL"] = "1"
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
	lineState := line.StateDir
	if lineState == "" {
		lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
	}
	probePreflight := commandStep{Name: python, Stage: "probe-preflight", Args: []string{
		filepath.Join(tools, "probe_cleanup.py"), "--preflight",
	}}
	switch operation.Kind {
	case "line.open":
		outputDir := filepath.Join(operationDir, "line-open")
		if line.StateDir != "" {
			outputDir = line.StateDir
		}
		result := []commandStep{probePreflight}
		if line.WhitelistSourceEnv != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist-fetch", Args: []string{filepath.Join(tools, "whitelist_sync.py"), "--source-env", line.WhitelistSourceEnv, "--mode", "auto", "--sing-box", line.SingBox, "--state-dir", filepath.Join(line.StateDir, "whitelist-sync"), "--output", line.WhitelistFile}})
		}
		provisionArgs := []string{filepath.Join(tools, "line_open.py"), line.SourceMachinesFile,
			"--line-id", line.LineID, "--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64),
			"--upstream-mbps", strconv.FormatFloat(line.UpstreamMbps, 'f', -1, 64), "--downstream-mbps", strconv.FormatFloat(line.DownstreamMbps, 'f', -1, 64),
			"--socks-port", socks,
			"--udp-port-min", strconv.Itoa(line.UDPPortMin), "--udp-port-max", strconv.Itoa(line.UDPPortMax),
			"--exit-port", strconv.Itoa(line.ExitPort), "--topology-mode", line.TopologyMode,
			"--service-profile", line.ServiceProfile, "--output-dir", outputDir, "--build-mode", line.BuildMode, "--execute"}
		if line.TopologyMode == "trihop" {
			provisionArgs = append(provisionArgs, "--middle-port", strconv.Itoa(line.MiddlePort))
		}
		result = append(result, commandStep{Name: python, Stage: "provision", Args: provisionArgs})
		if line.WhitelistFile != "" {
			result = append(result, commandStep{Name: python, Stage: "whitelist", Args: []string{deploy, "wl-push", "--whitelist", line.WhitelistFile}})
		}
		return result, nil
	case "line.validate", "line.optimize":
		return []commandStep{probePreflight, {Name: python, Stage: "validate", Args: []string{filepath.Join(tools, "line_probe.py"),
			"--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64), "--active",
			"--upstream-mbps", strconv.FormatFloat(line.UpstreamMbps, 'f', -1, 64),
			"--downstream-mbps", strconv.FormatFloat(line.DownstreamMbps, 'f', -1, 64),
			"--minimum-throughput-ratio", "0.95", "--duration", "90",
			"--socks-port", socks, "--via-entry-ssh", "--cache", filepath.Join(lineState, "provision", line.LineID, "probe-cache.json"),
			"--output", filepath.Join(operationDir, "validation.json")}}}, nil
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
		return []commandStep{{Name: python, Stage: "stop", Args: []string{deploy, "stop", "--ignore-unavailable"}}}, nil
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

func (r *Runner) runNodeReleaseBuild(ctx context.Context, operation Operation, request requestValues) (Result, error) {
	operationDir := filepath.Join(r.registry.StateDir, operation.ID)
	if !safeID.MatchString(request.UploadID) {
		return Result{}, errors.New("Node 源码上传标识无效")
	}
	uploadRoot := filepath.Clean(filepath.Join(filepath.Dir(r.registry.StateDir), "source-uploads"))
	archive := filepath.Clean(request.Archive)
	if filepath.Dir(archive) != uploadRoot || filepath.Base(archive) != request.UploadID+".archive" {
		return Result{}, errors.New("Node 源码包路径不在受限上传目录")
	}
	archiveFile, err := os.Open(archive)
	if err != nil {
		return Result{}, errors.New("Node 源码包不可读取")
	}
	hasher := sha256.New()
	_, hashErr := io.Copy(hasher, archiveFile)
	closeErr := archiveFile.Close()
	if hashErr != nil || closeErr != nil {
		return Result{}, errors.New("Node 源码包 SHA256 计算失败")
	}
	if !strings.EqualFold(hex.EncodeToString(hasher.Sum(nil)), request.ArchiveSHA256) {
		return Result{}, errors.New("Node 源码包 SHA256 不匹配")
	}
	if err := os.MkdirAll(operationDir, 0700); err != nil {
		return Result{}, err
	}
	logPath := filepath.Join(operationDir, "worker.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return Result{}, err
	}
	defer logFile.Close()
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: 1, Stage: "source-verify", Status: "running", Message: "正在校验上传 Git 仓库"})
	step := commandStep{Name: r.registry.Python, Stage: "node-build", Args: []string{filepath.Join(r.registry.Root, "tools", "node_release_upload.py"),
		"--archive", archive, "--current-root", r.registry.Root, "--operation-id", operation.ID}}
	var captured bytes.Buffer
	if err = r.execute(ctx, step, nil, io.MultiWriter(logFile, &captured)); err != nil {
		summary := commandFailureSummary(err, logPath)
		failure := operationFailure("node-build", fmt.Errorf("Node Release 构建失败：%s", summary), logPath)
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: 2, Stage: "node-build", Status: "failed", Message: failure.Summary})
		return Result{LogFile: logPath, Message: failure.Summary, Failure: failure}, err
	}
	marker := "NODE_RELEASE_JSON="
	index := strings.LastIndex(captured.String(), marker)
	if index < 0 {
		return Result{LogFile: logPath}, errors.New("Node Release 构建未返回发布元数据")
	}
	line := strings.SplitN(captured.String()[index+len(marker):], "\n", 2)[0]
	if !json.Valid([]byte(line)) {
		return Result{LogFile: logPath}, errors.New("Node Release 发布元数据无效")
	}
	_ = os.Remove(archive)
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: 2, Stage: "node-build", Status: "succeeded", Message: "Node Release 构建并激活完成"})
	return Result{LogFile: logPath, Message: "Node Release 构建并激活完成", NodeRelease: json.RawMessage(line)}, nil
}

func operationNeedsProbeCleanup(kind string) bool {
	return kind == "line.open" || kind == "line.validate" || kind == "line.optimize"
}

func (r *Runner) cleanupProbes(operation Operation, logPath string) error {
	var request requestValues
	if len(operation.Request) > 0 && json.Unmarshal(operation.Request, &request) != nil {
		return errors.New("任务请求无效，无法清理主动探针")
	}
	operationDir := filepath.Join(r.registry.StateDir, operation.ID)
	line, ok := r.registry.Line(operation.LineID)
	if !ok {
		var err error
		line, err = r.dynamicLine(operation, request, operationDir)
		if err != nil {
			return err
		}
	}
	environment, err := r.environment(line)
	if err != nil {
		return err
	}
	args := []string{filepath.Join(r.registry.Root, "tools", "probe_cleanup.py")}
	output := io.Discard
	var logFile *os.File
	if logPath != "" {
		logFile, err = os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
		if err != nil {
			return err
		}
		defer logFile.Close()
		output = logFile
	}
	cleanupCtx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	return r.execute(cleanupCtx, commandStep{Name: r.registry.Python, Stage: "probe-cleanup", Args: args}, environment, output)
}

func appendCleanupDiagnostic(result *Result, cleanupErr error) {
	line := redactFailureText("主动探针清理失败: " + cleanupErr.Error())
	if result.LogFile != "" {
		if logFile, err := os.OpenFile(result.LogFile, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600); err == nil {
			_, _ = fmt.Fprintln(logFile, line)
			_ = logFile.Close()
		}
	}
	if result.Failure != nil {
		if result.Failure.LogExcerpt != "" {
			result.Failure.LogExcerpt += "\n"
		}
		result.Failure.LogExcerpt += line
	}
}

func (r *Runner) Run(ctx context.Context, operation Operation) (result Result, runErr error) {
	result, runErr = r.runOperation(ctx, operation)
	if !operationNeedsProbeCleanup(operation.Kind) {
		return result, runErr
	}
	cleanupErr := r.cleanupProbes(operation, result.LogFile)
	if cleanupErr == nil {
		return result, runErr
	}
	if runErr != nil {
		appendCleanupDiagnostic(&result, cleanupErr)
		return result, runErr
	}
	result.Failure = operationFailure("probe-cleanup", cleanupErr, result.LogFile)
	result.Message = result.Failure.Summary
	return result, cleanupErr
}

func (r *Runner) runOperation(ctx context.Context, operation Operation) (Result, error) {
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: 1, Stage: "prepare", Status: "running", Message: "正在准备任务"})
	failPreparation := func(err error, result Result) (Result, error) {
		result.Failure = operationFailure("prepare", err, result.LogFile)
		result.Message = result.Failure.Summary
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: 2, Stage: "prepare", Status: "failed",
			Message: result.Failure.Summary, Parameters: map[string]any{"failure": result.Failure}})
		return result, err
	}
	var request requestValues
	if len(operation.Request) > 0 && json.Unmarshal(operation.Request, &request) != nil {
		return failPreparation(errors.New("任务请求无效"), Result{})
	}
	if operation.Kind == "node.release.build" {
		return r.runNodeReleaseBuild(ctx, operation, request)
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
			runErr := fmt.Errorf("%s 执行失败：%s", filepath.Base(step.Args[0]), summary)
			failure := operationFailure(stage, runErr, logPath)
			_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "failed",
				Message: failure.Summary, Parameters: map[string]any{"failure": failure}})
			return Result{LogFile: logPath, Message: failure.Summary, Failure: failure}, runErr
		}
		_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "succeeded", Message: "步骤执行完成"})
		sequence++
	}
	result := Result{LogFile: logPath, Profile: line.LineID, Message: "任务执行完成"}
	if operation.Kind == "line.optimize" {
		evidence, readErr := os.ReadFile(filepath.Join(operationDir, "validation.json"))
		if readErr != nil || !json.Valid(evidence) {
			optimizeErr := errors.New("线路验证未生成有效调优证据")
			result.Failure = operationFailure("validation", optimizeErr, logPath)
			result.Message = result.Failure.Summary
			return result, optimizeErr
		}
		var probe transportprofile.Probe
		if json.Unmarshal(evidence, &probe) != nil {
			optimizeErr := errors.New("线路验证证据格式无效")
			result.Failure = operationFailure("profile-generate", optimizeErr, logPath)
			result.Message = result.Failure.Summary
			return result, optimizeErr
		}
		result.Evidence = json.RawMessage(evidence)
		if admissionErr := probe.AdmissionError(); admissionErr != nil {
			validationErr := fmt.Errorf("线路验证未通过：%w", admissionErr)
			result.Failure = operationFailure("validation", validationErr, logPath)
			result.Message = result.Failure.Summary
			_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: "validation", Status: "failed",
				Message: result.Failure.Summary, Parameters: map[string]any{"failure": result.Failure,
					"admission": probe.Admission}})
			return result, validationErr
		}
		profile, generateErr := transportprofile.Generate(line.LineID, 1, line.PackageMbps, probe)
		if generateErr != nil {
			result.Failure = operationFailure("profile-generate", generateErr, logPath)
			result.Message = result.Failure.Summary
			return result, generateErr
		}
		request.TransportProfile = profile
		_ = atomicJSON(filepath.Join(operationDir, "optimize-checkpoint.json"), map[string]any{
			"line_id": line.LineID, "deployment_id": request.DeploymentID,
			"stage": "profile-generated", "evidence": json.RawMessage(evidence), "profile": profile,
		})
	}
	if operation.Kind == "line.tune" || operation.Kind == "line.optimize" {
		generation, profilePath, rolloutErr := r.applyPlannedProfile(ctx, line, request.DeploymentID, request.TransportProfile, environment, logFile, &sequence)
		result.TransportGeneration = generation
		request.TransportProfile.Generation = generation
		result.TransportProfile = &request.TransportProfile
		if profilePath != "" {
			result.Profile = fmt.Sprintf("%s:%d", line.LineID, generation)
		}
		result.TransportRollout = summarizeTransportRollout(line, r.registry.StateDir, request.DeploymentID)
		if rolloutErr != nil {
			stage := "profile-rollout"
			if profilePath == "" {
				stage = "profile-reconcile"
			}
			result.Failure = operationFailure(stage, rolloutErr, logPath)
			result.Message = result.Failure.Summary
			_ = emitOperationEvent(ctx, OperationEvent{Sequence: sequence, Stage: stage, Status: "failed",
				Message: result.Failure.Summary, Parameters: map[string]any{"failure": result.Failure}})
			return result, rolloutErr
		}
		if operation.Kind == "line.optimize" {
			result.Message = "线路验证与协议调优完成"
			lineState := line.StateDir
			if lineState == "" {
				lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
			}
			_ = os.Remove(filepath.Join(lineState, "qualification-pending.json"))
			_ = atomicJSON(filepath.Join(lineState, "qualification-complete.json"), map[string]any{
				"line_id": line.LineID, "deployment_id": request.DeploymentID,
				"generation": generation, "completed_at": time.Now().UTC().Format(time.RFC3339Nano),
			})
			_ = atomicJSON(filepath.Join(operationDir, "optimize-checkpoint.json"), map[string]any{
				"line_id": line.LineID, "deployment_id": request.DeploymentID,
				"stage": "committed", "generation": generation,
			})
		}
	}
	if operation.Kind == "line.validate" || operation.Kind == "line.optimize" {
		if evidence, readErr := os.ReadFile(filepath.Join(operationDir, "validation.json")); readErr == nil && json.Valid(evidence) {
			result.Evidence = json.RawMessage(evidence)
		}
	}
	if operation.Kind == "line.rollback" {
		result.Deployment = request.Deployment
	}
	if operation.Kind != "line.validate" && operation.Kind != "line.disable" && operation.Kind != "line.tune" && operation.Kind != "line.optimize" {
		result.Deployment, err = r.currentDeployment(ctx, line, environment, logFile)
		if err != nil {
			return result, err
		}
	}
	if operation.Kind != "line.tune" && operation.Kind != "line.optimize" {
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
		lineState := line.StateDir
		if lineState == "" {
			lineState = filepath.Join(r.registry.StateDir, "lines", line.LineID)
		}
		if _, pendingErr := os.Stat(filepath.Join(lineState, "qualification-pending.json")); pendingErr == nil {
			result.Message = "线路已可用，等待验证并调优"
			result.Evidence = json.RawMessage(`{"qualification":"pending"}`)
		}
	}
	return result, nil
}
