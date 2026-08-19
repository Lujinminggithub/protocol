package webapp

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
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

func TestClientConfigurationAndQRCode(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()

	_, err = database.UpsertLine(t.Context(), central.Line{ID: "line-1", Name: "test", Status: "draft",
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
	line := map[string]any{"id": "line-unique", "name": "unique", "status": "draft", "entry_region": "entry",
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
	if err != nil || stored.Name != "unique" {
		t.Fatalf("duplicate create changed line: %+v err=%v", stored, err)
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

	line := map[string]any{"id": "gz-hk-us", "name": "GZ / HK / US", "status": "active", "entry_region": "Guangzhou",
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
	response, body = call(t, server.Client(), http.MethodGet, server.URL+"/app.js", "", "", nil)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`operationAvailability`)) ||
		!bytes.Contains(body, []byte(`disabled title=`)) ||
		!bytes.Contains(body, []byte(`computer-icon`)) || !bytes.Contains(body, []byte(`队列最大等待`)) ||
		!bytes.Contains(body, []byte(`clientConfigSection`)) || !bytes.Contains(body, []byte(`/client-qr`)) ||
		!bytes.Contains(body, []byte(`upstream_mbps`)) || !bytes.Contains(body, []byte(`downstream_mbps`)) ||
		!bytes.Contains(body, []byte(`item.role === "entry"`)) ||
		!bytes.Contains(body, []byte(`tuneResultSection`)) || !bytes.Contains(body, []byte(`transport_rollout`)) ||
		bytes.Contains(body, []byte(`event.currentTarget.reset()`)) {
		t.Fatalf("line lifecycle actions missing from UI status=%d", response.StatusCode)
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
	line := map[string]any{"id": "line-1", "name": "test line", "status": "draft", "entry_region": "entry",
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
	spec := map[string]any{"resource_group": "test-group", "instance_id": "test", "bandwidth_mbps": 20,
		"socks_port": 1082, "relay_port": 4445, "exit_port": 4443, "exit_bind_ip": "192.0.2.3", "udp_port_min": 22048, "udp_port_max": 23071,
		"whitelist": []string{"domain example.com"}, "build_mode": "auto", "artifact_ref": "", "source_ref": "repo://current",
		"srs_ref": "https://rules.example.invalid/whitelist.srs?key=test-key", "jump_policy": "auto", "nodes": nodes}
	response, body = call(t, client, http.MethodPut, server.URL+"/api/v1/lines/line-1/spec", "admin", "", spec)
	if response.StatusCode != 200 || !bytes.Contains(body, []byte(`"exit_bind_ip":"192.0.2.3"`)) {
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
	autoLine := map[string]any{"id": "line-auto", "name": "auto allocated line", "status": "draft", "entry_region": "entry",
		"exit_region": "exit", "provider": "test", "capacity_mbps": 20, "active_deployment": "", "profile": "", "secret_ref": ""}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/lines", "admin", "", autoLine)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create auto line status=%d body=%s", response.StatusCode, body)
	}
	autoSpec := map[string]any{"resource_group": "other-group", "instance_id": "", "bandwidth_mbps": 20,
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
		"lines": []map[string]any{{"line_id": "*", "operations": []string{"line.open", "line.validate", "line.upgrade", "line.rollback", "line.disable", "line.tune"}}}}
	response, body = call(t, client, http.MethodPost, server.URL+"/agent/v1/executors/heartbeat", "agent", "", heartbeat)
	if response.StatusCode != 200 {
		t.Fatalf("heartbeat status=%d body=%s", response.StatusCode, body)
	}
	operation := map[string]any{"line_id": "line-1", "kind": "line.open", "requested_by": "test", "request": map[string]any{}}
	response, body = call(t, client, http.MethodPost, server.URL+"/api/v1/operations", "admin", "inventory-test", operation)
	if response.StatusCode != 201 || !bytes.Contains(body, []byte(`"plan"`)) {
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
