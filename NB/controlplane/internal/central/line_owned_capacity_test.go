package central

import (
	"encoding/json"
	"path/filepath"
	"testing"
)

func seedLineOwnedCapacity(t *testing.T, store *Store, lineID string) {
	t.Helper()
	for _, device := range []struct {
		id, region string
	}{{"entry-owned", "CN-GZ"}, {"relay-owned", "HK"}, {"exit-owned", "ES"}} {
		if _, err := store.UpsertDevice(t.Context(), Device{ID: device.id, Name: device.id, Status: "ready",
			Environment: "production", Host: "127.0.0.1", SSHPort: 22, SSHUser: "root", Region: device.region}); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := store.UpsertLine(t.Context(), Line{ID: lineID, Name: lineID, Status: "active",
		Environment: "production", Provider: "test", ActiveDeployment: "deployment-1"}); err != nil {
		t.Fatal(err)
	}
	_, err := store.SaveLineSpec(t.Context(), LineSpec{LineID: lineID, Environment: "production",
		TopologyMode: "trihop", ServiceProfile: "tiktok_live", ResourceGroup: "display-only",
		InstanceID: lineID + "_1", BandwidthMbps: 12, UpstreamMbps: 7, DownstreamMbps: 12,
		SocksPort: 1082, UDPPortMin: 22048, UDPPortMax: 23071, RelayPort: 4445, ExitPort: 4443,
		BuildMode: "auto", SourceRef: "repo://current", JumpPolicy: "auto",
		Whitelist: json.RawMessage(`[]`), DNSServers: json.RawMessage(`[]`), Nodes: []LineNode{
			{DeviceID: "entry-owned", Role: "entry", Ordinal: 0, NextHopDevice: "relay-owned"},
			{DeviceID: "relay-owned", Role: "relay", Ordinal: 0, NextHopDevice: "exit-owned"},
			{DeviceID: "exit-owned", Role: "exit", Ordinal: 0},
		}})
	if err != nil {
		t.Fatal(err)
	}
}

func TestLineOwnedCapacityRequiresNoPhysicalLinksOrReservations(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	seedLineOwnedCapacity(t, store, "line-owned")
	reservations, err := store.ReserveLineCapacity(t.Context(), "line-owned", "op-open")
	if err != nil || len(reservations) != 0 {
		t.Fatalf("line-owned capacity created reservations=%+v err=%v", reservations, err)
	}
	var count int
	if err = store.db.QueryRowContext(t.Context(), `SELECT COUNT(*) FROM line_capacity_reservations WHERE line_id=?`, "line-owned").Scan(&count); err != nil || count != 0 {
		t.Fatalf("historical reservation table changed count=%d err=%v", count, err)
	}
}

func TestLineOwnedGovernanceIgnoresPhysicalLinks(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	seedLineOwnedCapacity(t, store, "line-governance")
	findings, err := store.ProductionLineFindings(t.Context(), "line-governance")
	if err != nil {
		t.Fatal(err)
	}
	for _, finding := range findings {
		if finding.Code == "capacity_unknown" || finding.Code == "full_duplex_unqualified" ||
			finding.Code == "capacity_insufficient" {
			t.Fatalf("physical-link governance remains: %+v", finding)
		}
	}
}

func TestActiveProductionLineNeedsNoHistoricalQualificationButStillEnforcesDeviceEnvironment(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	seedLineOwnedCapacity(t, store, "line-without-qualification")

	findings, err := store.ProductionLineFindings(t.Context(), "line-without-qualification")
	if err != nil {
		t.Fatal(err)
	}
	if len(findings) != 0 {
		t.Fatalf("active production line inherited obsolete qualification findings: %+v", findings)
	}
	if err = store.ClientDeliveryAllowed(t.Context(), "line-without-qualification"); err != nil {
		t.Fatalf("active production line without qualification was blocked: %v", err)
	}

	entry, err := store.Device(t.Context(), "entry-owned")
	if err != nil {
		t.Fatal(err)
	}
	entry.Environment = "test"
	if _, err = store.UpsertDevice(t.Context(), entry); err != nil {
		t.Fatal(err)
	}
	findings, err = store.ProductionLineFindings(t.Context(), "line-without-qualification")
	if err != nil {
		t.Fatal(err)
	}
	if len(findings) != 1 || findings[0].Code != "maintenance_required" || findings[0].ResourceID != entry.ID {
		t.Fatalf("device environment governance findings=%+v", findings)
	}
	if err = store.ClientDeliveryAllowed(t.Context(), "line-without-qualification"); err == nil {
		t.Fatal("production line using a test device was allowed")
	}
}
