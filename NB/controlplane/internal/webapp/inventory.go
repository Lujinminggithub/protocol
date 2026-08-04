package webapp

import (
	"database/sql"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/url"
	"path/filepath"
	"strconv"
	"strings"

	"nb-controlplane/internal/central"
)

type discoveredLine struct {
	Line    central.Line     `json:"line"`
	Devices []central.Device `json:"devices"`
	Spec    central.LineSpec `json:"spec"`
}

type inventoryDiscovery struct {
	WorkerID   string           `json:"worker_id"`
	ObservedAt string           `json:"observed_at"`
	Lines      []discoveredLine `json:"lines"`
}

type deviceUpsertRequest struct {
	central.Device
	Password string `json:"password"`
}

func validHost(value string) bool {
	if net.ParseIP(value) != nil {
		return true
	}
	if len(value) < 1 || len(value) > 253 {
		return false
	}
	for _, part := range strings.Split(value, ".") {
		if part == "" || len(part) > 63 || !safeID.MatchString(strings.ReplaceAll(part, "_", "-")) {
			return false
		}
	}
	return true
}

func validDevice(item central.Device) error {
	if !safeID.MatchString(item.ID) || strings.TrimSpace(item.Name) == "" || len(item.Name) > 100 {
		return errors.New("invalid device identity")
	}
	if item.Status != "ready" && item.Status != "provisioning" && item.Status != "maintenance" && item.Status != "offline" && item.Status != "retired" {
		return errors.New("invalid device status")
	}
	if !validHost(item.Host) || item.SSHPort < 1 || item.SSHPort > 65535 || strings.TrimSpace(item.SSHUser) == "" {
		return errors.New("invalid SSH endpoint")
	}
	if item.PrivateIP != "" && net.ParseIP(item.PrivateIP) == nil {
		return errors.New("invalid private IP")
	}
	if item.SecretRef == "" || !safeSecretRef.MatchString(item.SecretRef) {
		return errors.New("valid secret_ref is required")
	}
	if len(item.Labels) > 16<<10 || (len(item.Labels) > 0 && !json.Valid(item.Labels)) {
		return errors.New("invalid labels")
	}
	return nil
}

func validWhitelistSource(value string) bool {
	if strings.Contains(value, "://") {
		parsed, err := url.ParseRequestURI(value)
		return err == nil && parsed.Scheme == "https" && parsed.Host != "" && parsed.User == nil && parsed.Fragment == ""
	}
	return safeSecretRef.MatchString(value) // Backward compatibility for existing env:/secret references.
}

func (a *App) devices(w http.ResponseWriter, r *http.Request) {
	items, err := a.store.Devices(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"devices": items})
}

func (a *App) upsertDevice(w http.ResponseWriter, r *http.Request) {
	var request deviceUpsertRequest
	if !decode(w, r, &request) {
		return
	}
	item := request.Device
	if item.Status == "" {
		item.Status = "ready"
	}
	if item.SSHPort == 0 {
		item.SSHPort = 22
	}
	existing, existingErr := a.store.Device(r.Context(), item.ID)
	if existingErr == nil && item.SecretRef == "" {
		item.SecretRef = existing.SecretRef
	}
	if existingErr != nil && !errors.Is(existingErr, sql.ErrNoRows) {
		problem(w, 500, existingErr.Error())
		return
	}
	if request.Password != "" {
		if len(request.Password) > 4096 {
			problem(w, 400, "device password is too long")
			return
		}
		item.SecretRef = "device:" + item.ID
	} else if errors.Is(existingErr, sql.ErrNoRows) {
		problem(w, 400, "SSH password is required when registering a device")
		return
	}
	if err := validDevice(item); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if request.Password != "" {
		if err := a.deviceSecrets.update(item.SecretRef, request.Password); err != nil {
			problem(w, 500, "failed to store device password")
			return
		}
	}
	result, err := a.store.UpsertDevice(r.Context(), item)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 201, result)
}

func (a *App) device(w http.ResponseWriter, r *http.Request) {
	item, err := a.store.Device(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "device not found")
		return
	}
	writeJSON(w, 200, item)
}

func (a *App) deleteDevice(w http.ResponseWriter, r *http.Request) {
	item, lookupErr := a.store.Device(r.Context(), r.PathValue("id"))
	if lookupErr != nil {
		problem(w, 404, "device not found")
		return
	}
	if err := a.store.DeleteDevice(r.Context(), item.ID); err != nil {
		problem(w, 409, err.Error())
		return
	}
	if strings.HasPrefix(item.SecretRef, "device:") {
		if err := a.deviceSecrets.delete(item.SecretRef); err != nil {
			problem(w, 500, "device was deleted but its local password could not be removed")
			return
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

func validLineSpecRequest(spec central.LineSpec) error {
	if !safeID.MatchString(spec.LineID) || !safeID.MatchString(spec.ResourceGroup) || (spec.InstanceID != "" && !safeID.MatchString(spec.InstanceID)) {
		return errors.New("invalid line deployment identity")
	}
	if spec.BandwidthMbps < 1 || spec.BandwidthMbps > 1000 || spec.UpstreamMbps < 1 || spec.UpstreamMbps > 1000 ||
		spec.DownstreamMbps < 1 || spec.DownstreamMbps > 1000 || spec.SocksPort < 0 || spec.SocksPort > 65535 || spec.RelayPort < 0 || spec.RelayPort > 65534 || spec.ExitPort < 0 || spec.ExitPort > 65534 {
		return errors.New("invalid line ports or bandwidth")
	}
	if spec.ExitBindIP != "" {
		parsed := net.ParseIP(spec.ExitBindIP)
		if parsed == nil || parsed.To4() == nil {
			return errors.New("exit_bind_ip must be an IPv4 address")
		}
	}
	if (spec.UDPPortMin == 0) != (spec.UDPPortMax == 0) || spec.UDPPortMin < 0 || spec.UDPPortMax > 65535 || (spec.UDPPortMin != 0 && (spec.UDPPortMin < 1024 || spec.UDPPortMin > spec.UDPPortMax)) {
		return errors.New("invalid UDP relay range")
	}
	if spec.BuildMode != "auto" && spec.BuildMode != "binary" && spec.BuildMode != "source" {
		return errors.New("invalid build mode")
	}
	if spec.JumpPolicy != "auto" && spec.JumpPolicy != "direct" && spec.JumpPolicy != "pinned" {
		return errors.New("invalid jump policy")
	}
	if spec.SRSRef != "" && !validWhitelistSource(spec.SRSRef) {
		return errors.New("whitelist update URL must use HTTPS")
	}
	if spec.ArtifactRef != "" && filepath.ToSlash(spec.ArtifactRef) != "build/nb_node" {
		return errors.New("artifact_ref must use build/nb_node")
	}
	if spec.SourceRef != "" && spec.SourceRef != "repo://current" {
		return errors.New("source_ref must use repo://current")
	}
	if len(spec.Whitelist) > 64<<10 || (len(spec.Whitelist) > 0 && !json.Valid(spec.Whitelist)) {
		return errors.New("invalid whitelist")
	}
	roles := map[string]int{}
	seen := map[string]bool{}
	for _, node := range spec.Nodes {
		key := node.Role + ":" + strconv.Itoa(node.Ordinal)
		if !safeID.MatchString(node.DeviceID) || (node.Role != "entry" && node.Role != "relay" && node.Role != "exit") || node.Ordinal < 0 || seen[key] {
			return errors.New("invalid or duplicate line node")
		}
		seen[key] = true
		roles[node.Role]++
		if len(node.JumpCandidates) > 16<<10 || (len(node.JumpCandidates) > 0 && !json.Valid(node.JumpCandidates)) {
			return errors.New("invalid jump candidates")
		}
	}
	if roles["entry"] != 1 || roles["exit"] != 1 {
		return errors.New("line requires exactly one entry and one exit")
	}
	return nil
}

func validLineSpec(spec central.LineSpec) error {
	if err := validLineSpecRequest(spec); err != nil {
		return err
	}
	if spec.SocksPort == 0 || spec.RelayPort == 0 || spec.ExitPort == 0 || spec.UDPPortMin == 0 || spec.UDPPortMax == 0 {
		return errors.New("internal line resources were not allocated")
	}
	return nil
}

func (a *App) saveLineSpec(w http.ResponseWriter, r *http.Request) {
	a.lineMu.Lock()
	defer a.lineMu.Unlock()
	var spec central.LineSpec
	if !decode(w, r, &spec) {
		return
	}
	spec.LineID = r.PathValue("id")
	spec.ExitBindIP = strings.TrimSpace(spec.ExitBindIP)
	spec.NormalizeRates()
	if _, err := a.store.Line(r.Context(), spec.LineID); err != nil {
		problem(w, 404, "line not found")
		return
	}
	if err := validLineSpecRequest(spec); err != nil {
		problem(w, 400, err.Error())
		return
	}
	for _, node := range spec.Nodes {
		if _, err := a.store.Device(r.Context(), node.DeviceID); err != nil {
			problem(w, 400, "line references unknown device: "+node.DeviceID)
			return
		}
	}
	allocated, err := a.store.AllocateLineSpec(r.Context(), spec)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	spec = allocated
	if err := validLineSpec(spec); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if conflict, err := a.store.LineSpecConflict(r.Context(), spec); err != nil {
		problem(w, 500, err.Error())
		return
	} else if conflict != "" {
		problem(w, 409, conflict)
		return
	}
	result, err := a.store.SaveLineSpec(r.Context(), spec)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 200, result)
}

func (a *App) lineSpec(w http.ResponseWriter, r *http.Request) {
	item, err := a.store.LineSpec(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "line specification not found")
		return
	}
	writeJSON(w, 200, item)
}

func (a *App) agentLinePlans(w http.ResponseWriter, r *http.Request) {
	plans, err := a.store.ActiveLineSpecs(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"plans": plans})
}

func (a *App) lineDetail(w http.ResponseWriter, r *http.Request) {
	line, err := a.store.Line(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "line not found")
		return
	}
	spec, specErr := a.store.LineSpec(r.Context(), line.ID)
	snapshots, snapErr := a.store.LatestSnapshots(r.Context(), line.ID)
	if snapErr != nil {
		problem(w, 500, snapErr.Error())
		return
	}
	result := map[string]any{"line": line, "snapshots": snapshots}
	if operation, operationErr := a.store.LatestSuccessfulOperation(r.Context(), line.ID, "line.open"); operationErr == nil {
		if _, clientErr := operationClientURL(operation); clientErr == nil {
			result["client_operation"] = operation
		}
	} else if !errors.Is(operationErr, sql.ErrNoRows) {
		problem(w, 500, operationErr.Error())
		return
	}
	if specErr == nil {
		result["spec"] = spec
	} else if !errors.Is(specErr, sql.ErrNoRows) {
		problem(w, 500, specErr.Error())
		return
	}
	writeJSON(w, 200, result)
}

func (a *App) discoverInventory(w http.ResponseWriter, r *http.Request) {
	var discovery inventoryDiscovery
	if !decode(w, r, &discovery) {
		return
	}
	if !safeID.MatchString(discovery.WorkerID) || len(discovery.Lines) > 100 {
		problem(w, 400, "invalid inventory identity or size")
		return
	}
	type outcome struct {
		LineID  string `json:"line_id"`
		Status  string `json:"status"`
		Message string `json:"message,omitempty"`
	}
	results := make([]outcome, 0, len(discovery.Lines))
	for _, found := range discovery.Lines {
		result := outcome{LineID: found.Line.ID, Status: "unchanged"}
		if found.Line.ID == "" || found.Spec.LineID != found.Line.ID || validLine(found.Line) != nil {
			result.Status, result.Message = "rejected", "invalid discovered line"
			results = append(results, result)
			continue
		}
		if _, err := a.store.Line(r.Context(), found.Line.ID); errors.Is(err, sql.ErrNoRows) {
			if _, err = a.store.UpsertLine(r.Context(), found.Line); err != nil {
				result.Status, result.Message = "rejected", err.Error()
				results = append(results, result)
				continue
			}
			result.Status = "created"
		} else if err != nil {
			result.Status, result.Message = "rejected", err.Error()
			results = append(results, result)
			continue
		}
		valid := true
		for _, device := range found.Devices {
			if err := validDevice(device); err != nil {
				result.Status, result.Message, valid = "rejected", err.Error(), false
				break
			}
			if _, _, err := a.store.EnsureDevice(r.Context(), device); err != nil {
				result.Status, result.Message, valid = "rejected", err.Error(), false
				break
			}
		}
		if !valid {
			results = append(results, result)
			continue
		}
		if _, err := a.store.LineSpec(r.Context(), found.Line.ID); err == nil {
			results = append(results, result)
			continue
		} else if !errors.Is(err, sql.ErrNoRows) {
			result.Status, result.Message = "rejected", err.Error()
			results = append(results, result)
			continue
		}
		found.Spec.NormalizeRates()
		if err := validLineSpec(found.Spec); err != nil {
			result.Status, result.Message = "rejected", err.Error()
		} else if conflict, err := a.store.LineSpecConflict(r.Context(), found.Spec); err != nil {
			result.Status, result.Message = "rejected", err.Error()
		} else if conflict != "" {
			result.Status, result.Message = "rejected", conflict
		} else if _, err = a.store.SaveLineSpec(r.Context(), found.Spec); err != nil {
			result.Status, result.Message = "rejected", err.Error()
		} else {
			result.Status = "discovered"
		}
		results = append(results, result)
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"results": results})
}

func (a *App) operationDetail(w http.ResponseWriter, r *http.Request) {
	operation, err := a.store.Operation(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "operation not found")
		return
	}
	events, err := a.store.OperationEvents(r.Context(), operation.ID)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"operation": operation, "events": events})
}

func (a *App) operationEvent(w http.ResponseWriter, r *http.Request) {
	var event central.OperationEvent
	if !decode(w, r, &event) {
		return
	}
	event.OperationID = r.PathValue("id")
	if event.Sequence < 1 || !safeID.MatchString(event.Stage) || (event.Status != "pending" && event.Status != "running" && event.Status != "succeeded" && event.Status != "failed" && event.Status != "skipped") || len(event.Message) > 1000 {
		problem(w, 400, "invalid operation event")
		return
	}
	if err := a.store.RecordOperationEvent(r.Context(), event); err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 202, map[string]string{"status": "accepted"})
}
