package central

import (
	"encoding/json"
	"path/filepath"
	"slices"
	"testing"
)

func platformTestSpec(lineID string, offset int, nodes ...LineNode) LineSpec {
	return LineSpec{LineID: lineID, Environment: "production", ResourceGroup: "mutable-name",
		InstanceID: lineID + "_1", BandwidthMbps: 10, UpstreamMbps: 10, DownstreamMbps: 10,
		SocksPort: 1082 + offset, UDPPortMin: 22048 + offset*1024, UDPPortMax: 23071 + offset*1024,
		RelayPort: 4445 + offset*2, ExitPort: 4443 + offset*2,
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto",
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`[]`), Nodes: nodes}
}

func TestPlatformUpgradeImpactUsesPhysicalDeviceClosure(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	lines := []struct {
		id    string
		nodes []LineNode
	}{
		{"line-a", []LineNode{{DeviceID: "entry-a", Role: "entry"}, {DeviceID: "middle-shared", Role: "relay"}, {DeviceID: "exit-a", Role: "exit"}}},
		{"line-b", []LineNode{{DeviceID: "entry-b", Role: "entry"}, {DeviceID: "middle-shared", Role: "relay"}, {DeviceID: "exit-shared", Role: "exit"}}},
		{"line-c", []LineNode{{DeviceID: "entry-c", Role: "entry"}, {DeviceID: "middle-c", Role: "relay"}, {DeviceID: "exit-shared", Role: "exit"}}},
		{"line-unrelated", []LineNode{{DeviceID: "entry-z", Role: "entry"}, {DeviceID: "middle-z", Role: "relay"}, {DeviceID: "exit-z", Role: "exit"}}},
	}
	devices := map[string]bool{}
	for _, item := range lines {
		for _, node := range item.nodes {
			devices[node.DeviceID] = true
		}
	}
	for deviceID := range devices {
		if _, err = store.UpsertDevice(t.Context(), Device{ID: deviceID, Name: deviceID, Status: "ready",
			Host: "127.0.0.1", SSHPort: 22, SSHUser: "root", Environment: "production"}); err != nil {
			t.Fatal(err)
		}
	}
	for index, item := range lines {
		if _, err = store.UpsertLine(t.Context(), Line{ID: item.id, Name: item.id, Status: "active", Environment: "production", Provider: "test"}); err != nil {
			t.Fatal(err)
		}
		if _, err = store.SaveLineSpec(t.Context(), platformTestSpec(item.id, index, item.nodes...)); err != nil {
			t.Fatal(err)
		}
	}
	impact, err := store.PlatformUpgradeImpact(t.Context(), "line-a")
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(impact.Lines, []string{"line-a", "line-b", "line-c"}) {
		t.Fatalf("impact lines=%v", impact.Lines)
	}
	if slices.Contains(impact.Lines, "line-unrelated") || len(impact.Devices) != 7 {
		t.Fatalf("impact=%+v", impact)
	}
}
