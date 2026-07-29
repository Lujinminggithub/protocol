package worker

import (
	"context"
	"fmt"
	"math"
	"net/http"
	"regexp"
	"strings"
	"time"
)

var unsafeInventoryID = regexp.MustCompile(`[^A-Za-z0-9_.-]+`)

type inventoryDevice struct {
	ID        string         `json:"id"`
	Name      string         `json:"name"`
	Status    string         `json:"status"`
	Host      string         `json:"host"`
	SSHPort   int            `json:"ssh_port"`
	SSHUser   string         `json:"ssh_user"`
	PrivateIP string         `json:"private_ip"`
	Region    string         `json:"region"`
	Provider  string         `json:"provider"`
	OS        string         `json:"os"`
	Arch      string         `json:"arch"`
	SecretRef string         `json:"secret_ref"`
	Labels    map[string]any `json:"labels"`
}

type inventoryNode struct {
	DeviceID       string         `json:"device_id"`
	Role           string         `json:"role"`
	Ordinal        int            `json:"ordinal"`
	NextHopDevice  string         `json:"next_hop_device_id"`
	JumpCandidates []string       `json:"jump_candidates"`
	Config         map[string]any `json:"config"`
}

type inventoryLine struct {
	Line    map[string]any    `json:"line"`
	Devices []inventoryDevice `json:"devices"`
	Spec    map[string]any    `json:"spec"`
}

func inventoryID(value, fallback string) string {
	value = strings.Trim(unsafeInventoryID.ReplaceAllString(value, "-"), "-.")
	if value == "" {
		value = fallback
	}
	if len(value) > 64 {
		value = value[:64]
	}
	return value
}

func integer(value any, fallback int) int {
	switch number := value.(type) {
	case float64:
		return int(number)
	case int:
		return number
	default:
		return fallback
	}
}

func discoveredDevice(lineID, role string, host map[string]any) (inventoryDevice, bool) {
	address := text(host["host"])
	user := text(host["user"])
	if address == "" || user == "" {
		return inventoryDevice{}, false
	}
	name := inventoryID(text(host["name"]), lineID+"-"+role)
	secretRef := "worker-local:" + lineID + ":" + role
	if environment := text(host["password_env"]); safeID.MatchString(environment) {
		secretRef = "env:" + environment
	}
	return inventoryDevice{ID: name, Name: name, Status: "ready", Host: address,
		SSHPort: integer(host["port"], 22), SSHUser: user, PrivateIP: text(host["private_ip"]),
		Region: text(host["region"]), Provider: text(host["provider"]), OS: text(host["os"]),
		Arch: text(host["arch"]), SecretRef: secretRef,
		Labels: map[string]any{"discovered_by": "nb-web-worker"}}, true
}

func (c *Client) inventoryLines() []inventoryLine {
	result := make([]inventoryLine, 0, len(c.registry.Lines))
	for _, line := range c.registry.Lines {
		source, err := loadJSON(line.SourceMachinesFile)
		if err != nil || len(source) == 0 {
			source, err = loadJSON(line.HostsFile)
		}
		if err != nil || len(source) == 0 {
			continue
		}
		roles := []string{"entry", "middle", "exit"}
		apiRoles := []string{"entry", "relay", "exit"}
		devices := make([]inventoryDevice, 0, 3)
		for index, role := range roles {
			device, ok := discoveredDevice(line.LineID, apiRoles[index], roleObject(source, role))
			if !ok {
				devices = nil
				break
			}
			devices = append(devices, device)
		}
		if len(devices) != 3 {
			continue
		}
		nodes := []inventoryNode{
			{DeviceID: devices[0].ID, Role: "entry", NextHopDevice: devices[1].ID, JumpCandidates: []string{}, Config: map[string]any{}},
			{DeviceID: devices[1].ID, Role: "relay", NextHopDevice: devices[2].ID, JumpCandidates: []string{devices[0].ID}, Config: map[string]any{}},
			{DeviceID: devices[2].ID, Role: "exit", JumpCandidates: []string{devices[1].ID, devices[0].ID}, Config: map[string]any{}},
		}
		result = append(result, inventoryLine{
			Line: map[string]any{"id": line.LineID, "name": line.LineID, "status": "maintenance",
				"entry_region": devices[0].Region, "exit_region": devices[2].Region, "provider": "mixed",
				"capacity_mbps": int64(math.Round(line.PackageMbps)), "active_deployment": "", "profile": "", "secret_ref": ""},
			Devices: devices,
			Spec: map[string]any{"line_id": line.LineID, "resource_group": line.ResourceGroup,
				"instance_id": line.InstanceID, "bandwidth_mbps": int(math.Round(line.PackageMbps)),
				"socks_port": line.SocksPort, "udp_port_min": line.UDPPortMin, "udp_port_max": line.UDPPortMax,
				"relay_port": line.MiddlePort, "exit_port": line.ExitPort, "whitelist": []string{},
				"build_mode": line.BuildMode, "artifact_ref": "build/nb_node", "source_ref": "repo://current",
				"srs_ref": "", "jump_policy": "auto", "nodes": nodes},
		})
	}
	return result
}

func (c *Client) syncInventory(ctx context.Context) error {
	lines := c.inventoryLines()
	if len(lines) == 0 {
		return nil
	}
	payload := map[string]any{"worker_id": c.registry.WorkerID,
		"observed_at": time.Now().UTC().Format(time.RFC3339Nano), "lines": lines}
	if err := c.request(ctx, http.MethodPost, "/agent/v1/inventory", payload, nil); err != nil {
		return fmt.Errorf("inventory discovery failed: %w", err)
	}
	return nil
}
