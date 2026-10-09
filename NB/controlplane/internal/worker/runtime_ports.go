package worker

import (
	"archive/tar"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

type runtimePortClaim struct {
	WorkerID     string `json:"worker_id,omitempty"`
	DeviceID     string `json:"device_id,omitempty"`
	Role         string `json:"role,omitempty"`
	ResourceKind string `json:"resource_kind"`
	InstanceID   string `json:"instance_id"`
	PortStart    int    `json:"port_start"`
	PortEnd      int    `json:"port_end"`
	ObservedAt   string `json:"observed_at,omitempty"`
	ExpiresAt    string `json:"expires_at,omitempty"`
	Source       string `json:"source,omitempty"`
}

type runtimePortClaimBatch struct {
	WorkerID     string             `json:"worker_id"`
	DeviceID     string             `json:"device_id"`
	Role         string             `json:"role"`
	ObservedAt   string             `json:"observed_at"`
	ScanComplete bool               `json:"scan_complete"`
	Claims       []runtimePortClaim `json:"claims"`
}

const runtimePortScanCooldown = 5 * time.Minute

type parsedShardConfig struct {
	instance string
	args     []string
	env      map[string]string
}

func parseShardConfig(text string) (parsedShardConfig, error) {
	result := parsedShardConfig{env: map[string]string{}}
	for _, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			return parsedShardConfig{}, errors.New("shard 配置缺少等号")
		}
		switch {
		case key == "instance_id":
			result.instance = value
		case key == "arg":
			result.args = append(result.args, value)
		case strings.HasPrefix(key, "env."):
			result.env[strings.TrimPrefix(key, "env.")] = value
		}
	}
	if result.instance == "" || len(result.args) == 0 {
		return parsedShardConfig{}, errors.New("shard 配置缺少实例或参数")
	}
	return result, nil
}

func argumentValue(args []string, name string) string {
	for index := 0; index+1 < len(args); index++ {
		if args[index] == name {
			return args[index+1]
		}
	}
	return ""
}

func boundedPort(value string) (int, error) {
	port, err := strconv.Atoi(value)
	if err != nil || port < 1 || port > 65535 {
		return 0, errors.New("shard 端口无效")
	}
	return port, nil
}

func parseRuntimeShardPortClaims(workerID, deviceID, role string, files map[string]string, observed time.Time) ([]runtimePortClaim, error) {
	if role != "entry" && role != "relay" && role != "exit" {
		return nil, errors.New("运行时端口角色无效")
	}
	type claimKey struct {
		resource, instance string
		first, last        int
	}
	claims := map[claimKey]bool{}
	for path, contents := range files {
		config, err := parseShardConfig(contents)
		if err != nil {
			return nil, fmt.Errorf("解析 %s 失败: %w", path, err)
		}
		configuredRole := argumentValue(config.args, "-r")
		expectedRole := role
		if role == "relay" {
			expectedRole = "middle"
		}
		if configuredRole != expectedRole {
			return nil, fmt.Errorf("配置 %s 角色不匹配", path)
		}
		option := "-p"
		if role == "entry" {
			option = "-l"
		}
		base, err := boundedPort(argumentValue(config.args, option))
		if err != nil {
			return nil, fmt.Errorf("配置 %s: %w", path, err)
		}
		actual := base
		if role != "entry" && config.env["NB_WORKER_LANE_PORTS"] == "on" {
			workerText := strings.Split(filepathToSlash(path), "/")[0]
			worker, workerErr := strconv.Atoi(workerText)
			if workerErr != nil || worker < 0 || base+worker > 65535 {
				return nil, fmt.Errorf("配置 %s worker 无效", path)
			}
			actual += worker
		}
		resource := "transport"
		if role == "entry" {
			resource = "socks"
		}
		claims[claimKey{resource: resource, instance: config.instance, first: actual, last: actual}] = true
		if role == "entry" && config.env["NB_SOCKS_UDP_PORT_MIN"] != "" {
			first, firstErr := boundedPort(config.env["NB_SOCKS_UDP_PORT_MIN"])
			last, lastErr := boundedPort(config.env["NB_SOCKS_UDP_PORT_MAX"])
			if firstErr != nil || lastErr != nil || first > last {
				return nil, fmt.Errorf("配置 %s UDP 端口段无效", path)
			}
			claims[claimKey{resource: "udp", instance: config.instance, first: first, last: last}] = true
		}
	}
	keys := make([]claimKey, 0, len(claims))
	for key := range claims {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].resource != keys[j].resource {
			return keys[i].resource < keys[j].resource
		}
		if keys[i].instance != keys[j].instance {
			return keys[i].instance < keys[j].instance
		}
		return keys[i].first < keys[j].first
	})
	merged := make([]claimKey, 0, len(keys))
	for _, key := range keys {
		last := len(merged) - 1
		if last >= 0 && merged[last].resource == key.resource && merged[last].instance == key.instance && key.first <= merged[last].last+1 {
			if key.last > merged[last].last {
				merged[last].last = key.last
			}
			continue
		}
		merged = append(merged, key)
	}
	observed = observed.UTC()
	result := make([]runtimePortClaim, 0, len(merged))
	for _, key := range merged {
		result = append(result, runtimePortClaim{WorkerID: workerID, DeviceID: deviceID, Role: role,
			ResourceKind: key.resource, InstanceID: key.instance, PortStart: key.first, PortEnd: key.last,
			ObservedAt: observed.Format(time.RFC3339Nano), ExpiresAt: observed.Add(5 * time.Minute).Format(time.RFC3339Nano), Source: "runtime"})
	}
	return result, nil
}

func filepathToSlash(value string) string { return strings.ReplaceAll(value, "\\", "/") }

func isMissingRuntimeShardConfigExitStatus(status int) bool { return status == 3 }

func runtimeScanFailure(deviceID, stage string, cause error) string {
	detail := "未知错误"
	if cause != nil && strings.TrimSpace(cause.Error()) != "" {
		detail = strings.TrimSpace(cause.Error())
	}
	if len(detail) > 240 {
		detail = detail[:240] + "..."
	}
	return fmt.Sprintf("设备 %s 运行时端口扫描失败（%s）：%s", deviceID, stage, detail)
}

func isIgnorableRuntimeScanError(err error) bool {
	if err == nil {
		return false
	}
	parts := strings.Split(err.Error(), ";")
	if len(parts) == 0 {
		return false
	}
	for _, part := range parts {
		if !strings.Contains(strings.TrimSpace(part), "运行时端口扫描失败") {
			return false
		}
	}
	return true
}

func (c *Client) runtimePlanCooling(plan dynamicPlan, now time.Time) bool {
	c.runtimeScanMu.Lock()
	defer c.runtimeScanMu.Unlock()
	for _, node := range plan.Nodes {
		if until, ok := c.runtimeScanCooldowns[node.DeviceID]; ok && until.After(now) {
			return true
		}
	}
	return false
}

func (c *Client) noteRuntimeScanFailure(plans []dynamicPlan, now time.Time, err error) {
	if !isIgnorableRuntimeScanError(err) {
		return
	}
	c.runtimeScanMu.Lock()
	defer c.runtimeScanMu.Unlock()
	if c.runtimeScanCooldowns == nil {
		c.runtimeScanCooldowns = map[string]time.Time{}
	}
	until := now.Add(runtimePortScanCooldown)
	for _, plan := range plans {
		for _, node := range plan.Nodes {
			c.runtimeScanCooldowns[node.DeviceID] = until
		}
	}
}

func (c *Client) filterRuntimeScanPlans(plans []dynamicPlan, now time.Time) []dynamicPlan {
	result := make([]dynamicPlan, 0, len(plans))
	for _, plan := range plans {
		if !c.runtimePlanCooling(plan, now) {
			result = append(result, plan)
		}
	}
	return result
}

func readRemoteShardConfigs(client *ssh.Client, role string) (map[string]string, error) {
	directory := role
	if role == "relay" {
		directory = "middle"
	}
	if directory != "entry" && directory != "middle" && directory != "exit" {
		return nil, errors.New("远程 shard 角色无效")
	}
	session, err := client.NewSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	stdout, err := session.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err = session.Start("if test -d /etc/NB/shards/configs/" + directory + "; then tar -C /etc/NB/shards/configs/" + directory + " -cf - .; else exit 3; fi"); err != nil {
		return nil, err
	}
	reader := tar.NewReader(stdout)
	files := map[string]string{}
	for {
		header, nextErr := reader.Next()
		if errors.Is(nextErr, io.EOF) {
			break
		}
		if nextErr != nil {
			return nil, nextErr
		}
		name := strings.TrimPrefix(filepathToSlash(header.Name), "./")
		if header.Typeflag != tar.TypeReg || !strings.HasSuffix(name, ".conf") || header.Size < 1 || header.Size > 65536 {
			continue
		}
		data, readErr := io.ReadAll(io.LimitReader(reader, 65537))
		if readErr != nil || len(data) > 65536 {
			return nil, fmt.Errorf("读取远程 shard 配置 %s 失败", name)
		}
		files[name] = string(data)
	}
	if err = session.Wait(); err != nil {
		var exitErr *ssh.ExitError
		if errors.As(err, &exitErr) && isMissingRuntimeShardConfigExitStatus(exitErr.ExitStatus()) {
			return files, nil
		}
		return nil, err
	}
	return files, nil
}

func (r *Runner) runtimePortBatches(ctx context.Context, plans []dynamicPlan) ([]runtimePortClaimBatch, error) {
	seen := map[string]bool{}
	result := []runtimePortClaimBatch{}
	var failures []string
	clients := map[string]*ssh.Client{}
	defer func() {
		for _, client := range clients {
			_ = client.Close()
		}
	}()
	for _, plan := range plans {
		for _, node := range plan.Nodes {
			key := node.DeviceID + ":" + node.Role
			if seen[key] {
				continue
			}
			secret, err := r.resolveSecret(node.Device.SecretRef)
			if err != nil {
				failures = append(failures, fmt.Sprintf("设备 %s 凭据不可用", node.DeviceID))
				continue
			}
			client := clients[node.DeviceID]
			var dialErr error
			if client == nil {
				client, dialErr = dialDynamicSSH(ctx, node, resolvedPassword(secret), nil)
			}
			if dialErr != nil {
				for _, candidate := range node.JumpCandidates {
					if jump := clients[candidate]; jump != nil {
						client, dialErr = dialDynamicSSH(ctx, node, resolvedPassword(secret), jump)
						if dialErr == nil {
							break
						}
					}
				}
			}
			if client == nil {
				failures = append(failures, runtimeScanFailure(node.DeviceID, "SSH连接", dialErr))
				continue
			}
			clients[node.DeviceID] = client
			files, scanErr := readRemoteShardConfigs(client, node.Role)
			if scanErr != nil {
				failures = append(failures, runtimeScanFailure(node.DeviceID, "读取配置", scanErr))
				continue
			}
			observed := time.Now().UTC()
			claims, parseErr := parseRuntimeShardPortClaims(r.registry.WorkerID, node.DeviceID, node.Role, files, observed)
			if parseErr != nil {
				failures = append(failures, fmt.Sprintf("设备 %s 运行时端口解析失败：%s", node.DeviceID, parseErr))
				continue
			}
			seen[key] = true
			result = append(result, runtimePortClaimBatch{WorkerID: r.registry.WorkerID, DeviceID: node.DeviceID,
				Role: node.Role, ObservedAt: observed.Format(time.RFC3339Nano), ScanComplete: true, Claims: claims})
		}
	}
	if len(failures) > 0 {
		return result, errors.New(strings.Join(failures, "; "))
	}
	return result, nil
}

func (c *Client) postRuntimePortBatches(ctx context.Context, batches []runtimePortClaimBatch) error {
	for _, batch := range batches {
		if err := c.request(ctx, http.MethodPost, "/agent/v1/runtime-port-claims", batch, nil); err != nil {
			return err
		}
	}
	return nil
}

func (c *Client) syncRuntimePortClaims(ctx context.Context) error {
	scanner, ok := c.runner.(interface {
		runtimePortBatches(context.Context, []dynamicPlan) ([]runtimePortClaimBatch, error)
	})
	if !ok || !c.registry.Dynamic.Enabled {
		return nil
	}
	var response struct {
		Plans []dynamicPlan `json:"plans"`
	}
	if err := c.request(ctx, http.MethodGet, "/agent/v1/runtime-port-scan-plans", nil, &response); err != nil {
		return err
	}
	plans := c.filterRuntimeScanPlans(response.Plans, time.Now())
	if len(plans) == 0 {
		return nil
	}
	batches, scanErr := scanner.runtimePortBatches(ctx, plans)
	if scanErr != nil {
		c.noteRuntimeScanFailure(plans, time.Now(), scanErr)
	}
	if err := c.postRuntimePortBatches(ctx, batches); err != nil {
		return err
	}
	return scanErr
}

func (c *Client) prepareRuntimePorts(ctx context.Context, operation Operation) (Operation, error) {
	if operation.Kind != "line.open" || !c.registry.Dynamic.Enabled {
		return operation, nil
	}
	var request requestValues
	if len(operation.Request) == 0 || json.Unmarshal(operation.Request, &request) != nil || request.Plan.LineID != operation.LineID {
		return Operation{}, errors.New("开线任务缺少有效端口扫描计划")
	}
	scanner, ok := c.runner.(interface {
		runtimePortBatches(context.Context, []dynamicPlan) ([]runtimePortClaimBatch, error)
	})
	if !ok {
		return Operation{}, errors.New("当前 Worker 不支持运行时端口扫描")
	}
	batches, scanErr := scanner.runtimePortBatches(ctx, []dynamicPlan{request.Plan})
	if err := c.postRuntimePortBatches(ctx, batches); err != nil {
		return Operation{}, err
	}
	if scanErr != nil {
		return Operation{}, scanErr
	}
	var prepared Operation
	payload := map[string]string{"line_id": operation.LineID, "worker_id": c.registry.WorkerID}
	if err := c.request(ctx, http.MethodPost, "/agent/v1/operations/"+url.PathEscape(operation.ID)+"/port-preflight", payload, &prepared); err != nil {
		return Operation{}, err
	}
	return prepared, nil
}

func cleanupInstanceID(base, role string) string {
	if role == "relay" {
		role = "middle"
	}
	return base + "-" + role
}

func (c *Client) confirmRuntimeCleanup(ctx context.Context, operation Operation) error {
	if operation.Kind != "line.disable" || !c.registry.Dynamic.Enabled {
		return nil
	}
	var request requestValues
	if len(operation.Request) == 0 || json.Unmarshal(operation.Request, &request) != nil || !request.DeleteAfterCleanup {
		return nil
	}
	if request.Plan.LineID != operation.LineID || request.Plan.InstanceID == "" {
		return errors.New("线路删除清理计划无效")
	}
	scanner, ok := c.runner.(interface {
		runtimePortBatches(context.Context, []dynamicPlan) ([]runtimePortClaimBatch, error)
	})
	if !ok {
		return errors.New("当前 Worker 不支持删除后运行时扫描")
	}
	batches, scanErr := scanner.runtimePortBatches(ctx, []dynamicPlan{request.Plan})
	if scanErr != nil && !isIgnorableRuntimeScanError(scanErr) {
		return scanErr
	}
	scanSkipped := scanErr != nil
	seen := map[string]bool{}
	for _, batch := range batches {
		seen[batch.DeviceID+":"+batch.Role] = batch.ScanComplete
		expected := cleanupInstanceID(request.Plan.InstanceID, batch.Role)
		for _, claim := range batch.Claims {
			if claim.InstanceID == expected {
				return fmt.Errorf("节点 %s 仍存在线路实例 %s", batch.DeviceID, expected)
			}
		}
	}
	for _, node := range request.Plan.Nodes {
		if !seen[node.DeviceID+":"+node.Role] && !scanSkipped {
			return fmt.Errorf("节点 %s 尚未完成删除后端口扫描", node.DeviceID)
		}
	}
	return c.postRuntimePortBatches(ctx, batches)
}
