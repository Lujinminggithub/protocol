package webapp

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"nb-controlplane/internal/central"
)

func TestPhysicalLinkMutationIsRetiredAndReadRemainsCompatible(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodGet, server.URL+"/api/v1/network-links", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"links":[]`)) {
		t.Fatalf("compat read status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodPut, server.URL+"/api/v1/network-links/retired", "admin", "", map[string]any{
		"from_device_id": "entry", "from_role": "entry", "to_device_id": "relay", "to_role": "relay",
		"forward_capacity_mbps": 10, "reverse_capacity_mbps": 10, "billing_mode": "independent-directions",
		"environment": "production", "status": "ready",
	})
	if response.StatusCode != http.StatusGone || !bytes.Contains(body, []byte("线路")) {
		t.Fatalf("retired mutation status=%d body=%s", response.StatusCode, body)
	}
}

func TestLineOwnedCapacityAssetsRemoveLinksAndPreserveTrafficAnalysis(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	read := func(path string) []byte {
		response, requestErr := server.Client().Get(server.URL + path)
		if requestErr != nil {
			t.Fatal(requestErr)
		}
		defer response.Body.Close()
		data, _ := io.ReadAll(response.Body)
		return data
	}
	index := read("/")
	for _, removed := range []string{`data-view="links"`, `id="linksView"`, `id="networkLinkModal"`} {
		if bytes.Contains(index, []byte(removed)) {
			t.Fatalf("retired physical-link UI remains: %s", removed)
		}
	}
	for _, required := range []string{`name="upstream_mbps"`, `name="downstream_mbps"`,
		`id="trafficWorkspace"`, `证据时间轴`} {
		if !bytes.Contains(index, []byte(required)) {
			t.Fatalf("line/traffic UI missing: %s", required)
		}
	}
	charts := read("/traffic-charts.js")
	for _, required := range []string{"Entry 接入", "Middle 中继", "Exit 出口", `data-chart="overview"`} {
		if !bytes.Contains(charts, []byte(required)) {
			t.Fatalf("traffic analysis contract changed: %s", required)
		}
	}
}
