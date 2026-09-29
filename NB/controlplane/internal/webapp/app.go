package webapp

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"database/sql"
	"embed"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"log"
	"math"
	"net/http"
	"net/url"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	qrcode "github.com/skip2/go-qrcode"
	"golang.org/x/crypto/ssh"
	"nb-controlplane/internal/central"
	"nb-controlplane/internal/transportprofile"
)

const maxBody = 1 << 20

var (
	safeID        = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,64}$`)
	safeSecretRef = regexp.MustCompile(`^[A-Za-z0-9_./:@-]{1,160}$`)
	safeEventKey  = regexp.MustCompile(`^[A-Za-z0-9_.:@-]{1,160}$`)
)

func (a *App) planTransportProfile(r *http.Request, lineID string, bandwidthMbps int) (transportprofile.Profile, error) {
	validation, err := a.store.LatestSuccessfulOperation(r.Context(), lineID, "line.validate")
	if err != nil {
		return transportprofile.Profile{}, errors.New("successful line validation evidence is required before tuning")
	}
	var validationResult struct {
		Evidence json.RawMessage `json:"evidence"`
	}
	if json.Unmarshal(validation.Result, &validationResult) != nil || len(validationResult.Evidence) == 0 {
		return transportprofile.Profile{}, errors.New("latest line validation has no transport evidence")
	}
	var probe transportprofile.Probe
	if json.Unmarshal(validationResult.Evidence, &probe) != nil {
		return transportprofile.Profile{}, errors.New("latest line validation evidence is invalid")
	}
	generation, err := a.store.AllocateTransportGeneration(r.Context(), lineID)
	if err != nil {
		return transportprofile.Profile{}, errors.New("transport generation allocation failed")
	}
	return transportprofile.Generate(lineID, generation, float64(bandwidthMbps), probe)
}

//go:embed assets/*
var assets embed.FS

type Config struct {
	AdminToken        string
	AgentToken        string
	DeviceSecretsFile string
}

type App struct {
	store                *central.Store
	cfg                  Config
	operationMu          sync.Mutex
	lineMu               sync.Mutex
	deviceSecrets        deviceSecretStore
	hostKeyTokenKey      [32]byte
	verifySSHCredentials func(context.Context, string, int, string, string, ssh.PublicKey) error
	snapshotHub          *snapshotHub
}

func New(store *central.Store, cfg Config) *App {
	hash, err := defaultAdminPasswordHash()
	if err != nil || store.EnsureInitialUser(context.Background(), initialAdminUsername, hash) != nil {
		panic("failed to initialize administrator account")
	}
	app := &App{store: store, cfg: cfg, deviceSecrets: deviceSecretStore{path: cfg.DeviceSecretsFile},
		verifySSHCredentials: verifySSHPassword, snapshotHub: newSnapshotHub()}
	if _, err := rand.Read(app.hostKeyTokenKey[:]); err != nil {
		panic("failed to initialize SSH host-key confirmation tokens")
	}
	return app
}

func (a *App) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", a.health)
	mux.HandleFunc("GET /readyz", a.ready)
	mux.HandleFunc("POST /api/v1/auth/login", a.login)
	mux.HandleFunc("GET /api/v1/auth/me", a.currentUser)
	mux.HandleFunc("POST /api/v1/auth/password", a.changePassword)
	mux.HandleFunc("GET /api/v1/dashboard", a.admin(a.dashboard))
	mux.HandleFunc("GET /api/v1/topology", a.admin(a.topology))
	mux.HandleFunc("PUT /api/v1/topology/layout", a.admin(a.saveTopologyLayout))
	mux.HandleFunc("DELETE /api/v1/topology/layout", a.admin(a.resetTopologyLayout))
	mux.HandleFunc("GET /api/v1/lines", a.admin(a.lines))
	mux.HandleFunc("POST /api/v1/lines", a.admin(a.upsertLine))
	mux.HandleFunc("GET /api/v1/lines/{id}", a.admin(a.line))
	mux.HandleFunc("PATCH /api/v1/lines/{id}", a.admin(a.patchLine))
	mux.HandleFunc("DELETE /api/v1/lines/{id}", a.admin(a.deleteLine))
	mux.HandleFunc("GET /api/v1/lines/{id}/detail", a.admin(a.lineDetail))
	mux.HandleFunc("GET /api/v1/lines/{id}/traffic", a.admin(a.trafficHistory))
	mux.HandleFunc("GET /api/v1/lines/{id}/traffic/stream", a.admin(a.trafficStream))
	mux.HandleFunc("GET /api/v1/lines/{id}/spec", a.admin(a.lineSpec))
	mux.HandleFunc("PUT /api/v1/lines/{id}/spec", a.admin(a.saveLineSpec))
	mux.HandleFunc("GET /api/v1/devices", a.admin(a.devices))
	mux.HandleFunc("GET /api/v1/devices/export", a.admin(a.exportDevices))
	mux.HandleFunc("POST /api/v1/devices/host-key/scan", a.admin(a.scanDeviceHostKey))
	mux.HandleFunc("POST /api/v1/devices", a.admin(a.upsertDevice))
	mux.HandleFunc("GET /api/v1/devices/{id}", a.admin(a.device))
	mux.HandleFunc("DELETE /api/v1/devices/{id}", a.admin(a.deleteDevice))
	mux.HandleFunc("POST /api/v1/devices/{id}/probe", a.admin(a.probeDevice))
	mux.HandleFunc("GET /api/v1/incidents", a.admin(a.incidents))
	mux.HandleFunc("GET /api/v1/operations", a.admin(a.operations))
	mux.HandleFunc("POST /api/v1/operations", a.admin(a.createOperation))
	mux.HandleFunc("DELETE /api/v1/operations", a.admin(a.deleteOperations))
	mux.HandleFunc("GET /api/v1/operations/{id}", a.admin(a.operationDetail))
	mux.HandleFunc("DELETE /api/v1/operations/{id}", a.admin(a.deleteOperation))
	mux.HandleFunc("GET /api/v1/operations/{id}/client-qr", a.admin(a.clientQR))
	mux.HandleFunc("POST /api/v1/operations/{id}/cancel", a.admin(a.cancelOperation))
	mux.HandleFunc("GET /api/v1/executors", a.admin(a.executors))
	mux.HandleFunc("POST /agent/v1/snapshots", a.agent(a.snapshot))
	mux.HandleFunc("POST /agent/v1/incidents", a.agent(a.agentIncident))
	mux.HandleFunc("POST /agent/v1/executors/heartbeat", a.agent(a.executorHeartbeat))
	mux.HandleFunc("POST /agent/v1/inventory", a.agent(a.discoverInventory))
	mux.HandleFunc("GET /agent/v1/line-plans", a.agent(a.agentLinePlans))
	mux.HandleFunc("GET /agent/v1/runtime-port-scan-plans", a.agent(a.runtimePortScanPlans))
	mux.HandleFunc("POST /agent/v1/runtime-port-claims", a.agent(a.replaceRuntimePortClaims))
	mux.HandleFunc("POST /agent/v1/lines/{id}/client-config", a.agent(a.attachClientConfig))
	mux.HandleFunc("GET /agent/v1/operations", a.agent(a.claimOperations))
	mux.HandleFunc("POST /agent/v1/operations/{id}/port-preflight", a.agent(a.prepareOperationPorts))
	mux.HandleFunc("POST /agent/v1/operations/{id}/result", a.agent(a.completeOperation))
	mux.HandleFunc("POST /agent/v1/operations/{id}/events", a.agent(a.operationEvent))
	mux.HandleFunc("POST /api/nb/v1/node-snapshots", a.agent(a.legacySnapshot))
	mux.HandleFunc("POST /api/nb/v1/incidents", a.agent(a.legacyIncident))
	mux.HandleFunc("POST /api/nb/v1/provision-results", a.agent(a.rawEvent))
	mux.HandleFunc("POST /api/nb/v1/usage-events", a.agent(a.rawEvent))
	mux.HandleFunc("POST /api/nb/v1/billing-periods", a.agent(a.rawEvent))
	static, _ := fs.Sub(assets, "assets")
	mux.Handle("/", http.FileServer(http.FS(static)))
	return requestLog(securityHeaders(mux))
}

func requestLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("component=nb-web method=%s path=%s elapsed_ms=%.1f", r.Method, r.URL.Path, float64(time.Since(started).Microseconds())/1000)
	})
}

func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'")
		next.ServeHTTP(w, r)
	})
}

func bearer(r *http.Request) string {
	return strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
}

func authorized(provided, expected string) bool {
	return expected != "" && len(provided) == len(expected) && subtle.ConstantTimeCompare([]byte(provided), []byte(expected)) == 1
}

func (a *App) admin(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(bearer(r), a.cfg.AdminToken) {
			user, _, err := a.authenticateSession(r.Context(), r)
			if err != nil {
				problem(w, http.StatusUnauthorized, "登录会话无效或已过期")
				return
			}
			if user.MustChangePassword {
				problem(w, http.StatusForbidden, "首次登录必须修改管理员密码")
				return
			}
		}
		next(w, r)
	}
}

func (a *App) agent(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(bearer(r), a.cfg.AgentToken) {
			problem(w, http.StatusUnauthorized, "agent authorization required")
			return
		}
		next(w, r)
	}
}

func decode(w http.ResponseWriter, r *http.Request, out any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, maxBody)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(out); err != nil {
		problem(w, 400, "invalid JSON: "+err.Error())
		return false
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		problem(w, 400, "request must contain one JSON value")
		return false
	}
	return true
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func problem(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]any{"error": message, "status": status})
}

func (a *App) health(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, 200, map[string]string{"status": "ok"})
}
func (a *App) ready(w http.ResponseWriter, r *http.Request) {
	if err := a.store.Ping(r.Context()); err != nil {
		problem(w, 503, "database unavailable")
		return
	}
	writeJSON(w, 200, map[string]string{"status": "ready"})
}

func (a *App) dashboard(w http.ResponseWriter, r *http.Request) {
	view, err := a.store.Dashboard(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, view)
}

func (a *App) topology(w http.ResponseWriter, r *http.Request) {
	result, err := a.store.Topology(r.Context())
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (a *App) saveTopologyLayout(w http.ResponseWriter, r *http.Request) {
	var request struct {
		UpdatedBy string                   `json:"updated_by"`
		Layouts   []central.TopologyLayout `json:"layouts"`
	}
	if !decode(w, r, &request) {
		return
	}
	if !safeID.MatchString(request.UpdatedBy) || len(request.Layouts) == 0 || len(request.Layouts) > 1000 {
		problem(w, http.StatusBadRequest, "布局提交人和设备坐标不能为空")
		return
	}
	seen := make(map[string]bool, len(request.Layouts))
	for index := range request.Layouts {
		item := &request.Layouts[index]
		item.UpdatedBy = request.UpdatedBy
		if !safeID.MatchString(item.DeviceID) || seen[item.DeviceID] {
			problem(w, http.StatusBadRequest, "设备布局包含无效或重复的设备 ID")
			return
		}
		seen[item.DeviceID] = true
		for _, coordinate := range []float64{item.X, item.Y, item.Z} {
			if math.IsNaN(coordinate) || math.IsInf(coordinate, 0) || math.Abs(coordinate) > 10000 {
				problem(w, http.StatusBadRequest, "设备布局坐标无效")
				return
			}
		}
		if _, err := a.store.Device(r.Context(), item.DeviceID); err != nil {
			problem(w, http.StatusBadRequest, "设备布局引用了不存在的设备")
			return
		}
	}
	if err := a.store.SaveTopologyLayouts(r.Context(), request.Layouts); err != nil {
		problem(w, http.StatusConflict, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"layouts": request.Layouts})
}

func (a *App) resetTopologyLayout(w http.ResponseWriter, r *http.Request) {
	if err := a.store.ResetTopologyLayouts(r.Context()); err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *App) lines(w http.ResponseWriter, r *http.Request) {
	lines, err := a.store.Lines(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"lines": lines})
}

func validLine(line central.Line) error {
	if !safeID.MatchString(line.ID) || strings.TrimSpace(line.Name) == "" || len(line.Name) > 100 {
		return errors.New("invalid line identity")
	}
	if line.Status != "draft" && line.Status != "validating" && line.Status != "active" && line.Status != "maintenance" && line.Status != "disabled" && line.Status != "archived" && line.Status != "deleting" {
		return errors.New("invalid line status")
	}
	if line.CapacityMbps < 0 || line.CapacityMbps > 1000000 {
		return errors.New("invalid line capacity")
	}
	if line.SecretRef != "" && !safeSecretRef.MatchString(line.SecretRef) {
		return errors.New("invalid secret reference")
	}
	for _, value := range []string{line.EntryRegion, line.ExitRegion, line.Provider, line.ActiveDeployment, line.Profile} {
		if len(value) > 160 {
			return errors.New("line field is too long")
		}
	}
	return nil
}

func (a *App) upsertLine(w http.ResponseWriter, r *http.Request) {
	a.lineMu.Lock()
	defer a.lineMu.Unlock()
	var line central.Line
	if !decode(w, r, &line) {
		return
	}
	if err := validLine(line); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if _, err := a.store.Line(r.Context(), line.ID); err == nil {
		problem(w, http.StatusConflict, "line ID already exists")
		return
	} else if !errors.Is(err, sql.ErrNoRows) {
		problem(w, 500, err.Error())
		return
	}
	result, err := a.store.UpsertLine(r.Context(), line)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 201, result)
}

func (a *App) line(w http.ResponseWriter, r *http.Request) {
	line, err := a.store.Line(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "line not found")
		return
	}
	writeJSON(w, 200, line)
}

func (a *App) patchLine(w http.ResponseWriter, r *http.Request) {
	line, err := a.store.Line(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "line not found")
		return
	}
	var patch map[string]json.RawMessage
	if !decode(w, r, &patch) {
		return
	}
	fields := map[string]any{"name": &line.Name, "status": &line.Status, "entry_region": &line.EntryRegion,
		"exit_region": &line.ExitRegion, "provider": &line.Provider, "capacity_mbps": &line.CapacityMbps,
		"active_deployment": &line.ActiveDeployment, "profile": &line.Profile, "secret_ref": &line.SecretRef}
	for key, value := range patch {
		target, ok := fields[key]
		if !ok {
			problem(w, 400, "unsupported field: "+key)
			return
		}
		if err = json.Unmarshal(value, target); err != nil {
			problem(w, 400, "invalid field: "+key)
			return
		}
	}
	if err = validLine(line); err != nil {
		problem(w, 400, err.Error())
		return
	}
	line, err = a.store.UpsertLine(r.Context(), line)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 200, line)
}

func limit(r *http.Request) int {
	value, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if value < 1 || value > 500 {
		return 100
	}
	return value
}

func (a *App) incidents(w http.ResponseWriter, r *http.Request) {
	items, err := a.store.Incidents(r.Context(), limit(r))
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"incidents": items})
}

func (a *App) operations(w http.ResponseWriter, r *http.Request) {
	items, err := a.store.Operations(r.Context(), r.URL.Query().Get("line_id"), limit(r))
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"operations": items})
}

type operationRequest struct {
	ID          string          `json:"id"`
	LineID      string          `json:"line_id"`
	Kind        string          `json:"kind"`
	RequestedBy string          `json:"requested_by"`
	Request     json.RawMessage `json:"request"`
}

func operationID() (string, error) {
	random := make([]byte, 12)
	if _, err := rand.Read(random); err != nil {
		return "", err
	}
	return "op-" + hex.EncodeToString(random), nil
}

func (a *App) createOperation(w http.ResponseWriter, r *http.Request) {
	a.operationMu.Lock()
	defer a.operationMu.Unlock()
	key := r.Header.Get("Idempotency-Key")
	if !safeID.MatchString(key) {
		problem(w, 400, "缺少有效的任务幂等键")
		return
	}
	var req operationRequest
	if !decode(w, r, &req) {
		return
	}
	if req.ID == "" {
		var err error
		req.ID, err = operationID()
		if err != nil {
			problem(w, 500, err.Error())
			return
		}
	}
	allowed := map[string]bool{"line.open": true, "line.validate": true, "line.upgrade": true, "line.rollback": true, "line.disable": true, "line.tune": true}
	if !safeID.MatchString(req.ID) || !safeID.MatchString(req.LineID) || !allowed[req.Kind] || !safeID.MatchString(req.RequestedBy) {
		problem(w, 400, "任务参数无效")
		return
	}
	if existing, existingErr := a.store.OperationByIdempotencyKey(r.Context(), key); existingErr == nil {
		if existing.LineID != req.LineID || existing.Kind != req.Kind || existing.RequestedBy != req.RequestedBy ||
			!sameUserOperationRequest(existing.Request, req.Request) {
			problem(w, http.StatusConflict, "幂等键已用于其他任务")
			return
		}
		writeJSON(w, http.StatusOK, existing)
		return
	} else if !errors.Is(existingErr, sql.ErrNoRows) {
		problem(w, 500, existingErr.Error())
		return
	}
	lineRecord, err := a.store.Line(r.Context(), req.LineID)
	if err != nil {
		problem(w, 404, "线路不存在")
		return
	}
	if spec, specErr := a.store.LineSpec(r.Context(), req.LineID); specErr == nil {
		if specErr = validLineSpec(spec); specErr != nil {
			problem(w, 409, "线路部署参数无效："+specErr.Error())
			return
		}
		var values map[string]any
		if len(req.Request) == 0 || json.Unmarshal(req.Request, &values) != nil {
			values = map[string]any{}
		}
		values["plan"] = spec
		values["deployment_id"] = lineRecord.ActiveDeployment
		if req.Kind == "line.tune" {
			profile, planErr := a.planTransportProfile(r, req.LineID, spec.BandwidthMbps)
			if planErr != nil {
				problem(w, 409, planErr.Error())
				return
			}
			values["transport_profile"] = profile
		}
		req.Request, _ = json.Marshal(values)
	} else if req.Kind == "line.open" {
		problem(w, 409, "开线前必须先保存线路部署参数")
		return
	}
	executors, err := a.store.Executors(r.Context(), 45*time.Second)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	available := false
	exactCapability := false
	for _, executor := range executors {
		for _, line := range executor.Lines {
			if executor.Online && line.LineID == req.LineID {
				exactCapability = true
			}
		}
	}
	for _, executor := range executors {
		for _, line := range executor.Lines {
			if executor.Online && (line.LineID == req.LineID || (!exactCapability && line.LineID == "*")) {
				for _, operation := range line.Operations {
					available = available || operation == req.Kind
				}
			}
		}
	}
	if !available {
		problem(w, 409, "当前没有在线且获得授权的执行器")
		return
	}
	if len(req.Request) == 0 {
		req.Request = json.RawMessage(`{}`)
	}
	operation := central.Operation{ID: req.ID, LineID: req.LineID, Kind: req.Kind, RequestedBy: req.RequestedBy, IdempotencyKey: key, Request: req.Request}
	result, replayed, err := a.store.CreateOperation(r.Context(), operation)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	status := 201
	if replayed {
		status = 200
	}
	writeJSON(w, status, result)
}

func sameUserOperationRequest(existing, requested json.RawMessage) bool {
	decodeRequest := func(raw json.RawMessage) map[string]any {
		values := map[string]any{}
		if len(raw) > 0 {
			_ = json.Unmarshal(raw, &values)
		}
		delete(values, "plan")
		delete(values, "transport_profile")
		return values
	}
	return reflect.DeepEqual(decodeRequest(existing), decodeRequest(requested))
}

func (a *App) cancelOperation(w http.ResponseWriter, r *http.Request) {
	if !safeID.MatchString(r.PathValue("id")) {
		problem(w, 400, "invalid operation id")
		return
	}
	var request struct {
		Reason string `json:"reason"`
	}
	if !decode(w, r, &request) {
		return
	}
	request.Reason = strings.TrimSpace(request.Reason)
	if request.Reason == "" || len(request.Reason) > 300 {
		problem(w, 400, "valid cancellation reason is required")
		return
	}
	if err := a.store.CancelOperation(r.Context(), r.PathValue("id"), request.Reason); err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 200, map[string]string{"status": "cancelled"})
}

func (a *App) executors(w http.ResponseWriter, r *http.Request) {
	items, err := a.store.Executors(r.Context(), 45*time.Second)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"executors": items})
}

func validOperationKind(kind string) bool {
	return kind == "line.open" || kind == "line.validate" || kind == "line.upgrade" ||
		kind == "line.rollback" || kind == "line.disable" || kind == "line.tune"
}

func (a *App) executorHeartbeat(w http.ResponseWriter, r *http.Request) {
	var item central.Executor
	if !decode(w, r, &item) {
		return
	}
	if !safeID.MatchString(item.WorkerID) || item.Status != "ready" || len(item.Version) > 64 {
		problem(w, 400, "invalid executor identity")
		return
	}
	observed, err := time.Parse(time.RFC3339Nano, item.ObservedAt)
	if err != nil || observed.Before(time.Now().UTC().Add(-5*time.Minute)) || observed.After(time.Now().UTC().Add(time.Minute)) {
		problem(w, 400, "invalid executor observed_at")
		return
	}
	if len(item.Lines) > 500 {
		problem(w, 400, "too many executor lines")
		return
	}
	for _, line := range item.Lines {
		if (line.LineID != "*" && !safeID.MatchString(line.LineID)) || len(line.Reason) > 300 || len(line.Operations) > 6 {
			problem(w, 400, "invalid executor line")
			return
		}
		for _, kind := range line.Operations {
			if !validOperationKind(kind) {
				problem(w, 400, "invalid executor operation")
				return
			}
		}
	}
	if err = a.store.RecordExecutor(r.Context(), item); err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]string{"status": "ready"})
}

func (a *App) rawEvent(w http.ResponseWriter, r *http.Request) {
	key := r.Header.Get("Idempotency-Key")
	if !safeEventKey.MatchString(key) {
		problem(w, 400, "valid Idempotency-Key is required")
		return
	}
	var payload json.RawMessage
	if !decode(w, r, &payload) {
		return
	}
	inserted, err := a.store.RecordRawEvent(r.Context(), r.URL.Path, key, payload)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func validSnapshot(item central.Snapshot) error {
	if !safeID.MatchString(item.LineID) || !safeID.MatchString(item.NodeID) || !safeID.MatchString(item.WorkerID) {
		return errors.New("invalid snapshot identity")
	}
	if item.Role != "entry" && item.Role != "middle" && item.Role != "exit" {
		return errors.New("invalid role")
	}
	if item.Health != "ok" && item.Health != "degraded" && item.Health != "down" {
		return errors.New("invalid health")
	}
	if _, err := time.Parse(time.RFC3339Nano, item.ObservedAt); err != nil {
		return errors.New("invalid observed_at")
	}
	if item.Sessions < 0 || item.ThroughputMbps < 0 || item.UpstreamMbps < 0 || item.DownstreamMbps < 0 || item.QueueAgeP95US < 0 || item.EffectiveLoss < 0 || item.EffectiveLoss > 100 {
		return errors.New("invalid metrics")
	}
	return nil
}

func (a *App) snapshot(w http.ResponseWriter, r *http.Request) {
	var item central.Snapshot
	if !decode(w, r, &item) {
		return
	}
	if err := validSnapshot(item); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if _, err := a.store.Line(r.Context(), item.LineID); err != nil {
		problem(w, 404, "line not found")
		return
	}
	inserted, err := a.store.RecordSnapshot(r.Context(), item)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	if inserted {
		a.snapshotHub.publish(item)
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func objectString(parent map[string]any, key string) string {
	value, _ := parent[key].(string)
	return value
}

func objectNumber(parent map[string]any, key string) float64 {
	value, _ := parent[key].(float64)
	return value
}

func objectMap(parent map[string]any, key string) map[string]any {
	value, _ := parent[key].(map[string]any)
	return value
}

func inferRole(nodeID string) string {
	for _, role := range []string{"entry", "middle", "exit"} {
		if strings.Contains(strings.ToLower(nodeID), role) {
			return role
		}
	}
	return "entry"
}

func (a *App) legacySnapshot(w http.ResponseWriter, r *http.Request) {
	var raw map[string]any
	if !decode(w, r, &raw) {
		return
	}
	health, metrics := objectMap(raw, "health"), objectMap(raw, "metrics")
	queue, link, fec := objectMap(metrics, "queue_age_max_us"), objectMap(metrics, "link"), objectMap(metrics, "fec")
	queueAge := max(objectNumber(queue, "down"), objectNumber(queue, "up"), objectNumber(queue, "q2t"))
	nodeID, lineID, observed := objectString(raw, "node_id"), objectString(raw, "line_id"), objectString(raw, "observed_at")
	worker := strings.TrimSpace(strings.TrimSuffix(strings.TrimPrefix(strings.TrimSpace(toText(health["worker"])), "+"), ".0"))
	if worker == "" {
		worker = "0"
	}
	item := central.Snapshot{LineID: lineID, NodeID: nodeID, Role: inferRole(nodeID), WorkerID: worker,
		ObservedAt: observed, Health: objectString(health, "status"), Deployment: objectString(health, "release_id"),
		Profile: objectString(health, "line_profile"), Sessions: int64(objectNumber(metrics, "sessions_inuse")),
		ThroughputMbps: objectNumber(metrics, "throughput_mbps"), QueueAgeP95US: queueAge,
		UpstreamMbps: objectNumber(metrics, "upstream_mbps"), DownstreamMbps: objectNumber(metrics, "downstream_mbps"),
		EffectiveLoss: objectNumber(link, "effective_loss_max_pct"), FECObserve: objectNumber(fec, "observe") != 0,
		FECActive: objectNumber(fec, "active") != 0}
	item.Payload, _ = json.Marshal(raw)
	if err := validSnapshot(item); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if _, err := a.store.Line(r.Context(), lineID); err != nil {
		problem(w, 404, "line not found")
		return
	}
	inserted, err := a.store.RecordSnapshot(r.Context(), item)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func toText(value any) string {
	switch item := value.(type) {
	case string:
		return item
	case float64:
		return strconv.FormatInt(int64(item), 10)
	default:
		return ""
	}
}

func (a *App) agentIncident(w http.ResponseWriter, r *http.Request) {
	var item central.Incident
	if !decode(w, r, &item) {
		return
	}
	if !safeID.MatchString(item.ID) || !safeID.MatchString(item.LineID) || !safeID.MatchString(item.Kind) ||
		(item.Severity != "critical" && item.Severity != "warning" && item.Severity != "info") ||
		(item.Status != "open" && item.Status != "firing" && item.Status != "resolved") {
		problem(w, 400, "invalid incident")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, item.ObservedAt); err != nil {
		problem(w, 400, "invalid observed_at")
		return
	}
	if _, err := a.store.Line(r.Context(), item.LineID); err != nil {
		problem(w, 404, "line not found")
		return
	}
	inserted, err := a.store.RecordIncident(r.Context(), item)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func (a *App) legacyIncident(w http.ResponseWriter, r *http.Request) {
	var raw map[string]any
	if !decode(w, r, &raw) {
		return
	}
	item := central.Incident{ID: objectString(raw, "incident_id"), LineID: objectString(raw, "line_id"),
		Severity: objectString(raw, "severity"), Status: objectString(raw, "status"), Kind: objectString(raw, "kind"),
		Message: objectString(raw, "message"), ObservedAt: objectString(raw, "observed_at")}
	item.Payload, _ = json.Marshal(raw)
	if !safeID.MatchString(item.ID) || !safeID.MatchString(item.LineID) || !safeID.MatchString(item.Kind) ||
		(item.Severity != "critical" && item.Severity != "warning" && item.Severity != "info") ||
		(item.Status != "open" && item.Status != "firing" && item.Status != "resolved") {
		problem(w, 400, "invalid incident")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, item.ObservedAt); err != nil {
		problem(w, 400, "invalid observed_at")
		return
	}
	if _, err := a.store.Line(r.Context(), item.LineID); err != nil {
		problem(w, 404, "line not found")
		return
	}
	inserted, err := a.store.RecordIncident(r.Context(), item)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func (a *App) claimOperations(w http.ResponseWriter, r *http.Request) {
	lineID := r.URL.Query().Get("line_id")
	if lineID != "*" && !safeID.MatchString(lineID) {
		problem(w, 400, "invalid line_id")
		return
	}
	var items []central.Operation
	var err error
	if lineID == "*" {
		executors, executorErr := a.store.Executors(r.Context(), 45*time.Second)
		if executorErr != nil {
			problem(w, 500, executorErr.Error())
			return
		}
		excluded := map[string]bool{}
		for _, executor := range executors {
			if executor.Online {
				for _, capability := range executor.Lines {
					if capability.LineID != "*" {
						excluded[capability.LineID] = true
					}
				}
			}
		}
		exactLines := make([]string, 0, len(excluded))
		for id := range excluded {
			exactLines = append(exactLines, id)
		}
		items, err = a.store.ClaimAnyOperationsExcept(r.Context(), min(limit(r), 20), exactLines)
	} else {
		items, err = a.store.ClaimOperations(r.Context(), lineID, min(limit(r), 20))
	}
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"operations": items})
}

type resultRequest struct {
	LineID string          `json:"line_id"`
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
}

func operationClientURL(operation central.Operation) (string, error) {
	if operation.Kind != "line.open" || operation.Status != "succeeded" || len(operation.Result) == 0 {
		return "", errors.New("client configuration is unavailable")
	}
	var result struct {
		ClientURL string `json:"client_url"`
	}
	if err := json.Unmarshal(operation.Result, &result); err != nil {
		return "", errors.New("client configuration is invalid")
	}
	parsed, err := url.Parse(result.ClientURL)
	password, hasPassword := "", false
	if parsed != nil && parsed.User != nil {
		password, hasPassword = parsed.User.Password()
	}
	if err != nil || parsed == nil || parsed.Scheme != "socks5" || parsed.Host == "" || parsed.User == nil ||
		parsed.User.Username() == "" || !hasPassword || password == "" || len(result.ClientURL) > 4096 {
		return "", errors.New("client configuration is invalid")
	}
	return result.ClientURL, nil
}

func (a *App) clientQR(w http.ResponseWriter, r *http.Request) {
	operation, err := a.store.Operation(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "operation not found")
		return
	}
	clientURL, err := operationClientURL(operation)
	if err != nil {
		problem(w, 404, err.Error())
		return
	}
	png, err := qrcode.Encode(clientURL, qrcode.Medium, 256)
	if err != nil {
		problem(w, 500, "client QR generation failed")
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]string{"media_type": "image/png", "data": base64.StdEncoding.EncodeToString(png)})
}

func (a *App) attachClientConfig(w http.ResponseWriter, r *http.Request) {
	lineID := r.PathValue("id")
	if !safeID.MatchString(lineID) {
		problem(w, 400, "invalid line id")
		return
	}
	var request struct {
		ClientURL string `json:"client_url"`
	}
	if !decode(w, r, &request) {
		return
	}
	encoded, _ := json.Marshal(map[string]string{"client_url": request.ClientURL})
	candidate := central.Operation{Kind: "line.open", Status: "succeeded", Result: encoded}
	if _, err := operationClientURL(candidate); err != nil {
		problem(w, 400, err.Error())
		return
	}
	operationID, err := a.store.AttachClientURL(r.Context(), lineID, request.ClientURL)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			problem(w, 404, "successful line.open operation not found")
		} else {
			problem(w, 500, err.Error())
		}
		return
	}
	writeJSON(w, 200, map[string]string{"operation_id": operationID, "status": "attached"})
}

func (a *App) completeOperation(w http.ResponseWriter, r *http.Request) {
	var req resultRequest
	if !decode(w, r, &req) {
		return
	}
	if !safeID.MatchString(req.LineID) || (req.Status != "succeeded" && req.Status != "failed" && req.Status != "rolled_back") {
		problem(w, 400, "invalid operation result")
		return
	}
	if len(req.Result) == 0 {
		req.Result = json.RawMessage(`{}`)
	}
	operation, err := a.store.Operation(r.Context(), r.PathValue("id"))
	if errors.Is(err, sql.ErrNoRows) && req.Status == "succeeded" {
		if completed, completedErr := a.store.LineDeletionCompleted(r.Context(), r.PathValue("id"), req.LineID); completedErr == nil && completed {
			writeJSON(w, 200, map[string]bool{"updated": true})
			return
		}
	}
	if err != nil || operation.LineID != req.LineID {
		problem(w, 404, "operation not found")
		return
	}
	var resultFields struct {
		ClientURL string `json:"client_url"`
	}
	if json.Unmarshal(req.Result, &resultFields) != nil {
		problem(w, 400, "invalid operation result")
		return
	}
	if resultFields.ClientURL != "" {
		candidate := operation
		candidate.Status, candidate.Result = req.Status, req.Result
		if _, validateErr := operationClientURL(candidate); validateErr != nil {
			problem(w, 400, validateErr.Error())
			return
		}
	}
	if err := a.store.CompleteOperation(r.Context(), r.PathValue("id"), req.LineID, req.Status, req.Result); err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"updated": true})
}
