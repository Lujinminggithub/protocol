package app

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"nb-controlplane/internal/store"
)

func request(t *testing.T, client *http.Client, method, url, token, key string, body any) (*http.Response, []byte) {
	t.Helper()
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			t.Fatal(err)
		}
		reader = bytes.NewReader(data)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	if key != "" {
		req.Header.Set("Idempotency-Key", key)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	response, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(response.Body)
	response.Body.Close()
	if err != nil {
		t.Fatal(err)
	}
	return response, data
}

func TestControlPlanePersistsAndReplays(t *testing.T) {
	dir := t.TempDir()
	db, err := store.Open(filepath.Join(dir, "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var webCalls atomic.Int32
	webReady := atomic.Bool{}
	web := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Idempotency-Key") == "" {
			t.Error("missing idempotency key")
		}
		webCalls.Add(1)
		if !webReady.Load() {
			http.Error(w, "not ready", http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer web.Close()
	service := New(db, Config{APIToken: "test-token", WebBaseURL: web.URL, UsersFile: filepath.Join(dir, "users.conf"), TenantsFile: filepath.Join(dir, "tenants.conf"), HTTPClient: web.Client()})
	server := httptest.NewServer(service.Handler())
	defer server.Close()

	user := map[string]any{"id": "u-1", "username": "alice", "status": "active", "plan": "live-10m", "route": "kz-1", "max_tcp": 16, "max_udp": 8, "rate_kbps": 10000, "quota_mb": 1024, "password": "correct-horse-battery"}
	response, body := request(t, server.Client(), http.MethodPost, server.URL+"/v1/users", "test-token", "op-create-1", user)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create status=%d body=%s", response.StatusCode, body)
	}
	firstBody := append([]byte(nil), body...)
	response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/users", "test-token", "op-create-1", user)
	if response.StatusCode != http.StatusCreated || !bytes.Equal(body, firstBody) {
		t.Fatalf("idempotent replay status=%d first=%s replay=%s", response.StatusCode, firstBody, body)
	}
	changed := make(map[string]any, len(user))
	for key, value := range user {
		changed[key] = value
	}
	changed["plan"] = "other-plan"
	response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/users", "test-token", "op-create-1", changed)
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("idempotency conflict status=%d body=%s", response.StatusCode, body)
	}
	usersFile, err := os.ReadFile(filepath.Join(dir, "users.conf"))
	if err != nil {
		t.Fatal(err)
	}
	parts := strings.Split(strings.TrimSpace(strings.Split(string(usersFile), "\n")[1]), ":")
	if len(parts) != 4 || parts[0] != "alice" || parts[1] != "210000" || len(parts[2]) != 32 || len(parts[3]) != 64 {
		t.Fatalf("invalid auth record: %q", usersFile)
	}
	tenantsFile, err := os.ReadFile(filepath.Join(dir, "tenants.conf"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(tenantsFile), "tenant alice 16 8 10000 1024") {
		t.Fatalf("invalid tenant config: %s", tenantsFile)
	}

	event := map[string]any{"event_id": "node-0-boot-1-1", "node_id": "entry-1", "worker_id": "0", "boot_id": "boot-1", "sequence": 1, "tenant": "alice", "bytes_up": 1200, "bytes_down": 3400, "observed_at": "2026-07-27T12:00:00Z"}
	for i := 0; i < 2; i++ {
		response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/usage-events", "test-token", "", event)
		if response.StatusCode != 200 {
			t.Fatalf("usage status=%d body=%s", response.StatusCode, body)
		}
	}
	response, body = request(t, server.Client(), http.MethodGet, server.URL+"/v1/usage?tenant=alice&from=2026-07-27T00:00:00Z&to=2026-07-28T00:00:00Z", "test-token", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"bytes_total":4600`)) {
		t.Fatalf("totals status=%d body=%s", response.StatusCode, body)
	}
	period := map[string]any{"id": "period-20260726", "from": "2026-07-26T00:00:00Z", "to": "2026-07-27T00:00:00Z"}
	response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/billing-periods", "test-token", "op-close-1", period)
	if response.StatusCode != http.StatusCreated || !bytes.Contains(body, []byte(`"tenant":"alice"`)) {
		t.Fatalf("billing close status=%d body=%s", response.StatusCode, body)
	}
	firstPeriod := append([]byte(nil), body...)
	response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/billing-periods", "test-token", "op-close-1", period)
	if response.StatusCode != http.StatusCreated || !bytes.Equal(body, firstPeriod) {
		t.Fatalf("billing replay status=%d first=%s replay=%s", response.StatusCode, firstPeriod, body)
	}
	period["to"] = "2026-07-27T01:00:00Z"
	response, body = request(t, server.Client(), http.MethodPost, server.URL+"/v1/billing-periods", "test-token", "op-close-1", period)
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("billing idempotency conflict status=%d body=%s", response.StatusCode, body)
	}
	response, body = request(t, server.Client(), http.MethodGet, server.URL+"/v1/billing-periods/period-20260726", "test-token", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Equal(body, firstPeriod) {
		t.Fatalf("billing read status=%d close=%s read=%s", response.StatusCode, firstPeriod, body)
	}

	ctx, cancel := context.WithCancel(context.Background())
	go service.RunDispatcher(ctx)
	time.Sleep(1200 * time.Millisecond)
	pending, failed, _, usageCount, err := db.Stats(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if pending < 2 || failed < 1 || usageCount != 1 {
		t.Fatalf("before recovery pending=%d failed=%d usage=%d calls=%d", pending, failed, usageCount, webCalls.Load())
	}
	webReady.Store(true)
	deadline := time.Now().Add(6 * time.Second)
	for time.Now().Before(deadline) {
		pending, _, _, _, err = db.Stats(context.Background())
		if err != nil {
			t.Fatal(err)
		}
		if pending == 0 {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	cancel()
	pending, failed, _, usageCount, err = db.Stats(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if pending != 0 || failed != 0 || usageCount != 1 {
		t.Fatalf("after recovery pending=%d failed=%d usage=%d calls=%d", pending, failed, usageCount, webCalls.Load())
	}
	response, body = request(t, server.Client(), http.MethodGet, server.URL+"/metrics", "test-token", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`nb_control_outbox_pending{line=""} 0`)) {
		t.Fatalf("metrics status=%d body=%s", response.StatusCode, body)
	}
}

func TestControlPlaneRejectsUnauthenticatedMutation(t *testing.T) {
	db, err := store.Open(filepath.Join(t.TempDir(), "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	server := httptest.NewServer(New(db, Config{APIToken: "secret"}).Handler())
	defer server.Close()
	response, err := http.Post(server.URL+"/v1/users", "application/json", strings.NewReader(`{}`))
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status=%d", response.StatusCode)
	}
}

func TestRXQDeltaHandlesFirstSampleIncrementAndReset(t *testing.T) {
	for _, test := range []struct {
		previous float64
		current  float64
		seen     bool
		expected float64
	}{
		{0, 7, false, 7},
		{7, 11, true, 4},
		{11, 11, true, 0},
		{11, 3, true, 3},
	} {
		if actual := rxqDelta(test.previous, test.current, test.seen); actual != test.expected {
			t.Fatalf("rxqDelta(%v,%v,%v)=%v want=%v", test.previous, test.current,
				test.seen, actual, test.expected)
		}
	}
}
