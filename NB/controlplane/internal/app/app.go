package app

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"nb-controlplane/internal/store"
)

const maxBody = 1 << 20

var safeID = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,64}$`)

type Config struct {
	Listen        string
	APIToken      string
	WebBaseURL    string
	WebToken      string
	WebSigningKey string
	UsersFile     string
	TenantsFile   string
	ReloadPIDFile string
	HTTPClient    *http.Client
	ControlGlob   string
	LineID        string
	CollectEvery  time.Duration
}

type App struct {
	store          *store.Store
	cfg            Config
	client         *http.Client
	delivered      atomic.Uint64
	deliveryErrors atomic.Uint64
	lastDelivery   atomic.Int64
}

func New(s *store.Store, cfg Config) *App {
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{Timeout: 10 * time.Second}
	}
	return &App{store: s, cfg: cfg, client: client}
}

func (a *App) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", a.health)
	mux.HandleFunc("GET /readyz", a.ready)
	mux.HandleFunc("GET /metrics", a.metrics)
	mux.HandleFunc("GET /v1/users", a.authorize(a.listUsers))
	mux.HandleFunc("POST /v1/users", a.authorize(a.createUser))
	mux.HandleFunc("GET /v1/users/{id}", a.authorize(a.getUser))
	mux.HandleFunc("PATCH /v1/users/{id}", a.authorize(a.patchUser))
	mux.HandleFunc("POST /v1/usage-events", a.authorize(a.usage))
	mux.HandleFunc("POST /v1/node-snapshots", a.authorize(a.snapshot))
	mux.HandleFunc("POST /v1/incidents", a.authorize(a.incident))
	mux.HandleFunc("GET /v1/usage", a.authorize(a.usageTotals))
	mux.HandleFunc("POST /v1/billing-periods", a.authorize(a.closeBillingPeriod))
	mux.HandleFunc("GET /v1/billing-periods/{id}", a.authorize(a.getBillingPeriod))
	return requestLog(mux)
}

func requestLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("method=%s path=%s elapsed_ms=%.1f", r.Method, r.URL.Path, float64(time.Since(started).Microseconds())/1000)
	})
}

func (a *App) authorize(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if a.cfg.APIToken == "" {
			problem(w, http.StatusServiceUnavailable, "API token is not configured")
			return
		}
		provided := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		if len(provided) != len(a.cfg.APIToken) || subtle.ConstantTimeCompare([]byte(provided), []byte(a.cfg.APIToken)) != 1 {
			problem(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		next(w, r)
	}
}

func decode(w http.ResponseWriter, r *http.Request, out any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, maxBody)
	d := json.NewDecoder(r.Body)
	d.DisallowUnknownFields()
	if err := d.Decode(out); err != nil {
		problem(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return false
	}
	if err := d.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		problem(w, http.StatusBadRequest, "request must contain one JSON value")
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
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}
func (a *App) ready(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), time.Second)
	defer cancel()
	if err := a.store.Ping(ctx); err != nil {
		problem(w, 503, "database unavailable")
		return
	}
	writeJSON(w, 200, map[string]string{"status": "ready"})
}

func (a *App) metrics(w http.ResponseWriter, r *http.Request) {
	pending, failed, users, usage, err := a.store.Stats(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	w.Header().Set("Content-Type", "text/plain; version=0.0.4")
	label := fmt.Sprintf("{line=%q}", a.cfg.LineID)
	fmt.Fprintf(w, "nb_control_outbox_pending%s %d\nnb_control_outbox_failed%s %d\nnb_control_active_users%s %d\nnb_control_usage_events_total%s %d\nnb_control_web_delivered_total%s %d\nnb_control_web_delivery_errors_total%s %d\nnb_control_web_last_delivery_unixtime%s %d\n",
		label, pending, label, failed, label, users, label, usage, label, a.delivered.Load(),
		label, a.deliveryErrors.Load(), label, a.lastDelivery.Load())
}

type userRequest struct {
	ID           string `json:"id"`
	Username     string `json:"username"`
	Status       string `json:"status"`
	Plan         string `json:"plan"`
	Route        string `json:"route"`
	MaxTCP       int64  `json:"max_tcp"`
	MaxUDP       int64  `json:"max_udp"`
	RateKbps     int64  `json:"rate_kbps"`
	QuotaMB      int64  `json:"quota_mb"`
	CredentialID string `json:"credential_id"`
	Password     string `json:"password"`
}

func validateUser(u userRequest) error {
	if !safeID.MatchString(u.ID) || !safeID.MatchString(u.Username) || !safeID.MatchString(u.Plan) || !safeID.MatchString(u.Route) {
		return errors.New("id, username, plan and route must be safe identifiers")
	}
	if u.Status != "active" && u.Status != "disabled" {
		return errors.New("status must be active or disabled")
	}
	if u.MaxTCP < 0 || u.MaxTCP > 100000 || u.MaxUDP < 0 || u.MaxUDP > 100000 || u.RateKbps < 0 || u.RateKbps > 100000000 || u.QuotaMB < 0 {
		return errors.New("quota values are out of range")
	}
	if u.Password != "" && (len(u.Password) < 16 || len(u.Password) > 255) {
		return errors.New("password must contain 16..255 bytes")
	}
	return nil
}

func toUser(u userRequest) store.User {
	return store.User{ID: u.ID, Username: u.Username, Status: u.Status, Plan: u.Plan, Route: u.Route, MaxTCP: u.MaxTCP, MaxUDP: u.MaxUDP, RateKbps: u.RateKbps, QuotaMB: u.QuotaMB, Credential: u.CredentialID}
}

func operationID(r *http.Request) (string, error) {
	id := r.Header.Get("Idempotency-Key")
	if !safeID.MatchString(id) {
		return "", errors.New("valid Idempotency-Key is required")
	}
	return id, nil
}

func requestHash(action string, req userRequest) string {
	password := sha256.Sum256([]byte(req.Password))
	req.Password = hex.EncodeToString(password[:])
	payload, _ := json.Marshal(struct {
		Action  string      `json:"action"`
		Request userRequest `json:"request"`
	}{action, req})
	digest := sha256.Sum256(payload)
	return hex.EncodeToString(digest[:])
}

func valueHash(action string, value any) string {
	payload, _ := json.Marshal(struct {
		Action string `json:"action"`
		Value  any    `json:"value"`
	}{action, value})
	digest := sha256.Sum256(payload)
	return hex.EncodeToString(digest[:])
}

func newCredential(credentialID, password string) (*store.Credential, error) {
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return nil, err
	}
	if credentialID == "" {
		random := make([]byte, 12)
		if _, err := rand.Read(random); err != nil {
			return nil, err
		}
		credentialID = "cred-" + hex.EncodeToString(random)
	}
	const iterations = 210000
	hash := pbkdf2SHA256([]byte(password), salt, iterations, 32)
	return &store.Credential{ID: credentialID, Iterations: iterations,
		Salt: hex.EncodeToString(salt), Hash: hex.EncodeToString(hash)}, nil
}

func (a *App) createUser(w http.ResponseWriter, r *http.Request) {
	op, err := operationID(r)
	if err != nil {
		problem(w, 400, err.Error())
		return
	}
	var req userRequest
	if !decode(w, r, &req) {
		return
	}
	if err = validateUser(req); err != nil {
		problem(w, 400, err.Error())
		return
	}
	if req.Password == "" {
		problem(w, 400, "password is required when creating a user")
		return
	}
	credential, err := newCredential(req.CredentialID, req.Password)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	_, _, err = a.store.UpsertUser(r.Context(), op, requestHash("user.create", req), "web", "user.create", toUser(req), credential)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	if err = a.Reconcile(r.Context()); err != nil {
		problem(w, 500, err.Error())
		return
	}
	u, _ := a.store.User(r.Context(), req.ID)
	writeJSON(w, 201, u)
}

func (a *App) patchUser(w http.ResponseWriter, r *http.Request) {
	op, err := operationID(r)
	if err != nil {
		problem(w, 400, err.Error())
		return
	}
	current, err := a.store.User(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "user not found")
		return
	}
	var patch map[string]json.RawMessage
	if !decode(w, r, &patch) {
		return
	}
	req := userRequest{ID: current.ID, Username: current.Username, Status: current.Status, Plan: current.Plan, Route: current.Route, MaxTCP: current.MaxTCP, MaxUDP: current.MaxUDP, RateKbps: current.RateKbps, QuotaMB: current.QuotaMB, CredentialID: current.Credential}
	fields := map[string]any{"username": &req.Username, "status": &req.Status, "plan": &req.Plan, "route": &req.Route, "max_tcp": &req.MaxTCP, "max_udp": &req.MaxUDP, "rate_kbps": &req.RateKbps, "quota_mb": &req.QuotaMB, "credential_id": &req.CredentialID, "password": &req.Password}
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
	if err = validateUser(req); err != nil {
		problem(w, 400, err.Error())
		return
	}
	var credential *store.Credential
	if req.Password != "" {
		credential, err = newCredential(req.CredentialID, req.Password)
		if err != nil {
			problem(w, 500, err.Error())
			return
		}
	}
	_, _, err = a.store.UpsertUser(r.Context(), op, requestHash("user.update", req), "web", "user.update", toUser(req), credential)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	err = a.Reconcile(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	u, _ := a.store.User(r.Context(), req.ID)
	writeJSON(w, 200, u)
}

func (a *App) getUser(w http.ResponseWriter, r *http.Request) {
	u, err := a.store.User(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "user not found")
		return
	}
	writeJSON(w, 200, u)
}
func (a *App) listUsers(w http.ResponseWriter, r *http.Request) {
	users, err := a.store.Users(r.Context())
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"users": users})
}

func (a *App) usage(w http.ResponseWriter, r *http.Request) {
	var event store.UsageEvent
	if !decode(w, r, &event) {
		return
	}
	if !safeID.MatchString(event.EventID) || !safeID.MatchString(event.NodeID) || !safeID.MatchString(event.WorkerID) || !safeID.MatchString(event.BootID) || !safeID.MatchString(event.Tenant) || event.Sequence < 0 || event.BytesUp < 0 || event.BytesDown < 0 {
		problem(w, 400, "invalid usage event")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, event.Observed); err != nil {
		problem(w, 400, "invalid observed_at")
		return
	}
	inserted, err := a.store.RecordUsage(r.Context(), event)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func rawObject(w http.ResponseWriter, r *http.Request) (map[string]any, []byte, bool) {
	var object map[string]any
	if !decode(w, r, &object) {
		return nil, nil, false
	}
	payload, err := json.Marshal(object)
	if err != nil {
		problem(w, 400, err.Error())
		return nil, nil, false
	}
	return object, payload, true
}
func textField(m map[string]any, key string) (string, bool) {
	v, ok := m[key].(string)
	return v, ok && safeID.MatchString(v)
}

func (a *App) snapshot(w http.ResponseWriter, r *http.Request) {
	m, payload, ok := rawObject(w, r)
	if !ok {
		return
	}
	node, ok := textField(m, "node_id")
	if !ok {
		problem(w, 400, "invalid node_id")
		return
	}
	observed, ok := m["observed_at"].(string)
	if !ok {
		problem(w, 400, "invalid observed_at")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, observed); err != nil {
		problem(w, 400, "invalid observed_at")
		return
	}
	key := r.Header.Get("Idempotency-Key")
	if !safeID.MatchString(key) {
		problem(w, 400, "valid Idempotency-Key is required")
		return
	}
	inserted, err := a.store.RecordSnapshot(r.Context(), key, node, observed, payload)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func (a *App) incident(w http.ResponseWriter, r *http.Request) {
	m, payload, ok := rawObject(w, r)
	if !ok {
		return
	}
	id, idOK := textField(m, "incident_id")
	severity, severityOK := textField(m, "severity")
	status, statusOK := textField(m, "status")
	if !idOK || !severityOK || !statusOK {
		problem(w, 400, "invalid incident")
		return
	}
	inserted, err := a.store.RecordIncident(r.Context(), id, severity, status, payload)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]bool{"inserted": inserted})
}

func (a *App) usageTotals(w http.ResponseWriter, r *http.Request) {
	tenant := r.URL.Query().Get("tenant")
	from := r.URL.Query().Get("from")
	to := r.URL.Query().Get("to")
	if !safeID.MatchString(tenant) {
		problem(w, 400, "invalid tenant")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, from); err != nil {
		problem(w, 400, "invalid from")
		return
	}
	if _, err := time.Parse(time.RFC3339Nano, to); err != nil {
		problem(w, 400, "invalid to")
		return
	}
	up, down, err := a.store.UsageTotals(r.Context(), tenant, from, to)
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"tenant": tenant, "from": from, "to": to, "bytes_up": up, "bytes_down": down, "bytes_total": up + down})
}

type billingRequest struct {
	ID   string `json:"id"`
	From string `json:"from"`
	To   string `json:"to"`
}

func (a *App) closeBillingPeriod(w http.ResponseWriter, r *http.Request) {
	op, err := operationID(r)
	if err != nil {
		problem(w, 400, err.Error())
		return
	}
	var req billingRequest
	if !decode(w, r, &req) {
		return
	}
	from, fromErr := time.Parse(time.RFC3339Nano, req.From)
	to, toErr := time.Parse(time.RFC3339Nano, req.To)
	if !safeID.MatchString(req.ID) || fromErr != nil || toErr != nil || !from.Before(to) || to.After(time.Now().Add(5*time.Minute)) {
		problem(w, 400, "invalid billing period")
		return
	}
	period, _, err := a.store.CloseBillingPeriod(r.Context(), op, valueHash("billing.close", req), req.ID, req.From, req.To)
	if err != nil {
		problem(w, 409, err.Error())
		return
	}
	writeJSON(w, 201, period)
}

func (a *App) getBillingPeriod(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !safeID.MatchString(id) {
		problem(w, 400, "invalid billing period id")
		return
	}
	period, err := a.store.BillingPeriod(r.Context(), id)
	if err != nil {
		problem(w, 404, "billing period not found")
		return
	}
	writeJSON(w, 200, period)
}

func pbkdf2SHA256(password, salt []byte, iterations, length int) []byte {
	result := make([]byte, 0, length)
	for block := uint32(1); len(result) < length; block++ {
		mac := hmac.New(sha256.New, password)
		mac.Write(salt)
		mac.Write([]byte{byte(block >> 24), byte(block >> 16), byte(block >> 8), byte(block)})
		u := mac.Sum(nil)
		t := append([]byte(nil), u...)
		for i := 1; i < iterations; i++ {
			mac = hmac.New(sha256.New, password)
			mac.Write(u)
			u = mac.Sum(nil)
			for j := range t {
				t[j] ^= u[j]
			}
		}
		result = append(result, t...)
	}
	return result[:length]
}

func atomicWrite(path string, data []byte, mode os.FileMode) error {
	if path == "" {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".nb-control-")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if err = tmp.Chmod(mode); err == nil {
		_, err = tmp.Write(data)
	}
	if closeErr := tmp.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(name, path)
}

func (a *App) Reconcile(ctx context.Context) error {
	users, err := a.store.Users(ctx)
	if err != nil {
		return err
	}
	auth, err := a.store.AuthRecords(ctx)
	if err != nil {
		return err
	}
	var tenants, credentials strings.Builder
	tenants.WriteString("# generated by nb-control; do not edit\n")
	credentials.WriteString("# generated by nb-control; do not edit\n")
	for _, u := range users {
		if u.Status == "active" {
			fmt.Fprintf(&tenants, "tenant %s %d %d %d %d\n", u.Username, u.MaxTCP, u.MaxUDP, u.RateKbps, u.QuotaMB)
		}
	}
	for _, record := range auth {
		fmt.Fprintf(&credentials, "%s:%d:%s:%s\n", record.Username, record.Iterations, record.Salt, record.Hash)
	}
	if err = atomicWrite(a.cfg.TenantsFile, []byte(tenants.String()), 0600); err != nil {
		return err
	}
	if err = atomicWrite(a.cfg.UsersFile, []byte(credentials.String()), 0600); err != nil {
		return err
	}
	if a.cfg.ReloadPIDFile != "" {
		data, readErr := os.ReadFile(a.cfg.ReloadPIDFile)
		if readErr != nil {
			return readErr
		}
		pid, parseErr := strconv.Atoi(strings.TrimSpace(string(data)))
		if parseErr != nil || pid <= 1 {
			return errors.New("invalid supervisor PID")
		}
		process, findErr := os.FindProcess(pid)
		if findErr != nil {
			return findErr
		}
		if signalErr := process.Signal(reloadSignal()); signalErr != nil {
			return signalErr
		}
	}
	return nil
}

func (a *App) RunDispatcher(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			a.dispatch(ctx)
		}
	}
}

func (a *App) dispatch(ctx context.Context) {
	if a.cfg.WebBaseURL == "" {
		return
	}
	items, err := a.store.ReadyOutbox(ctx, 50)
	if err != nil {
		a.deliveryErrors.Add(1)
		return
	}
	for _, item := range items {
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimRight(a.cfg.WebBaseURL, "/")+item.Path, strings.NewReader(string(item.Payload)))
		if err == nil {
			req.Header.Set("Content-Type", "application/json")
			req.Header.Set("Idempotency-Key", item.IdempotencyKey)
			if a.cfg.WebToken != "" {
				req.Header.Set("Authorization", "Bearer "+a.cfg.WebToken)
			}
			if a.cfg.WebSigningKey != "" {
				mac := hmac.New(sha256.New, []byte(a.cfg.WebSigningKey))
				mac.Write(item.Payload)
				req.Header.Set("X-NB-Signature", "sha256="+hex.EncodeToString(mac.Sum(nil)))
			}
			var response *http.Response
			response, err = a.client.Do(req)
			if err == nil {
				io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
				response.Body.Close()
				if response.StatusCode < 200 || response.StatusCode >= 300 {
					err = fmt.Errorf("web status %d", response.StatusCode)
				}
			}
		}
		if err == nil {
			_ = a.store.MarkDelivered(ctx, item.ID)
			a.delivered.Add(1)
			a.lastDelivery.Store(time.Now().Unix())
		} else {
			_ = a.store.MarkFailed(ctx, item.ID, item.Attempts+1, err.Error())
			a.deliveryErrors.Add(1)
		}
	}
}
