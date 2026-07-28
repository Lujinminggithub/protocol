package worker

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type Operation struct {
	ID      string          `json:"id"`
	LineID  string          `json:"line_id"`
	Kind    string          `json:"kind"`
	Request json.RawMessage `json:"request"`
}

type Result struct {
	Deployment string `json:"deployment,omitempty"`
	Profile    string `json:"profile,omitempty"`
	LogFile    string `json:"log_file"`
	Message    string `json:"message"`
}

type requestValues struct {
	Deployment string `json:"deployment"`
	Note       string `json:"note"`
}

type commandStep struct {
	Name string
	Args []string
}

type Runner struct{ registry Registry }

func NewRunner(registry Registry) *Runner { return &Runner{registry: registry} }

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
		secret, loadErr := loadJSON(line.ClientSecretFile)
		if loadErr != nil {
			return nil, loadErr
		}
		values["NB_SOCKS_USERNAME"], values["NB_SOCKS_PASSWORD"] = text(secret["username"]), text(secret["password"])
	}
	if values["NB_KNOWN_HOSTS"] == "" {
		return nil, errors.New("known_hosts is required")
	}
	return values, nil
}

func (r *Runner) steps(line LineSpec, operation Operation, request requestValues, operationDir string) ([]commandStep, error) {
	python, tools := r.registry.Python, filepath.Join(r.registry.Root, "tools")
	deploy := filepath.Join(tools, "deploy.py")
	socks := strconv.Itoa(line.SocksPort)
	switch operation.Kind {
	case "line.open":
		return []commandStep{{Name: python, Args: []string{filepath.Join(tools, "line_open.py"), line.SourceMachinesFile,
			"--line-id", line.LineID, "--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64),
			"--socks-port", socks, "--middle-port", strconv.Itoa(line.MiddlePort), "--exit-port", strconv.Itoa(line.ExitPort),
			"--udp-port-min", strconv.Itoa(line.UDPPortMin), "--udp-port-max", strconv.Itoa(line.UDPPortMax),
			"--output-dir", filepath.Join(operationDir, "line-open"), "--execute"}}}, nil
	case "line.validate":
		return []commandStep{{Name: python, Args: []string{filepath.Join(tools, "line_probe.py"),
			"--package-mbps", strconv.FormatFloat(line.PackageMbps, 'f', -1, 64), "--active",
			"--socks-port", socks, "--via-entry-ssh", "--output", filepath.Join(operationDir, "validation.json")}}}, nil
	case "line.upgrade":
		return []commandStep{{Name: python, Args: []string{deploy, "build"}},
			{Name: python, Args: []string{deploy, "deploy-socks", "--socks-port", socks}}}, nil
	case "line.rollback":
		if !safeDeployment.MatchString(request.Deployment) {
			return nil, errors.New("rollback requires a valid deployment")
		}
		return []commandStep{{Name: python, Args: []string{deploy, "rollback-socks", "--deployment-id", request.Deployment,
			"--socks-port", socks}}}, nil
	case "line.disable":
		return []commandStep{{Name: python, Args: []string{deploy, "stop"}}}, nil
	default:
		return nil, errors.New("operation kind is not allowlisted")
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
		return "", errors.New("deployment query did not return CURRENT_JSON")
	}
	lineText := strings.SplitN(captured.String()[index+len(marker):], "\n", 2)[0]
	var roles map[string]string
	if err := json.Unmarshal([]byte(lineText), &roles); err != nil {
		return "", err
	}
	deployment := ""
	for _, role := range []string{"entry", "middle", "exit"} {
		if roles[role] == "" || (deployment != "" && roles[role] != deployment) {
			return "", errors.New("deployment differs across roles")
		}
		deployment = roles[role]
	}
	return deployment, nil
}

func (r *Runner) Run(ctx context.Context, operation Operation) (Result, error) {
	line, ok := r.registry.Line(operation.LineID)
	if !ok {
		return Result{}, errors.New("line is not registered in this worker")
	}
	if !line.Allows(operation.Kind) {
		reason := line.DisabledReason
		if reason == "" {
			reason = "operation is disabled by the worker registry"
		}
		return Result{}, errors.New(reason)
	}
	var request requestValues
	if len(operation.Request) > 0 && json.Unmarshal(operation.Request, &request) != nil {
		return Result{}, errors.New("invalid operation request")
	}
	operationDir := filepath.Join(r.registry.StateDir, operation.ID)
	if err := os.MkdirAll(operationDir, 0700); err != nil {
		return Result{}, err
	}
	logPath := filepath.Join(operationDir, "worker.log")
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return Result{}, err
	}
	defer logFile.Close()
	_, _ = fmt.Fprintf(logFile, "started_at=%s operation=%s line=%s kind=%s\n",
		time.Now().UTC().Format(time.RFC3339Nano), operation.ID, operation.LineID, operation.Kind)
	environment, err := r.environment(line)
	if err != nil {
		return Result{LogFile: logPath}, err
	}
	steps, err := r.steps(line, operation, request, operationDir)
	if err != nil {
		return Result{LogFile: logPath}, err
	}
	for _, step := range steps {
		if err = r.execute(ctx, step, environment, logFile); err != nil {
			return Result{LogFile: logPath}, fmt.Errorf("%s failed: %w", filepath.Base(step.Args[0]), err)
		}
	}
	result := Result{LogFile: logPath, Profile: line.LineID, Message: "operation completed"}
	if operation.Kind == "line.rollback" {
		result.Deployment = request.Deployment
	}
	if operation.Kind != "line.validate" && operation.Kind != "line.disable" {
		result.Deployment, err = r.currentDeployment(ctx, line, environment, logFile)
		if err != nil {
			return result, err
		}
	}
	if profile, loadErr := loadJSON(line.LineProfileFile); loadErr == nil {
		if schema, ok := profile["schema_version"].(float64); ok {
			result.Profile = fmt.Sprintf("%s:%d", line.LineID, int(schema))
		}
	}
	return result, nil
}
