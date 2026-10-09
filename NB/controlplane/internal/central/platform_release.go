package central

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"fmt"
	"sort"
)

type PlatformUpgradeDevice struct {
	DeviceID string `json:"device_id"`
	Role     string `json:"role"`
}

type PlatformUpgradeImpact struct {
	UnitID  string                  `json:"unit_id"`
	Lines   []string                `json:"lines"`
	Devices []PlatformUpgradeDevice `json:"devices"`
}

func upgradeDeviceKey(deviceID, role string) string { return deviceID + "\x00" + role }

// PlatformUpgradeImpact returns the transitive blast radius of shared physical shard roles.
func (s *Store) PlatformUpgradeImpact(ctx context.Context, seedLineID string) (PlatformUpgradeImpact, error) {
	specs, err := s.ActiveLineSpecs(ctx)
	if err != nil {
		return PlatformUpgradeImpact{}, err
	}
	byLine := make(map[string]LineSpec, len(specs))
	for _, spec := range specs {
		byLine[spec.LineID] = spec
	}
	seed, ok := byLine[seedLineID]
	if !ok {
		return PlatformUpgradeImpact{}, sql.ErrNoRows
	}
	selectedLines := map[string]bool{seedLineID: true}
	selectedDevices := map[string]PlatformUpgradeDevice{}
	addNodes := func(spec LineSpec) {
		for _, node := range spec.Nodes {
			key := upgradeDeviceKey(node.DeviceID, node.Role)
			selectedDevices[key] = PlatformUpgradeDevice{DeviceID: node.DeviceID, Role: node.Role}
		}
	}
	addNodes(seed)
	for changed := true; changed; {
		changed = false
		for _, spec := range specs {
			if selectedLines[spec.LineID] {
				continue
			}
			shares := false
			for _, node := range spec.Nodes {
				if _, exists := selectedDevices[upgradeDeviceKey(node.DeviceID, node.Role)]; exists {
					shares = true
					break
				}
			}
			if shares {
				selectedLines[spec.LineID] = true
				addNodes(spec)
				changed = true
			}
		}
	}
	impact := PlatformUpgradeImpact{}
	for lineID := range selectedLines {
		impact.Lines = append(impact.Lines, lineID)
	}
	for _, device := range selectedDevices {
		impact.Devices = append(impact.Devices, device)
	}
	sort.Strings(impact.Lines)
	sort.Slice(impact.Devices, func(i, j int) bool {
		if impact.Devices[i].DeviceID == impact.Devices[j].DeviceID {
			return impact.Devices[i].Role < impact.Devices[j].Role
		}
		return impact.Devices[i].DeviceID < impact.Devices[j].DeviceID
	})
	digest := sha256.New()
	for _, device := range impact.Devices {
		_, _ = fmt.Fprintf(digest, "%s\x00%s\n", device.DeviceID, device.Role)
	}
	impact.UnitID = fmt.Sprintf("unit-%x", digest.Sum(nil)[:8])
	return impact, nil
}

func (s *Store) HasActivePlatformOperation(ctx context.Context) (bool, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM operations
 WHERE line_id='__platform__' AND status IN ('queued','dispatched','running')`).Scan(&count)
	return count > 0, err
}
