package webapp

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
	"nb-controlplane/internal/central"
)

func startTestSSHServer(t *testing.T) (string, int, ssh.PublicKey) {
	t.Helper()
	_, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(privateKey)
	if err != nil {
		t.Fatal(err)
	}
	configuration := &ssh.ServerConfig{PasswordCallback: func(metadata ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
		if metadata.User() == "root" && (string(password) == "private-password" ||
			string(password) == "initial-secret" || string(password) == "rotated-secret") {
			return nil, nil
		}
		return nil, errors.New("password rejected")
	}}
	configuration.AddHostKey(signer)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = listener.Close() })
	go func() {
		for {
			connection, acceptErr := listener.Accept()
			if acceptErr != nil {
				return
			}
			go func() {
				server, _, _, _ := ssh.NewServerConn(connection, configuration)
				if server != nil {
					_ = server.Close()
				} else {
					_ = connection.Close()
				}
			}()
		}
	}()
	address := listener.Addr().(*net.TCPAddr)
	return "127.0.0.1", address.Port, signer.PublicKey()
}

func TestTrafficHistoryAPIRequiresAuthAndValidRange(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-traffic", Name: "traffic", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	observed := time.Now().UTC().Add(-time.Minute)
	if _, err = database.RecordSnapshot(t.Context(), central.Snapshot{LineID: "line-traffic", NodeID: "entry-0",
		Role: "entry", WorkerID: "0", ObservedAt: observed.Format(time.RFC3339Nano), Health: "ok",
		UpstreamMbps: 2.5, DownstreamMbps: 3.5}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	from, to := observed.Add(-time.Minute).Format(time.RFC3339), observed.Add(time.Minute).Format(time.RFC3339)
	response, body := call(t, server.Client(), http.MethodGet,
		server.URL+"/api/v1/lines/line-traffic/traffic?from="+from+"&to="+to, "", "", nil)
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthorized status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet,
		server.URL+"/api/v1/lines/line-traffic/traffic?from=2025-01-01T00:00:00Z&to=2026-08-18T00:00:00Z", "admin", "", nil)
	if response.StatusCode != http.StatusBadRequest {
		t.Fatalf("oversized range status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet,
		server.URL+"/api/v1/lines/line-traffic/traffic?from="+from+"&to="+to, "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"resolution_s":15`)) ||
		!bytes.Contains(body, []byte(`"upstream_mbps":2.5`)) {
		t.Fatalf("history status=%d body=%s", response.StatusCode, body)
	}
}

func TestSnapshotStreamPublishesInsertedSnapshot(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-stream", Name: "stream", Status: "active",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, server.URL+"/api/v1/lines/line-stream/traffic/stream", nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer admin")
	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "text/event-stream" {
		t.Fatalf("stream status=%d content-type=%q", response.StatusCode, response.Header.Get("Content-Type"))
	}
	payload := central.Snapshot{LineID: "line-stream", NodeID: "entry-0", Role: "entry", WorkerID: "0",
		ObservedAt: time.Now().UTC().Format(time.RFC3339Nano), Health: "ok", UpstreamMbps: 4.25, DownstreamMbps: 1.5}
	response2, body := call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/snapshots", "agent", "", payload)
	if response2.StatusCode != http.StatusOK {
		t.Fatalf("snapshot status=%d body=%s", response2.StatusCode, body)
	}
	streamBody, err := io.ReadAll(io.LimitReader(response.Body, 4096))
	if err != nil && !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal(err)
	}
	if !bytes.Contains(streamBody, []byte("event: snapshot")) || !bytes.Contains(streamBody, []byte(`"upstream_mbps":4.25`)) {
		t.Fatalf("stream body=%q", streamBody)
	}
}

func TestTopologyAPIRequiresAdminAuthorization(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/topology", "", "", nil)
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthorized status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/topology", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"devices":[]`)) ||
		!bytes.Contains(body, []byte(`"links":[]`)) {
		t.Fatalf("topology status=%d body=%s", response.StatusCode, body)
	}
}

func TestTopologyLayoutAPIValidatesPersistsAndResets(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertDevice(t.Context(), central.Device{ID: "entry-1", Name: "Entry", Status: "ready",
		Host: "192.0.2.10", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	request := map[string]any{"updated_by": "operator", "layouts": []map[string]any{{
		"device_id": "entry-1", "x": -140.0, "y": 12.0, "z": 4.0,
	}}}
	response, body := call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/topology/layout", "admin", "", request)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"x":-140`)) {
		t.Fatalf("save layout status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/topology", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"layout":{"device_id":"entry-1"`)) {
		t.Fatalf("topology layout status=%d body=%s", response.StatusCode, body)
	}
	invalid := map[string]any{"updated_by": "operator", "layouts": []map[string]any{{
		"device_id": "entry-1", "x": 10001.0, "y": 0.0, "z": 0.0,
	}}}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/topology/layout", "admin", "", invalid)
	if response.StatusCode != http.StatusBadRequest {
		t.Fatalf("invalid layout status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/topology/layout", "admin", "", nil)
	if response.StatusCode != http.StatusNoContent {
		t.Fatalf("reset layout status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/topology", "admin", "", nil)
	if response.StatusCode != http.StatusOK || bytes.Contains(body, []byte(`"layout"`)) {
		t.Fatalf("layout remained after reset status=%d body=%s", response.StatusCode, body)
	}
}

func TestVisualizationAssetsAreEmbeddedLocally(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	for path, minimumSize := range map[string]int{
		"/vendor/echarts-6.1.0.min.js":         500000,
		"/vendor/three-0.185.1.module.min.js":  300000,
		"/vendor/three.core.min.js":            350000,
		"/vendor/3d-force-graph-1.80.0.min.js": 1000000,
		"/traffic-charts.js":                   3000,
		"/device-topology.js":                  3000,
		"/traffic-workspace.js":                3000,
		"/traffic-time.js":                     800,
		"/topology-layout.js":                  1000,
	} {
		response, err := server.Client().Get(server.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		body, readErr := io.ReadAll(response.Body)
		_ = response.Body.Close()
		if readErr != nil {
			t.Fatal(readErr)
		}
		if response.StatusCode != http.StatusOK || len(body) < minimumSize {
			t.Fatalf("asset %s status=%d size=%d", path, response.StatusCode, len(body))
		}
	}
	response, err := server.Client().Get(server.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	body, err := io.ReadAll(response.Body)
	_ = response.Body.Close()
	if err != nil {
		t.Fatal(err)
	}
	for _, required := range []string{"echarts-6.1.0.min.js", "3d-force-graph-1.80.0.min.js", `type="module"`, `id="deviceTopology"`, `id="trafficWorkspace"`, `data-traffic-from`, `data-traffic-events`, `id="operationSelectAll"`, `id="deleteSelectedOperations"`} {
		if !bytes.Contains(body, []byte(required)) {
			t.Fatalf("index is missing %q", required)
		}
	}
	policy := response.Header.Get("Content-Security-Policy")
	if !strings.Contains(policy, "script-src 'self'") || strings.Contains(policy, "script-src 'self' 'unsafe-inline'") ||
		!strings.Contains(policy, "style-src 'self' 'unsafe-inline'") {
		t.Fatalf("unexpected visualization CSP: %q", policy)
	}
}

func confirmedDeviceFields(t *testing.T, app *App, host string, port int, supplied ...ssh.PublicKey) map[string]any {
	t.Helper()
	var publicKey ssh.PublicKey
	if len(supplied) > 0 {
		publicKey = supplied[0]
	} else {
		_, privateKey, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		signer, err := ssh.NewSignerFromKey(privateKey)
		if err != nil {
			t.Fatal(err)
		}
		publicKey = signer.PublicKey()
	}
	key, keyType, fingerprint := publicKeyFields(publicKey)
	confirmation := hostKeyConfirmation{Host: host, SSHPort: port, Key: key, KeyType: keyType,
		Fingerprint: fingerprint, ExpiresAt: time.Now().Add(time.Minute).Unix()}
	token, err := app.signHostKeyConfirmation(confirmation)
	if err != nil {
		t.Fatal(err)
	}
	return map[string]any{"ssh_host_key": key, "ssh_host_key_type": keyType,
		"ssh_host_key_sha256": fingerprint, "host_key_confirmation_token": token}
}

func TestDeviceHostKeyMustBeScannedAndExplicitlyConfirmed(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent",
		DeviceSecretsFile: filepath.Join(directory, "device-secrets.json")}).Handler())
	defer server.Close()
	host, port, publicKey := startTestSSHServer(t)

	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices/host-key/scan", "admin", "",
		map[string]any{"host": host, "ssh_port": port})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("host-key scan status=%d body=%s", response.StatusCode, body)
	}
	var scanned struct {
		Key         string `json:"ssh_host_key"`
		KeyType     string `json:"ssh_host_key_type"`
		Fingerprint string `json:"ssh_host_key_sha256"`
		Token       string `json:"confirmation_token"`
	}
	if err = json.Unmarshal(body, &scanned); err != nil {
		t.Fatal(err)
	}
	expectedKey := base64.StdEncoding.EncodeToString(publicKey.Marshal())
	if scanned.Key != expectedKey || scanned.KeyType != publicKey.Type() ||
		scanned.Fingerprint != ssh.FingerprintSHA256(publicKey) || scanned.Token == "" {
		t.Fatalf("unexpected scan result: %+v", scanned)
	}

	device := map[string]any{"id": "exit-new", "name": "new exit", "status": "ready", "host": host,
		"ssh_port": port, "ssh_user": "root", "password": "private-password", "labels": map[string]any{}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("请先扫描并确认 SSH 主机密钥")) {
		t.Fatalf("unconfirmed create status=%d body=%s", response.StatusCode, body)
	}
	device["ssh_host_key"] = scanned.Key
	device["ssh_host_key_type"] = scanned.KeyType
	device["ssh_host_key_sha256"] = "SHA256:mismatched"
	device["host_key_confirmation_token"] = scanned.Token
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("SSH 主机密钥确认信息不匹配")) {
		t.Fatalf("mismatched fingerprint status=%d body=%s", response.StatusCode, body)
	}
	device["ssh_host_key_sha256"] = scanned.Fingerprint
	device["password"] = "wrong-password"
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("SSH 密码认证失败")) {
		t.Fatalf("invalid password status=%d body=%s", response.StatusCode, body)
	}
	device["password"] = "private-password"
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated || !bytes.Contains(body, []byte(`"ssh_host_key_status":"trusted"`)) {
		t.Fatalf("confirmed create status=%d body=%s", response.StatusCode, body)
	}

	delete(device, "password")
	delete(device, "ssh_host_key")
	delete(device, "ssh_host_key_type")
	delete(device, "ssh_host_key_sha256")
	delete(device, "host_key_confirmation_token")
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated || !bytes.Contains(body, []byte(`"ssh_host_key_status":"trusted"`)) {
		t.Fatalf("same-endpoint update did not retain trust status=%d body=%s", response.StatusCode, body)
	}
	device["ssh_port"] = port + 1
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("SSH 地址或端口已变化，请重新扫描并确认主机密钥")) {
		t.Fatalf("endpoint change retained trust status=%d body=%s", response.StatusCode, body)
	}
}

func TestDeviceFormRequiresVisibleHostKeyConfirmation(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{"deviceHostKeyConfirmation", "deviceHostKeyFingerprint", "confirmDeviceHostKey"} {
		if !bytes.Contains(index, []byte(expected)) {
			t.Fatalf("device form does not expose %s", expected)
		}
	}
	for _, expected := range []string{"/api/v1/devices/host-key/scan", "host_key_confirmation_token", "ssh_host_key_sha256"} {
		if !bytes.Contains(script, []byte(expected)) {
			t.Fatalf("device form does not implement %s", expected)
		}
	}
	if !bytes.Contains(script, []byte(`clearDeviceHostKeyConfirmation();$("#deviceModal").classList.add("hidden")`)) {
		t.Fatal("closing the device form does not clear pending credentials and host-key confirmation")
	}
}

func TestNodeSourceUploadUIExposesArchiveFormatsAndProgress(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{".zip", ".tar", ".tar.gz", "nodeSourceProgressBar", "nodeSourceProgressPercent"} {
		if !bytes.Contains(index, []byte(expected)) {
			t.Fatalf("Node upload UI is missing %s", expected)
		}
	}
	if bytes.Contains(index, []byte(`name="git_commit"`)) {
		t.Fatal("Node upload UI still requires a Git commit")
	}
	for _, expected := range []string{"XMLHttpRequest", "X-Upload-Offset", "file.slice", "attempt<5", "nodeUploadRequest.abort()"} {
		if !bytes.Contains(script, []byte(expected)) {
			t.Fatalf("Node upload progress is missing %s", expected)
		}
	}
}

func TestLineActionsExposeCompositeOptimize(t *testing.T) {
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{`"line.optimize":"验证并调优"`, `[["line.optimize","验证并调优","primary-action"]`} {
		if !bytes.Contains(script, []byte(expected)) {
			t.Fatalf("composite optimize UI is missing %s", expected)
		}
	}
	if bytes.Contains(script, []byte(`[["line.validate","验证",""] ,["line.tune","协议调优",""]`)) {
		t.Fatal("active line still renders separate validation and tuning actions")
	}
	if !bytes.Contains(script, []byte(`operation.kind!=="line.tune"&&operation.kind!=="line.optimize"`)) {
		t.Fatal("optimize detail does not render tuning result")
	}
}

func TestClientConfigurationAndQRCode(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()

	_, err = database.UpsertLine(t.Context(), central.Line{ID: "line-1", Name: "test", Status: "draft", Environment: "test",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10})
	if err != nil {
		t.Fatal(err)
	}
	operation, _, err := database.CreateOperation(t.Context(), central.Operation{ID: "op-client", LineID: "line-1",
		Kind: "line.open", RequestedBy: "test", IdempotencyKey: "client-config-test", Request: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = database.ClaimOperations(t.Context(), "line-1", 1); err != nil {
		t.Fatal(err)
	}
	clientURL := "socks5://user:password@192.0.2.10:1080#line-1"
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/operations/"+operation.ID+"/result", "agent", "",
		map[string]any{"line_id": "line-1", "status": "succeeded", "result": map[string]any{"deployment": "dep-1", "profile": "line-1:1"}})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("complete status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/lines/line-1/client-config", "agent", "",
		map[string]string{"client_url": clientURL})
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(operation.ID)) {
		t.Fatalf("attach status=%d body=%s", response.StatusCode, body)
	}
	response, _ = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations/"+operation.ID+"/client-qr", "agent", "", nil)
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("agent accessed client QR: %d", response.StatusCode)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations/"+operation.ID+"/client-qr", "admin", "", nil)
	var qr struct {
		MediaType string `json:"media_type"`
		Data      string `json:"data"`
	}
	if response.StatusCode != http.StatusOK || json.Unmarshal(body, &qr) != nil || qr.MediaType != "image/png" {
		t.Fatalf("QR status=%d body=%s", response.StatusCode, body)
	}
	png, err := base64.StdEncoding.DecodeString(qr.Data)
	if err != nil || !bytes.HasPrefix(png, []byte("\x89PNG\r\n\x1a\n")) {
		t.Fatal("QR response is not a PNG")
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/line-1/detail", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"client_operation"`)) || !bytes.Contains(body, []byte(clientURL)) {
		t.Fatalf("line detail omitted client config status=%d body=%s", response.StatusCode, body)
	}
}

func TestClientGateHidesProductionMaterialUntilQualification(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	ctx := t.Context()
	lineID := "line-client-gate"
	if _, err = database.UpsertLine(ctx, central.Line{ID: lineID, Name: "client gate",
		Status: "qualification_pending", Environment: "production", EntryRegion: "gz",
		ExitRegion: "es", Provider: "test", CapacityMbps: 10, ActiveDeployment: "deployment-gate"}); err != nil {
		t.Fatal(err)
	}
	operation, _, err := database.CreateOperation(ctx, central.Operation{ID: "op-client-gate", LineID: lineID,
		Kind: "line.open", RequestedBy: "test", IdempotencyKey: "client-gate-open", Request: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = database.ClaimOperations(ctx, lineID, 1); err != nil {
		t.Fatal(err)
	}
	clientURL := "socks5://user:password@192.0.2.10:1082#line-client-gate"
	if err = database.CompleteOperation(ctx, operation.ID, lineID, "succeeded", json.RawMessage(
		`{"deployment":"deployment-gate","profile":"line-client-gate:1","client_url":"`+clientURL+`"}`)); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations/"+operation.ID+"/client-qr", "admin", "", nil)
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("pending QR status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/"+lineID+"/detail", "admin", "", nil)
	if response.StatusCode != http.StatusOK || bytes.Contains(body, []byte(clientURL)) || bytes.Contains(body, []byte(`"client_operation"`)) {
		t.Fatalf("pending detail leaked client material status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations/"+operation.ID, "admin", "", nil)
	if response.StatusCode != http.StatusOK || bytes.Contains(body, []byte(clientURL)) || bytes.Contains(body, []byte(`"client_url"`)) {
		t.Fatalf("pending operation detail leaked client material status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations?line_id="+lineID, "admin", "", nil)
	if response.StatusCode != http.StatusOK || bytes.Contains(body, []byte(clientURL)) || bytes.Contains(body, []byte(`"client_url"`)) {
		t.Fatalf("pending operation list leaked client material status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/lines/"+lineID+"/client-config", "agent", "",
		map[string]string{"client_url": clientURL})
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("pending attachment status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/nb/v1/usage-events", "agent", "pending-usage",
		map[string]any{"line_id": lineID, "bytes": 1024})
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("pending usage status=%d body=%s", response.StatusCode, body)
	}
}

func TestAgentListsOnlyActiveLinePlans(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	for _, item := range []central.Line{
		{ID: "line-active", Name: "active", Status: "active", EntryRegion: "a", ExitRegion: "b", Provider: "test", CapacityMbps: 10},
		{ID: "line-draft", Name: "draft", Status: "draft", EntryRegion: "a", ExitRegion: "b", Provider: "test", CapacityMbps: 10},
	} {
		if _, err = database.UpsertLine(t.Context(), item); err != nil {
			t.Fatal(err)
		}
		if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: item.ID, ResourceGroup: "group", InstanceID: item.ID,
			BandwidthMbps: 10, SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071, RelayPort: 4445, ExitPort: 4443,
			Whitelist: json.RawMessage(`[]`), BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto"}); err != nil {
			t.Fatal(err)
		}
	}
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/line-plans", "agent", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"line_id":"line-active"`)) || bytes.Contains(body, []byte(`"line_id":"line-draft"`)) {
		t.Fatalf("plans status=%d body=%s", response.StatusCode, body)
	}
	response, _ = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/line-plans", "admin", "", nil)
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("admin accessed agent plans: %d", response.StatusCode)
	}
}

func TestLineSpecDNSDefaultsAndValidation(t *testing.T) {
	spec := central.LineSpec{LineID: "line-dns", ResourceGroup: "group", InstanceID: "line-dns_1",
		BandwidthMbps: 5, UpstreamMbps: 5, DownstreamMbps: 5, SocksPort: 1082,
		UDPPortMin: 22048, UDPPortMax: 23071, RelayPort: 4445, ExitPort: 4443,
		Whitelist: json.RawMessage(`[]`), BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto",
		Nodes: []central.LineNode{{DeviceID: "entry", Role: "entry"}, {DeviceID: "exit", Role: "exit"}}}
	spec.NormalizeRates()
	if err := validLineSpecRequest(spec); err != nil {
		t.Fatalf("default DNS was rejected: %v", err)
	}
	spec.DNSServers = json.RawMessage(`["1.1.1.1","dns.example"]`)
	if err := validLineSpecRequest(spec); err == nil || !strings.Contains(err.Error(), "IPv4") {
		t.Fatalf("invalid DNS was accepted: %v", err)
	}
}

func TestCreateLineRejectsDuplicateID(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	line := map[string]any{"id": "line-unique", "name": "unique", "status": "draft", "environment": "test", "entry_region": "entry",
		"exit_region": "exit", "provider": "test", "capacity_mbps": 10, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin", "", line)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("first create status=%d body=%s", response.StatusCode, body)
	}
	line["name"] = "must not replace"
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin", "", line)
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("line ID already exists")) {
		t.Fatalf("duplicate create status=%d body=%s", response.StatusCode, body)
	}
	stored, err := database.Line(t.Context(), "line-unique")
	if err != nil || stored.Name != "unique" || stored.Environment != "test" {
		t.Fatalf("duplicate create changed line: %+v err=%v", stored, err)
	}
}

func TestPatchLineEnvironmentUpdatesLineAndSpec(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	line := map[string]any{"id": "line-environment-edit", "name": "environment edit", "status": "draft", "environment": "production", "entry_region": "entry", "exit_region": "exit", "provider": "test", "capacity_mbps": 10}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin", "", line)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create status=%d body=%s", response.StatusCode, body)
	}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: "line-environment-edit", ResourceGroup: "group", InstanceID: "line-environment-edit-1", BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071, RelayPort: 4445, ExitPort: 4443, BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto", Environment: "production", Whitelist: json.RawMessage(`[]`)}); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPatch, server.URL+"/api/v1/lines/line-environment-edit", "admin", "", map[string]any{"environment": "test"})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("patch status=%d body=%s", response.StatusCode, body)
	}
	storedLine, err := database.Line(t.Context(), "line-environment-edit")
	if err != nil || storedLine.Environment != "test" {
		t.Fatalf("line environment=%q err=%v", storedLine.Environment, err)
	}
	storedSpec, err := database.LineSpec(t.Context(), "line-environment-edit")
	if err != nil || storedSpec.Environment != "test" {
		t.Fatalf("spec environment=%q err=%v", storedSpec.Environment, err)
	}
	if _, _, err = database.CreateOperation(t.Context(), central.Operation{ID: "op-environment-edit", LineID: "line-environment-edit", Kind: "line.validate", RequestedBy: "operator", IdempotencyKey: "environment-edit-operation", Request: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPatch, server.URL+"/api/v1/lines/line-environment-edit", "admin", "", map[string]any{"environment": "production"})
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("排队中或执行中")) {
		t.Fatalf("active operation environment patch status=%d body=%s", response.StatusCode, body)
	}
}

func TestDevicePasswordIsStoredLocallyAndNeverReturned(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	secretsFile := filepath.Join(directory, "private", "device-secrets.json")
	if err = os.MkdirAll(filepath.Dir(secretsFile), 0700); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(secretsFile, []byte(`{"legacy":{"password_env":"NB_LEGACY_PASSWORD"}}`), 0600); err != nil {
		t.Fatal(err)
	}
	app := New(database, Config{AdminToken: "admin", AgentToken: "agent", DeviceSecretsFile: secretsFile})
	server := httptest.NewServer(app.Handler())
	defer server.Close()
	host, port, publicKey := startTestSSHServer(t)

	device := map[string]any{"id": "entry-new", "name": "new entry", "status": "ready",
		"host": host, "ssh_port": port, "ssh_user": "root", "private_ip": "",
		"region": "gz", "provider": "test", "os": "linux", "arch": "amd64", "labels": map[string]any{}}
	for key, value := range confirmedDeviceFields(t, app, host, port, publicKey) {
		device[key] = value
	}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("SSH password is required")) {
		t.Fatalf("passwordless create status=%d body=%s", response.StatusCode, body)
	}

	device["password"] = "initial-secret"
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated || !bytes.Contains(body, []byte(`"secret_ref":"device:entry-new"`)) || bytes.Contains(body, []byte("initial-secret")) {
		t.Fatalf("password create status=%d body=%s", response.StatusCode, body)
	}
	contents, err := os.ReadFile(secretsFile)
	if err != nil || !bytes.Contains(contents, []byte("initial-secret")) || !bytes.Contains(contents, []byte("NB_LEGACY_PASSWORD")) {
		t.Fatalf("local device secret was not stored: err=%v contents=%s", err, contents)
	}

	delete(device, "password")
	delete(device, "ssh_host_key")
	delete(device, "ssh_host_key_type")
	delete(device, "ssh_host_key_sha256")
	delete(device, "host_key_confirmation_token")
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated || bytes.Contains(body, []byte("initial-secret")) {
		t.Fatalf("password-preserving update status=%d body=%s", response.StatusCode, body)
	}
	contents, _ = os.ReadFile(secretsFile)
	if !bytes.Contains(contents, []byte("initial-secret")) {
		t.Fatal("empty edit replaced the stored password")
	}

	device["password"] = "rotated-secret"
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated || bytes.Contains(body, []byte("rotated-secret")) {
		t.Fatalf("password rotation status=%d body=%s", response.StatusCode, body)
	}
	contents, _ = os.ReadFile(secretsFile)
	if !bytes.Contains(contents, []byte("rotated-secret")) || bytes.Contains(contents, []byte("initial-secret")) {
		t.Fatalf("password was not atomically rotated: %s", contents)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/devices/entry-new", "admin", "", nil)
	if response.StatusCode != http.StatusOK || bytes.Contains(body, []byte("rotated-secret")) {
		t.Fatalf("device API exposed password status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/devices/entry-new", "admin", "", nil)
	if response.StatusCode != http.StatusNoContent {
		t.Fatalf("device delete status=%d body=%s", response.StatusCode, body)
	}
	contents, _ = os.ReadFile(secretsFile)
	if bytes.Contains(contents, []byte("device:entry-new")) || bytes.Contains(contents, []byte("rotated-secret")) {
		t.Fatalf("device delete retained password: %s", contents)
	}
}

func TestDeviceExportIncludesEnvironmentAndExcludesSecrets(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertDevice(t.Context(), central.Device{ID: "test-device", Name: "Test device", Status: "ready", Environment: "test",
		Host: "192.0.2.10", SSHPort: 22, SSHUser: "root", SecretRef: "device:test-device", Region: "test", Provider: "lab"}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/devices/export", "admin", "", nil)
	if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "text/csv; charset=utf-8" {
		t.Fatalf("export status=%d content-type=%q body=%s", response.StatusCode, response.Header.Get("Content-Type"), body)
	}
	text := string(body)
	if !strings.Contains(text, "id,name,environment") || !strings.Contains(text, "test-device,Test device,test") || strings.Contains(text, "device:test-device") {
		t.Fatalf("unexpected export: %s", text)
	}
}

func TestNodeSourceUploadCreatesBuildOperation(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	uploadDir := filepath.Join(directory, "source-uploads")
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent", NodeSourceUploadDir: uploadDir}).Handler())
	defer server.Close()
	var archive bytes.Buffer
	gzipWriter := gzip.NewWriter(&archive)
	tarWriter := tar.NewWriter(gzipWriter)
	content := []byte("ref: refs/heads/main\n")
	if err = tarWriter.WriteHeader(&tar.Header{Name: "repo/.git/HEAD", Mode: 0600, Size: int64(len(content))}); err != nil {
		t.Fatal(err)
	}
	if _, err = tarWriter.Write(content); err != nil {
		t.Fatal(err)
	}
	_ = tarWriter.Close()
	_ = gzipWriter.Close()
	var requestBody bytes.Buffer
	multipartWriter := multipart.NewWriter(&requestBody)
	_ = multipartWriter.WriteField("git_commit", strings.Repeat("a", 40))
	_ = multipartWriter.WriteField("requested_by", "operator")
	part, err := multipartWriter.CreateFormFile("archive", "repo.tar.gz")
	if err != nil {
		t.Fatal(err)
	}
	_, _ = part.Write(archive.Bytes())
	_ = multipartWriter.Close()
	request, err := http.NewRequest(http.MethodPost, server.URL+"/api/v1/node-releases/uploads", &requestBody)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer admin")
	request.Header.Set("Content-Type", multipartWriter.FormDataContentType())
	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(response.Body)
	_ = response.Body.Close()
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"kind":"node.release.build"`)) {
		t.Fatalf("upload status=%d body=%s", response.StatusCode, body)
	}
	items, err := database.Operations(t.Context(), "__node_release__", 10)
	if err != nil || len(items) != 1 || items[0].Kind != "node.release.build" {
		t.Fatalf("operations=%+v err=%v", items, err)
	}
}

func TestNodeSourceUploadAcceptsZipAndTar(t *testing.T) {
	formats := map[string]func(*bytes.Buffer) error{
		"repo.zip": func(output *bytes.Buffer) error {
			archive := zip.NewWriter(output)
			file, err := archive.Create("repo/.git/HEAD")
			if err == nil {
				_, err = file.Write([]byte("ref: refs/heads/main\n"))
			}
			if closeErr := archive.Close(); err == nil {
				err = closeErr
			}
			return err
		},
		"repo.tar": func(output *bytes.Buffer) error {
			archive := tar.NewWriter(output)
			content := []byte("ref: refs/heads/main\n")
			err := archive.WriteHeader(&tar.Header{Name: "repo/.git/HEAD", Mode: 0600, Size: int64(len(content))})
			if err == nil {
				_, err = archive.Write(content)
			}
			if closeErr := archive.Close(); err == nil {
				err = closeErr
			}
			return err
		},
	}
	for filename, build := range formats {
		t.Run(filename, func(t *testing.T) {
			directory := t.TempDir()
			database, err := central.Open(filepath.Join(directory, "central.db"))
			if err != nil {
				t.Fatal(err)
			}
			defer database.Close()
			server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent", NodeSourceUploadDir: filepath.Join(directory, "uploads")}).Handler())
			defer server.Close()
			var source, body bytes.Buffer
			if err = build(&source); err != nil {
				t.Fatal(err)
			}
			writer := multipart.NewWriter(&body)
			_ = writer.WriteField("git_commit", strings.Repeat("b", 40))
			part, createErr := writer.CreateFormFile("archive", filename)
			if createErr != nil {
				t.Fatal(createErr)
			}
			_, _ = part.Write(source.Bytes())
			_ = writer.Close()
			request, _ := http.NewRequest(http.MethodPost, server.URL+"/api/v1/node-releases/uploads", &body)
			request.Header.Set("Authorization", "Bearer admin")
			request.Header.Set("Content-Type", writer.FormDataContentType())
			response, requestErr := server.Client().Do(request)
			if requestErr != nil {
				t.Fatal(requestErr)
			}
			responseBody, _ := io.ReadAll(response.Body)
			_ = response.Body.Close()
			if response.StatusCode != http.StatusAccepted {
				t.Fatalf("status=%d body=%s", response.StatusCode, responseBody)
			}
		})
	}
}

func TestNodeSourceChunkedUploadResumesAndCreatesBuildOperation(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent", NodeSourceUploadDir: filepath.Join(directory, "uploads")}).Handler())
	defer server.Close()
	var source bytes.Buffer
	archive := zip.NewWriter(&source)
	file, _ := archive.Create("repo/.git/HEAD")
	_, _ = file.Write([]byte("ref: refs/heads/main\n"))
	_ = archive.Close()
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/node-releases/uploads", "admin", "", map[string]any{
		"filename": "repo.zip", "size": source.Len(), "upload_key": "repo-zip-test",
	})
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("init status=%d body=%s", response.StatusCode, body)
	}
	var initialized struct {
		UploadID string `json:"upload_id"`
		Received int64  `json:"received"`
	}
	if json.Unmarshal(body, &initialized) != nil || initialized.UploadID == "" || initialized.Received != 0 {
		t.Fatalf("invalid init body=%s", body)
	}
	cut := source.Len() / 2
	uploadChunk := func(offset int, data []byte, want int) {
		request, _ := http.NewRequest(http.MethodPut, server.URL+"/api/v1/node-releases/uploads/"+initialized.UploadID, bytes.NewReader(data))
		request.Header.Set("Authorization", "Bearer admin")
		request.Header.Set("X-Upload-Offset", strconv.Itoa(offset))
		response, requestErr := server.Client().Do(request)
		if requestErr != nil {
			t.Fatal(requestErr)
		}
		responseBody, _ := io.ReadAll(response.Body)
		_ = response.Body.Close()
		if response.StatusCode != http.StatusOK || !bytes.Contains(responseBody, []byte(`"received":`+strconv.Itoa(want))) {
			t.Fatalf("chunk status=%d body=%s", response.StatusCode, responseBody)
		}
	}
	uploadChunk(0, source.Bytes()[:cut], cut)
	uploadChunk(cut, source.Bytes()[cut:], source.Len())
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/node-releases/uploads/"+initialized.UploadID+"/complete", "admin", "", map[string]any{
		"requested_by": "operator",
	})
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"kind":"node.release.build"`)) {
		t.Fatalf("complete status=%d body=%s", response.StatusCode, body)
	}
}

func call(t *testing.T, client *http.Client, method, url, token, key string, body any) (*http.Response, []byte) {
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
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
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

func TestCentralWebWorkflow(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin-secret", AgentToken: "agent-secret"}).Handler())
	defer server.Close()

	line := map[string]any{"id": "gz-hk-us", "name": "GZ / HK / US", "status": "active", "environment": "test", "entry_region": "Guangzhou",
		"exit_region": "United States", "provider": "multi", "capacity_mbps": 10, "active_deployment": "dep-1", "profile": "gz-hk-us:1", "secret_ref": "vault/nb/lines/gz-hk-us"}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin-secret", "", line)
	if response.StatusCode != 201 {
		t.Fatalf("line status=%d body=%s", response.StatusCode, body)
	}

	snapshot := map[string]any{"line_id": "gz-hk-us", "node_id": "entry-1", "role": "entry", "worker_id": "0",
		"observed_at": "2026-07-28T02:00:00Z", "health": "ok", "deployment": "dep-1", "profile": "gz-hk-us:1",
		"sessions": 4, "throughput_mbps": 3.8, "queue_age_p95_us": 120, "effective_loss_pct": 0, "fec_observe": true, "fec_active": false}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/snapshots", "agent-secret", "", snapshot)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("snapshot status=%d body=%s", response.StatusCode, body)
	}
	legacy := map[string]any{"node_id": "nb-exit-1", "line_id": "gz-hk-us", "observed_at": "2026-07-28T02:00:01Z",
		"health": map[string]any{"status": "ok", "worker": 1, "release_id": "dep-1", "line_profile": "gz-hk-us:1"},
		"metrics": map[string]any{"sessions_inuse": 2, "throughput_mbps": 1.2,
			"queue_age_max_us": map[string]any{"down": 40, "up": 20, "q2t": 10},
			"link":             map[string]any{"effective_loss_max_pct": 0}, "fec": map[string]any{"observe": 1, "active": 0}}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/nb/v1/node-snapshots", "agent-secret", "", legacy)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("legacy snapshot status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/dashboard", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"lines_healthy":1`)) ||
		!bytes.Contains(body, []byte(`"throughput_mbps":3.8`)) || !bytes.Contains(body, []byte(`"sessions":4`)) {
		t.Fatalf("dashboard status=%d body=%s", response.StatusCode, body)
	}
	newerSnapshot := map[string]any{"line_id": "gz-hk-us", "node_id": "entry-1", "role": "entry", "worker_id": "collector-2",
		"observed_at": "2026-07-28T02:00:02Z", "health": "down", "deployment": "dep-1", "profile": "gz-hk-us:1",
		"sessions": 0, "throughput_mbps": 0, "queue_age_p95_us": 0, "effective_loss_pct": 0, "fec_observe": true, "fec_active": false}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/snapshots", "agent-secret", "", newerSnapshot)
	if response.StatusCode != 200 {
		t.Fatalf("newer snapshot status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/gz-hk-us/detail", "admin-secret", "", nil)
	if response.StatusCode != 200 || bytes.Count(body, []byte(`"node_id":"entry-1"`)) != 1 ||
		!bytes.Contains(body, []byte(`"worker_id":"collector-2"`)) {
		t.Fatalf("latest node snapshot was not deduplicated status=%d body=%s", response.StatusCode, body)
	}
	heartbeat := map[string]any{"worker_id": "windows-1", "status": "ready", "version": "test",
		"observed_at": time.Now().UTC().Format(time.RFC3339Nano),
		"lines":       []map[string]any{{"line_id": "gz-hk-us", "operations": []string{"line.validate", "line.upgrade"}}}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/executors/heartbeat", "agent-secret", "", heartbeat)
	if response.StatusCode != 200 {
		t.Fatalf("heartbeat status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/executors", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"online":true`)) {
		t.Fatalf("executors status=%d body=%s", response.StatusCode, body)
	}
	usage := map[string]any{"line_id": "gz-hk-us", "bytes": 1234}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/nb/v1/usage-events", "agent-secret", "usage-1", usage)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"inserted":true`)) {
		t.Fatalf("raw event status=%d body=%s", response.StatusCode, body)
	}

	operation := map[string]any{"line_id": "gz-hk-us", "kind": "line.upgrade", "requested_by": "operator", "request": map[string]any{"deployment": "dep-2"}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-1", operation)
	if response.StatusCode != 201 || !bytes.Contains(body, []byte(`"status":"queued"`)) {
		t.Fatalf("operation status=%d body=%s", response.StatusCode, body)
	}
	var created central.Operation
	if err = json.Unmarshal(body, &created); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-1", operation)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(created.ID)) {
		t.Fatalf("operation replay status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/operations?line_id=gz-hk-us&limit=5", "agent-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"dispatched"`)) {
		t.Fatalf("claim status=%d body=%s", response.StatusCode, body)
	}
	result := map[string]any{"line_id": "gz-hk-us", "status": "succeeded", "result": map[string]any{"deployment": "dep-2"}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/agent/v1/operations/"+created.ID+"/result", "agent-secret", "", result)
	if response.StatusCode != 200 {
		t.Fatalf("complete status=%d body=%s", response.StatusCode, body)
	}

	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/operations", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"succeeded"`)) {
		t.Fatalf("operations status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"active_deployment":"dep-2"`)) {
		t.Fatalf("operation did not update line status=%d body=%s", response.StatusCode, body)
	}
	cancellable := map[string]any{"line_id": "gz-hk-us", "kind": "line.validate", "requested_by": "operator", "request": map[string]any{}}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin-secret", "web-op-cancel", cancellable)
	var queued central.Operation
	if response.StatusCode != 201 || json.Unmarshal(body, &queued) != nil {
		t.Fatalf("cancellable operation status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations/"+queued.ID+"/cancel", "admin-secret", "", map[string]string{"reason": "duplicate"})
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"cancelled"`)) {
		t.Fatalf("cancel status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPatch, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", map[string]string{"status": "archived"})
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"status":"archived"`)) {
		t.Fatalf("archive status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines", "admin-secret", "", nil)
	if response.StatusCode != 200 || bytes.Contains(body, []byte(`"id":"gz-hk-us"`)) {
		t.Fatalf("archived line remained visible status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines/gz-hk-us", "admin-secret", "", nil)
	if response.StatusCode != 200 {
		t.Fatalf("archived line history is unavailable status=%d body=%s", response.StatusCode, body)
	}
}

func TestCentralWebSeparatesAdminAndAgentTokens(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, _ := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/lines", "agent", "", nil)
	if response.StatusCode != 401 {
		t.Fatalf("agent accessed admin API: %d", response.StatusCode)
	}
	response, _ = call(t, server.Client(), http.MethodGet, server.URL+"/agent/v1/operations?line_id=test", "admin", "", nil)
	if response.StatusCode != 401 {
		t.Fatalf("admin accessed agent API: %d", response.StatusCode)
	}
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/", "", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte("NB 运营控制台")) {
		t.Fatalf("static UI status=%d", response.StatusCode)
	}
	if !bytes.Contains(body, []byte(`白名单更新地址`)) || bytes.Contains(body, []byte(`env:NB_LINE_SRS_URL`)) {
		t.Fatal("whitelist URL field is missing or still exposes environment references")
	}
	if !bytes.Contains(body, []byte(`name="password"`)) || bytes.Contains(body, []byte(`name="secret_ref"`)) {
		t.Fatal("device form must collect an initial password instead of a secret reference")
	}
	if bytes.Contains(body, []byte(`name="resource_group"`)) {
		t.Fatal("line form still exposes internal resource group metadata")
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/app.js", "", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`operationAvailability`)) ||
		!bytes.Contains(body, []byte(`disabled title=`)) ||
		!bytes.Contains(body, []byte(`computer-icon`)) || !bytes.Contains(body, []byte(`队列最大等待`)) ||
		!bytes.Contains(body, []byte(`clientConfigSection`)) || !bytes.Contains(body, []byte(`/client-qr`)) ||
		!bytes.Contains(body, []byte(`upstream_mbps`)) || !bytes.Contains(body, []byte(`downstream_mbps`)) ||
		!bytes.Contains(body, []byte(`item.role === "entry"`)) ||
		!bytes.Contains(body, []byte(`tuneResultSection`)) || !bytes.Contains(body, []byte(`transport_rollout`)) ||
		!bytes.Contains(body, []byte(`影响线路`)) || !bytes.Contains(body, []byte(`affected_lines`)) ||
		!bytes.Contains(body, []byte(`节点升级待处理`)) || bytes.Contains(body, []byte(`node_release_ack`)) ||
		!bytes.Contains(body, []byte(`失败原因`)) || !bytes.Contains(body, []byte(`log_excerpt`)) ||
		bytes.Contains(body, []byte(`event.currentTarget.reset()`)) {
		t.Fatalf("line lifecycle actions missing from UI status=%d", response.StatusCode)
	}
}

func TestSingleHKLineSpecAPI(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	app := New(database, Config{AdminToken: "admin", AgentToken: "agent",
		DeviceSecretsFile: filepath.Join(directory, "device-secrets.json")})
	app.verifySSHCredentials = func(context.Context, string, int, string, string, ssh.PublicKey) error { return nil }
	server := httptest.NewServer(app.Handler())
	defer server.Close()
	device := map[string]any{"id": "hk-1", "name": "Hong Kong", "status": "ready",
		"host": "192.0.2.20", "ssh_port": 22, "ssh_user": "root", "region": "HK",
		"environment": "test", "provider": "test", "password": "secret", "labels": map[string]any{}}
	for key, value := range confirmedDeviceFields(t, app, "192.0.2.20", 22) {
		device[key] = value
	}
	response, body := call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create device status=%d body=%s", response.StatusCode, body)
	}
	line := map[string]any{"id": "hk-single", "name": "HK single", "status": "draft",
		"environment": "test", "entry_region": "HK", "exit_region": "HK", "provider": "test",
		"capacity_mbps": 5, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/lines", "admin", "", line)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create line status=%d body=%s", response.StatusCode, body)
	}
	nodes := []map[string]any{{"device_id": "hk-1", "role": "entry", "ordinal": 0},
		{"device_id": "hk-1", "role": "exit", "ordinal": 0}}
	spec := map[string]any{"topology_mode": "single_hk", "service_profile": "general",
		"instance_id": "hk-single_1", "environment": "test", "bandwidth_mbps": 5,
		"upstream_mbps": 5, "downstream_mbps": 5, "socks_port": 0, "relay_port": 0,
		"exit_port": 0, "udp_port_min": 0, "udp_port_max": 0, "dns_servers": []string{"1.1.1.1"},
		"whitelist": []string{}, "build_mode": "auto", "source_ref": "repo://current",
		"jump_policy": "direct", "nodes": nodes}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/lines/hk-single/spec", "admin", "", spec)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"topology_mode":"single_hk"`)) ||
		!bytes.Contains(body, []byte(`"service_profile":"general"`)) || !bytes.Contains(body, []byte(`"relay_port":0`)) {
		t.Fatalf("save single-HK status=%d body=%s", response.StatusCode, body)
	}
	spec["nodes"] = []map[string]any{{"device_id": "hk-1", "role": "entry", "ordinal": 0},
		{"device_id": "other", "role": "exit", "ordinal": 0}}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/lines/hk-single/spec", "admin", "", spec)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("同一台香港设备")) {
		t.Fatalf("different-device status=%d body=%s", response.StatusCode, body)
	}
}

func TestInventoryTopologyAndOperationEvents(t *testing.T) {
	directory := t.TempDir()
	database, err := central.Open(filepath.Join(directory, "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	app := New(database, Config{AdminToken: "admin", AgentToken: "agent",
		DeviceSecretsFile: filepath.Join(directory, "device-secrets.json")})
	app.verifySSHCredentials = func(context.Context, string, int, string, string, ssh.PublicKey) error { return nil }
	server := httptest.NewServer(app.Handler())
	defer server.Close()
	client := server.Client()
	for index, role := range []string{"entry", "relay", "exit"} {
		device := map[string]any{"id": role + "-1", "name": role + " device", "status": "ready",
			"host": "192.0.2." + string(rune('1'+index)), "ssh_port": 22, "ssh_user": "root",
			"region": role, "provider": "test", "password": "test-" + role, "labels": map[string]any{}}
		for key, value := range confirmedDeviceFields(t, app, device["host"].(string), 22) {
			device[key] = value
		}
		response, body := call(t, client, http.MethodPost, server.URL+"/api/v1/devices", "admin", "", device)
		if response.StatusCode != 201 {
			t.Fatalf("create device status=%d body=%s", response.StatusCode, body)
		}
	}
	line := map[string]any{"id": "line-1", "name": "test line", "status": "draft", "environment": "test", "entry_region": "entry",
		"exit_region": "exit", "provider": "test", "capacity_mbps": 20, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body := call(t, client, http.MethodPost, server.URL+"/api/v1/lines", "admin", "", line)
	if response.StatusCode != 201 {
		t.Fatalf("create line status=%d body=%s", response.StatusCode, body)
	}
	nodes := []map[string]any{
		{"device_id": "entry-1", "role": "entry", "ordinal": 0, "next_hop_device_id": "relay-1", "jump_candidates": []string{}, "config": map[string]any{}},
		{"device_id": "relay-1", "role": "relay", "ordinal": 0, "next_hop_device_id": "exit-1", "jump_candidates": []string{"entry-1"}, "config": map[string]any{}},
		{"device_id": "exit-1", "role": "exit", "ordinal": 0, "next_hop_device_id": "", "jump_candidates": []string{"relay-1"}, "config": map[string]any{}},
	}
	spec := map[string]any{"instance_id": "test", "environment": "test", "bandwidth_mbps": 20,
		"socks_port": 1082, "relay_port": 4445, "exit_port": 4443, "exit_bind_ip": "192.0.2.3", "udp_port_min": 22048, "udp_port_max": 23071,
		"whitelist": []string{"domain example.com"}, "build_mode": "auto", "artifact_ref": "", "source_ref": "repo://current",
		"srs_ref": "https://rules.example.invalid/whitelist.srs?key=test-key", "jump_policy": "auto", "nodes": nodes}
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-1/spec", "admin", "", spec)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"exit_bind_ip":"192.0.2.3"`)) ||
		!bytes.Contains(body, []byte(`"resource_group":"managed"`)) {
		t.Fatalf("save topology status=%d body=%s", response.StatusCode, body)
	}
	spec["socks_port"] = 1080
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-1/spec", "admin", "", spec)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("1082-1199")) {
		t.Fatalf("out-of-range entry port status=%d body=%s", response.StatusCode, body)
	}
	spec["socks_port"] = 1082
	spec["srs_ref"] = "http://rules.example.invalid/whitelist.srs"
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-1/spec", "admin", "", spec)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("HTTPS")) {
		t.Fatalf("insecure whitelist URL status=%d body=%s", response.StatusCode, body)
	}
	spec["srs_ref"] = "https://rules.example.invalid/whitelist.srs?key=test-key"
	spec["exit_bind_ip"] = "2001:db8::1"
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-1/spec", "admin", "", spec)
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("IPv4")) {
		t.Fatalf("IPv6 exit bind address status=%d body=%s", response.StatusCode, body)
	}
	spec["exit_bind_ip"] = "192.0.2.3"
	secondLine := map[string]any{"id": "line-2", "name": "second line", "status": "draft", "entry_region": "entry",
		"exit_region": "exit", "provider": "test", "capacity_mbps": 20, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/lines", "admin", "", secondLine)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create second line status=%d body=%s", response.StatusCode, body)
	}
	conflictingSpec := map[string]any{"resource_group": "other-group", "instance_id": "second", "bandwidth_mbps": 20,
		"socks_port": 1182, "relay_port": 4545, "exit_port": 4444, "udp_port_min": 24000, "udp_port_max": 25023,
		"whitelist": []string{}, "build_mode": "auto", "artifact_ref": "", "source_ref": "repo://current",
		"srs_ref": "", "jump_policy": "auto", "nodes": nodes}
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-2/spec", "admin", "", conflictingSpec)
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("Exit")) || !bytes.Contains(body, []byte("冲突")) {
		t.Fatalf("shared exit port conflict status=%d body=%s", response.StatusCode, body)
	}
	autoLine := map[string]any{"id": "line-auto", "name": "auto allocated line", "status": "draft", "environment": "test", "entry_region": "entry",
		"exit_region": "exit", "provider": "test", "capacity_mbps": 20, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/lines", "admin", "", autoLine)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create auto line status=%d body=%s", response.StatusCode, body)
	}
	autoSpec := map[string]any{"resource_group": "other-group", "environment": "test", "instance_id": "", "bandwidth_mbps": 20,
		"socks_port": 1083, "relay_port": 0, "exit_port": 0, "udp_port_min": 0, "udp_port_max": 0,
		"whitelist": []string{}, "build_mode": "auto", "artifact_ref": "", "source_ref": "repo://current",
		"srs_ref": "", "jump_policy": "auto", "nodes": nodes}
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-auto/spec", "admin", "", autoSpec)
	if response.StatusCode != http.StatusOK {
		t.Fatalf("auto allocate shared devices status=%d body=%s", response.StatusCode, body)
	}
	var allocated central.LineSpec
	if err = json.Unmarshal(body, &allocated); err != nil {
		t.Fatal(err)
	}
	if allocated.InstanceID != "line-auto_1" || allocated.RelayPort != 4447 || allocated.ExitPort != 4445 ||
		allocated.UDPPortMin == 0 || allocated.UDPPortMin <= 23071 {
		t.Fatalf("internal resources were not independently allocated: %+v", allocated)
	}
	heartbeat := map[string]any{"worker_id": "worker-1", "status": "ready", "version": "test", "observed_at": time.Now().UTC(),
		"lines": []map[string]any{{"line_id": "*", "operations": []string{"line.open", "line.validate", "line.optimize", "line.upgrade", "line.rollback", "line.disable", "line.tune"}}}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/executors/heartbeat", "agent", "", heartbeat)
	if response.StatusCode != 200 {
		t.Fatalf("heartbeat status=%d body=%s", response.StatusCode, body)
	}
	upgrade := map[string]any{"line_id": "line-auto", "kind": "line.upgrade", "requested_by": "test",
		"request": map[string]any{"affected_lines": []string{"forged-line"}}}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/operations", "admin", "upgrade-impact-test", upgrade)
	if response.StatusCode != http.StatusCreated || bytes.Contains(body, []byte("forged-line")) ||
		!bytes.Contains(body, []byte(`"affected_lines":["line-1","line-auto"]`)) {
		t.Fatalf("upgrade impact status=%d body=%s", response.StatusCode, body)
	}
	var upgradeOperation central.Operation
	if err = json.Unmarshal(body, &upgradeOperation); err != nil {
		t.Fatal(err)
	}
	if err = database.CancelOperation(t.Context(), upgradeOperation.ID, "impact test complete"); err != nil {
		t.Fatal(err)
	}
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "__node_release__", Name: "Node Release",
		Status: "active", EntryRegion: "control", ExitRegion: "control", Provider: "internal", CapacityMbps: 1}); err != nil {
		t.Fatal(err)
	}
	releaseOperation := central.Operation{ID: "op-release-for-open", LineID: "__node_release__",
		Kind: "node.release.build", RequestedBy: "test", IdempotencyKey: "release-for-open", Request: json.RawMessage(`{}`)}
	if _, _, err = database.CreateOperation(t.Context(), releaseOperation); err != nil {
		t.Fatal(err)
	}
	if _, err = database.ClaimOperations(t.Context(), releaseOperation.LineID, 1); err != nil {
		t.Fatal(err)
	}
	if err = database.CompleteOperation(t.Context(), releaseOperation.ID, releaseOperation.LineID, "succeeded",
		json.RawMessage(`{"node_release":{"release_id":"0123456789abcdef","node_version":{"semantic":"2.1.0"}}}`)); err != nil {
		t.Fatal(err)
	}
	operation := map[string]any{"line_id": "line-1", "kind": "line.open", "requested_by": "test", "request": map[string]any{}}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/operations", "admin", "inventory-test", operation)
	if response.StatusCode != 201 || !bytes.Contains(body, []byte(`"plan"`)) ||
		!bytes.Contains(body, []byte(`"pending_node_release"`)) {
		t.Fatalf("create operation status=%d body=%s", response.StatusCode, body)
	}
	var created central.Operation
	if err = json.Unmarshal(body, &created); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/agent/v1/operations?line_id=line-1&limit=1", "agent", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(created.ID)) {
		t.Fatalf("exact claim status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/agent/v1/operations?line_id=*&limit=1", "agent", "", nil)
	if response.StatusCode != 200 || bytes.Contains(body, []byte(created.ID)) {
		t.Fatalf("wildcard reclaimed dispatched operation status=%d body=%s", response.StatusCode, body)
	}
	event := map[string]any{"sequence": 1, "stage": "whitelist-fetch", "status": "running", "message": "started", "parameters": map[string]any{"mode": "auto"}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/operations/"+created.ID+"/events", "agent", "", event)
	if response.StatusCode != 202 {
		t.Fatalf("event status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/api/v1/operations/"+created.ID, "admin", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"whitelist-fetch"`)) || bytes.Contains(body, []byte("NB_TEST_SRS_URL=")) {
		t.Fatalf("operation detail status=%d body=%s", response.StatusCode, body)
	}
}

func TestProductionOpenRequiresIndependentDirectionalCapacity(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	device := func(id, environment string) central.Device {
		return central.Device{ID: id, Name: id, Status: "ready", Environment: environment,
			Host: "192.0.2.1", SSHPort: 22, SSHUser: "root", Labels: json.RawMessage(`{}`)}
	}
	for _, item := range []central.Device{device("entry-prod", "production"), device("relay-test", "test"),
		device("exit-prod", "production")} {
		if _, err = database.UpsertDevice(t.Context(), item); err != nil {
			t.Fatal(err)
		}
	}
	lineID := "line-full-duplex"
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: lineID, Name: lineID, Status: "draft",
		Environment: "production", EntryRegion: "gz", ExitRegion: "es", Provider: "test",
		CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	nodes := []central.LineNode{{DeviceID: "entry-prod", Role: "entry"},
		{DeviceID: "relay-test", Role: "relay"}, {DeviceID: "exit-prod", Role: "exit"}}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: lineID, Environment: "production",
		ResourceGroup: "managed", InstanceID: lineID + "_1", BandwidthMbps: 10,
		UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
		UDPPortMin: 22048, UDPPortMax: 23071, Whitelist: json.RawMessage(`[]`),
		DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", JumpPolicy: "auto",
		Nodes: nodes}); err != nil {
		t.Fatal(err)
	}
	if err = database.RecordExecutor(t.Context(), central.Executor{WorkerID: "worker-capacity", Status: "ready",
		Version: "test", ObservedAt: time.Now().UTC().Format(time.RFC3339Nano),
		Lines: []central.ExecutorLine{{LineID: "*", Operations: []string{"line.open"}}}}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	create := func(key string) (*http.Response, []byte) {
		return call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin", key,
			map[string]any{"line_id": lineID, "kind": "line.open", "requested_by": "operator",
				"request": map[string]any{}})
	}
	response, body := create("capacity-device-env")
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("relay-test")) ||
		!bytes.Contains(body, []byte("production")) {
		t.Fatalf("test device preflight status=%d body=%s", response.StatusCode, body)
	}
	if _, err = database.UpsertDevice(t.Context(), device("relay-test", "production")); err != nil {
		t.Fatal(err)
	}
	response, body = create("capacity-missing-link")
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("not registered")) {
		t.Fatalf("missing link preflight status=%d body=%s", response.StatusCode, body)
	}
	putLink := func(id string, payload map[string]any) {
		response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/network-links/"+id,
			"admin", "", payload)
		if response.StatusCode != http.StatusOK {
			t.Fatalf("put link %s status=%d body=%s", id, response.StatusCode, body)
		}
	}
	gzHK := map[string]any{"from_device_id": "entry-prod", "from_role": "entry",
		"to_device_id": "relay-test", "to_role": "relay", "forward_capacity_mbps": 10,
		"reverse_capacity_mbps": 10, "billing_mode": central.LinkBillingAggregate,
		"environment": "production", "status": "ready"}
	hkExit := map[string]any{"from_device_id": "relay-test", "from_role": "relay",
		"to_device_id": "exit-prod", "to_role": "exit", "forward_capacity_mbps": 10,
		"reverse_capacity_mbps": 10, "billing_mode": central.LinkBillingIndependent,
		"environment": "production", "status": "ready"}
	putLink("entry-relay", gzHK)
	putLink("relay-exit", hkExit)
	response, body = create("capacity-aggregate")
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("independent")) {
		t.Fatalf("aggregate link preflight status=%d body=%s", response.StatusCode, body)
	}
	gzHK["billing_mode"] = central.LinkBillingIndependent
	gzHK["reverse_capacity_mbps"] = 5
	putLink("entry-relay", gzHK)
	response, body = create("capacity-reverse")
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("reverse required=10 available=5")) {
		t.Fatalf("reverse capacity preflight status=%d body=%s", response.StatusCode, body)
	}
	gzHK["reverse_capacity_mbps"] = 10
	putLink("entry-relay", gzHK)
	response, body = create("capacity-ready")
	if response.StatusCode != http.StatusCreated || !bytes.Contains(body, []byte(`"capacity_reservations"`)) {
		t.Fatalf("qualified capacity status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/network-links", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"forward_reserved_mbps":10`)) ||
		!bytes.Contains(body, []byte(`"reverse_reserved_mbps":10`)) ||
		!bytes.Contains(body, []byte(`"forward_available_mbps":0`)) ||
		!bytes.Contains(body, []byte(`"reverse_available_mbps":0`)) ||
		!bytes.Contains(body, []byte(`"affected_lines":["line-full-duplex"]`)) {
		t.Fatalf("network link directional usage status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/network-links/entry-relay", "admin", "", gzHK)
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("禁止修改物理链路")) {
		t.Fatalf("active operation link mutation status=%d body=%s", response.StatusCode, body)
	}
	storedSpec, err := database.LineSpec(t.Context(), lineID)
	if err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/lines/"+lineID+"/spec", "admin", "", storedSpec)
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("禁止修改部署规格")) {
		t.Fatalf("active operation spec mutation status=%d body=%s", response.StatusCode, body)
	}
	for _, id := range []string{"test-entry", "test-relay", "test-exit"} {
		if _, err = database.UpsertDevice(t.Context(), device(id, "test")); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-test-capacity", Name: "test",
		Status: "draft", Environment: "test", EntryRegion: "gz", ExitRegion: "test", Provider: "test",
		CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: "line-test-capacity",
		Environment: "test", ResourceGroup: "managed", InstanceID: "line-test-capacity_1",
		BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10, SocksPort: 1083,
		RelayPort: 4447, ExitPort: 4445, UDPPortMin: 23072, UDPPortMax: 24095,
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto",
		JumpPolicy: "auto", Nodes: []central.LineNode{{DeviceID: "test-entry", Role: "entry"},
			{DeviceID: "test-relay", Role: "relay"}, {DeviceID: "test-exit", Role: "exit"}}}); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin",
		"capacity-test-line", map[string]any{"line_id": "line-test-capacity", "kind": "line.open",
			"requested_by": "operator", "request": map[string]any{}})
	if response.StatusCode != http.StatusCreated || bytes.Contains(body, []byte(`"capacity_reservations"`)) {
		t.Fatalf("test line capacity status=%d body=%s", response.StatusCode, body)
	}
}

func TestNetworkLinkUIExposesDirectionalCapacityAndAffectedLines(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, marker := range []string{"linksView", "networkLinksTable", "networkLinkModal",
		"forward_capacity_mbps", "reverse_capacity_mbps", "billing_mode", "affected_lines",
		"forward_reserved_mbps", "reverse_reserved_mbps", "forward_available_mbps", "reverse_available_mbps"} {
		if !bytes.Contains(index, []byte(marker)) && !bytes.Contains(script, []byte(marker)) {
			t.Fatalf("network link UI missing %q", marker)
		}
	}
	if bytes.Contains(index, []byte(`name="bidirectional_capacity_mbps"`)) ||
		bytes.Contains(script, []byte("bidirectional_capacity_mbps")) {
		t.Fatal("network link UI uses a combined bidirectional capacity field")
	}
}

func TestQualificationUIShowsDirectionalEvidence(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, marker := range []string{"待验证", "全双工合格", "全双工不合格", "资源不足",
		"target_upstream_mbps", "target_downstream_mbps", "achieved_upstream_mbps",
		"achieved_downstream_mbps", "required_ratio", "duration_seconds", "deployment_id", "operation_id"} {
		if !bytes.Contains(index, []byte(marker)) && !bytes.Contains(script, []byte(marker)) {
			t.Fatalf("qualification UI missing %q", marker)
		}
	}
}

func TestGovernanceAPIReportsRemediationAndBlocksUpgrade(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "governance-line", Name: "governance",
		Status: "active", Environment: "production", EntryRegion: "gz", ExitRegion: "es",
		Provider: "test", CapacityMbps: 10, ActiveDeployment: "deployment-current"}); err != nil {
		t.Fatal(err)
	}
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "governance-test", Name: "test",
		Status: "active", Environment: "test", EntryRegion: "gz", ExitRegion: "lab",
		Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	governanceNodes := []central.LineNode{}
	for _, item := range []struct{ id, role string }{{"governance-entry", "entry"}, {"governance-relay", "relay"}, {"governance-exit", "exit"}} {
		if _, err = database.UpsertDevice(t.Context(), central.Device{ID: item.id, Name: item.id, Status: "ready",
			Environment: "production", Host: "192.0.2.1", SSHPort: 22, SSHUser: "root",
			Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
		governanceNodes = append(governanceNodes, central.LineNode{DeviceID: item.id, Role: item.role})
	}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: "governance-line", Environment: "production",
		ResourceGroup: "managed", InstanceID: "governance_1", BandwidthMbps: 10, UpstreamMbps: 10,
		DownstreamMbps: 10, SocksPort: 1082, RelayPort: 4445, ExitPort: 4443,
		UDPPortMin: 22048, UDPPortMax: 23071, Whitelist: json.RawMessage(`[]`),
		DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", JumpPolicy: "auto",
		Nodes: governanceNodes}); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"governance-clean-relay", "governance-clean-exit"} {
		if _, err = database.UpsertDevice(t.Context(), central.Device{ID: id, Name: id, Status: "ready",
			Environment: "production", Host: "192.0.2.2", SSHPort: 22, SSHUser: "root",
			Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	cleanLineID := "governance-clean"
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: cleanLineID, Name: "clean", Status: "active",
		Environment: "production", EntryRegion: "gz", ExitRegion: "uk", Provider: "test", CapacityMbps: 10,
		ActiveDeployment: "clean-deployment", Profile: cleanLineID + ":2"}); err != nil {
		t.Fatal(err)
	}
	cleanNodes := []central.LineNode{{DeviceID: "governance-entry", Role: "entry"},
		{DeviceID: "governance-clean-relay", Role: "relay"}, {DeviceID: "governance-clean-exit", Role: "exit"}}
	if _, err = database.SaveLineSpec(t.Context(), central.LineSpec{LineID: cleanLineID, Environment: "production",
		ResourceGroup: "managed", InstanceID: "governance_clean_1", BandwidthMbps: 10, UpstreamMbps: 10,
		DownstreamMbps: 10, SocksPort: 1083, RelayPort: 4447, ExitPort: 4445,
		UDPPortMin: 23072, UDPPortMax: 24095, Whitelist: json.RawMessage(`[]`),
		DNSServers: json.RawMessage(`["1.1.1.1"]`), BuildMode: "auto", JumpPolicy: "auto", Nodes: cleanNodes}); err != nil {
		t.Fatal(err)
	}
	for _, link := range []central.NetworkLink{
		{ID: "governance-clean-entry-relay", FromDeviceID: "governance-entry", FromRole: "entry",
			ToDeviceID: "governance-clean-relay", ToRole: "relay", ForwardCapacityMbps: 10,
			ReverseCapacityMbps: 10, BillingMode: central.LinkBillingIndependent, Environment: "production", Status: "ready"},
		{ID: "governance-clean-relay-exit", FromDeviceID: "governance-clean-relay", FromRole: "relay",
			ToDeviceID: "governance-clean-exit", ToRole: "exit", ForwardCapacityMbps: 10,
			ReverseCapacityMbps: 10, BillingMode: central.LinkBillingIndependent, Environment: "production", Status: "ready"},
	} {
		if _, err = database.UpsertNetworkLink(t.Context(), link); err != nil {
			t.Fatal(err)
		}
	}
	cleanOperation := central.Operation{ID: "op-governance-clean", LineID: cleanLineID, Kind: "line.optimize",
		RequestedBy: "test", IdempotencyKey: "governance-clean-qualification", Request: json.RawMessage(`{}`)}
	if _, _, err = database.CreateOperation(t.Context(), cleanOperation); err != nil {
		t.Fatal(err)
	}
	if _, err = database.ClaimOperations(t.Context(), cleanLineID, 1); err != nil {
		t.Fatal(err)
	}
	if err = database.CompleteOperation(t.Context(), cleanOperation.ID, cleanLineID, "succeeded",
		json.RawMessage(`{"profile":"governance-clean:2"}`)); err != nil {
		t.Fatal(err)
	}
	if _, err = database.SaveLineQualification(t.Context(), central.LineQualification{LineID: cleanLineID,
		DeploymentID: "clean-deployment", OperationID: cleanOperation.ID, TargetUpstreamMbps: 10,
		TargetDownstreamMbps: 10, AchievedUpstreamMbps: 10, AchievedDownstreamMbps: 10,
		DurationSeconds: 90, RequiredRatio: 0.95, Status: "admitted", Reasons: json.RawMessage(`[]`),
		Evidence: json.RawMessage(`{}`)}); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/governance/production-lines", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"line_id":"governance-line"`)) ||
		!bytes.Contains(body, []byte(`"code":"capacity_unknown"`)) ||
		!bytes.Contains(body, []byte(`"code":"qualification_required"`)) ||
		bytes.Contains(body, []byte(`"line_id":"governance-test"`)) || !bytes.Contains(body, []byte(`"required_action"`)) {
		t.Fatalf("governance status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin",
		"governance-upgrade", map[string]any{"line_id": "governance-line", "kind": "line.upgrade",
			"requested_by": "operator", "request": map[string]any{}})
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("禁止升级")) {
		t.Fatalf("governance upgrade status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPost, server.URL+"/api/v1/operations", "admin",
		"governance-shared-upgrade", map[string]any{"line_id": cleanLineID, "kind": "line.upgrade",
			"requested_by": "operator", "request": map[string]any{}})
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("governance-line")) {
		t.Fatalf("shared governance upgrade status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/lines/governance-line/spec", "admin", "",
		map[string]any{"environment": "production", "upstream_mbps": 11, "downstream_mbps": 10})
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("禁止扩容")) {
		t.Fatalf("governance expansion status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPatch, server.URL+"/api/v1/lines/"+cleanLineID, "admin", "",
		map[string]any{"active_deployment": "forged-deployment"})
	if response.StatusCode != http.StatusBadRequest || !bytes.Contains(body, []byte("unsupported field")) {
		t.Fatalf("governance metadata patch status=%d body=%s", response.StatusCode, body)
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	for _, marker := range []string{"maintenance_required", "capacity_unknown", "full_duplex_unqualified",
		"qualification_required", "required_action", "生产治理"} {
		if !bytes.Contains(script, []byte(marker)) {
			t.Fatalf("governance UI missing %q", marker)
		}
	}
}

func TestAgentDiscoversMissingTopologyAndIncompleteLineCanBeDeleted(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	client := server.Client()
	legacy := map[string]any{"id": "legacy-line", "name": "legacy", "status": "active", "entry_region": "gz",
		"exit_region": "us", "provider": "mixed", "capacity_mbps": 10, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body := call(t, client, http.MethodPost, server.URL+"/api/v1/lines", "admin", "", legacy)
	if response.StatusCode != 201 {
		t.Fatalf("create legacy line status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodDelete, server.URL+"/api/v1/lines/legacy-line", "admin", "",
		map[string]string{"requested_by": "operator", "reason": "incomplete legacy record"})
	if response.StatusCode != http.StatusNoContent {
		t.Fatalf("delete incomplete line status=%d body=%s", response.StatusCode, body)
	}
	devices := []map[string]any{
		{"id": "entry-1", "name": "entry-1", "status": "ready", "host": "192.0.2.1", "ssh_port": 22, "ssh_user": "root", "private_ip": "", "region": "gz", "provider": "test", "os": "", "arch": "", "secret_ref": "worker-local:line-1:entry", "labels": map[string]any{}},
		{"id": "relay-1", "name": "relay-1", "status": "ready", "host": "192.0.2.2", "ssh_port": 22, "ssh_user": "root", "private_ip": "", "region": "hk", "provider": "test", "os": "", "arch": "", "secret_ref": "worker-local:line-1:relay", "labels": map[string]any{}},
		{"id": "exit-1", "name": "exit-1", "status": "ready", "host": "192.0.2.3", "ssh_port": 2273, "ssh_user": "root", "private_ip": "", "region": "us", "provider": "test", "os": "", "arch": "", "secret_ref": "worker-local:line-1:exit", "labels": map[string]any{}},
	}
	nodes := []map[string]any{
		{"device_id": "entry-1", "role": "entry", "ordinal": 0, "next_hop_device_id": "relay-1", "jump_candidates": []string{}, "config": map[string]any{}},
		{"device_id": "relay-1", "role": "relay", "ordinal": 0, "next_hop_device_id": "exit-1", "jump_candidates": []string{"entry-1"}, "config": map[string]any{}},
		{"device_id": "exit-1", "role": "exit", "ordinal": 0, "next_hop_device_id": "", "jump_candidates": []string{"relay-1"}, "config": map[string]any{}},
	}
	discovery := map[string]any{"worker_id": "worker-1", "observed_at": time.Now().UTC(), "lines": []map[string]any{{
		"line":    map[string]any{"id": "line-1", "name": "line-1", "status": "maintenance", "entry_region": "gz", "exit_region": "us", "provider": "mixed", "capacity_mbps": 10, "active_deployment": "", "profile": "", "secret_ref": ""},
		"devices": devices, "spec": map[string]any{"line_id": "line-1", "resource_group": "shared", "instance_id": "us", "bandwidth_mbps": 10, "socks_port": 1082, "udp_port_min": 22048, "udp_port_max": 23071, "relay_port": 4445, "exit_port": 4443, "exit_bind_ip": "192.0.2.3", "whitelist": []string{}, "build_mode": "source", "artifact_ref": "build/nb_node", "source_ref": "repo://current", "srs_ref": "", "jump_policy": "auto", "nodes": nodes},
	}}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/inventory", "agent", "", discovery)
	if response.StatusCode != http.StatusAccepted || !bytes.Contains(body, []byte(`"status":"discovered"`)) {
		t.Fatalf("inventory discovery status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/api/v1/lines/line-1/detail", "admin", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"device_id":"entry-1"`)) || bytes.Contains(body, []byte("entry-secret")) {
		t.Fatalf("discovered topology status=%d body=%s", response.StatusCode, body)
	}
	manual := devices[0]
	manual["name"] = "operator managed entry"
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/devices", "admin", "", manual)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("manual device update status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/inventory", "agent", "", discovery)
	if response.StatusCode != http.StatusAccepted {
		t.Fatalf("repeat discovery status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/api/v1/devices/entry-1", "admin", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"name":"operator managed entry"`)) {
		t.Fatalf("discovery overwrote operator device status=%d body=%s", response.StatusCode, body)
	}
}

func TestAgentRuntimePortClaimsProtectAutomaticAndExplicitPorts(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	client := server.Client()
	for _, device := range []map[string]any{
		{"id": "entry-runtime", "name": "entry", "status": "ready", "host": "192.0.2.10", "ssh_port": 22, "ssh_user": "root", "secret_ref": "device:entry-runtime", "labels": map[string]any{}},
		{"id": "relay-runtime", "name": "relay", "status": "ready", "host": "192.0.2.11", "ssh_port": 22, "ssh_user": "root", "secret_ref": "device:relay-runtime", "labels": map[string]any{}},
		{"id": "exit-runtime", "name": "exit", "status": "ready", "host": "192.0.2.12", "ssh_port": 22, "ssh_user": "root", "secret_ref": "device:exit-runtime", "labels": map[string]any{}},
	} {
		if _, err = database.UpsertDevice(t.Context(), central.Device{ID: device["id"].(string), Name: device["name"].(string), Status: "ready",
			Host: device["host"].(string), SSHPort: 22, SSHUser: "root", SecretRef: device["secret_ref"].(string), Labels: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-runtime", Name: "runtime", Status: "draft", Environment: "test", Provider: "test"}); err != nil {
		t.Fatal(err)
	}
	observed := time.Now().UTC()
	batch := map[string]any{"worker_id": "worker-1", "device_id": "entry-runtime", "role": "entry", "scan_complete": true,
		"observed_at": observed.Format(time.RFC3339Nano), "claims": []map[string]any{{"resource_kind": "socks", "instance_id": "legacy-runtime",
			"port_start": 1082, "port_end": 1083, "source": "runtime"}}}
	response, body := call(t, client, http.MethodPost, server.URL+"/agent/v1/runtime-port-claims", "agent", "", batch)
	if response.StatusCode != http.StatusAccepted {
		t.Fatalf("claim upload status=%d body=%s", response.StatusCode, body)
	}
	nodes := []map[string]any{{"device_id": "entry-runtime", "role": "entry", "ordinal": 0},
		{"device_id": "relay-runtime", "role": "relay", "ordinal": 0}, {"device_id": "exit-runtime", "role": "exit", "ordinal": 0}}
	autoSpec := map[string]any{"resource_group": "shared", "environment": "test", "instance_id": "line-runtime_1", "bandwidth_mbps": 5,
		"upstream_mbps": 5, "downstream_mbps": 5, "socks_port": 0, "relay_port": 0, "exit_port": 0,
		"udp_port_min": 0, "udp_port_max": 0, "dns_servers": []string{"1.1.1.1"}, "whitelist": []string{},
		"build_mode": "auto", "source_ref": "repo://current", "jump_policy": "auto", "nodes": nodes}
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-runtime/spec", "admin", "", autoSpec)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"socks_port":1084`)) || !bytes.Contains(body, []byte(`"socks_port_auto":true`)) {
		t.Fatalf("claim-aware allocation status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/agent/v1/runtime-port-scan-plans", "agent", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte("line-runtime")) {
		t.Fatalf("scan plans status=%d body=%s", response.StatusCode, body)
	}
	heartbeat := map[string]any{"worker_id": "worker-1", "status": "ready", "version": "test", "observed_at": time.Now().UTC(),
		"lines": []map[string]any{{"line_id": "*", "operations": []string{"line.open"}}}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/executors/heartbeat", "agent", "", heartbeat)
	if response.StatusCode != http.StatusOK {
		t.Fatalf("heartbeat status=%d body=%s", response.StatusCode, body)
	}
	operationRequest := map[string]any{"line_id": "line-runtime", "kind": "line.open", "requested_by": "operator", "request": map[string]any{}}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/operations", "admin", "runtime-preflight", operationRequest)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("operation create status=%d body=%s", response.StatusCode, body)
	}
	var operation central.Operation
	if err = json.Unmarshal(body, &operation); err != nil {
		t.Fatal(err)
	}
	response, body = call(t, client, http.MethodGet, server.URL+"/agent/v1/operations?line_id=*&limit=1", "agent", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(operation.ID)) {
		t.Fatalf("operation claim status=%d body=%s", response.StatusCode, body)
	}
	batch["claims"] = []map[string]any{{"resource_kind": "socks", "instance_id": "legacy-runtime", "port_start": 1082, "port_end": 1084, "source": "runtime"}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/runtime-port-claims", "agent", "", batch)
	if response.StatusCode != http.StatusAccepted {
		t.Fatalf("late claim status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/operations/"+operation.ID+"/port-preflight", "agent", "",
		map[string]any{"line_id": "line-runtime", "worker_id": "worker-1"})
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"socks_port":1085`)) {
		t.Fatalf("port preflight status=%d body=%s", response.StatusCode, body)
	}
}
