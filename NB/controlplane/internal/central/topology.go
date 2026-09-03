package central

import (
	"context"
	"database/sql"
	"errors"
)

type TopologyDevice struct {
	ID         string          `json:"id"`
	Name       string          `json:"name"`
	Status     string          `json:"status"`
	Health     string          `json:"health"`
	Host       string          `json:"host"`
	PrivateIP  string          `json:"private_ip"`
	Region     string          `json:"region"`
	Provider   string          `json:"provider"`
	OS         string          `json:"os"`
	Arch       string          `json:"arch"`
	LastSeenAt string          `json:"last_seen_at"`
	Layout     *TopologyLayout `json:"layout,omitempty"`
}

type TopologyLayout struct {
	DeviceID  string  `json:"device_id"`
	X         float64 `json:"x"`
	Y         float64 `json:"y"`
	Z         float64 `json:"z"`
	UpdatedBy string  `json:"updated_by"`
	UpdatedAt string  `json:"updated_at"`
}

type TopologyLink struct {
	ID             string  `json:"id"`
	LineID         string  `json:"line_id"`
	LineName       string  `json:"line_name"`
	Source         string  `json:"source"`
	Target         string  `json:"target"`
	Role           string  `json:"role"`
	Health         string  `json:"health"`
	UpstreamMbps   float64 `json:"upstream_mbps"`
	DownstreamMbps float64 `json:"downstream_mbps"`
}

type TopologyResult struct {
	Devices []TopologyDevice `json:"devices"`
	Links   []TopologyLink   `json:"links"`
}

func snapshotRoleHealth(items []Snapshot, role string) (string, float64, float64) {
	if role == "relay" {
		role = "middle"
	}
	found, rank := false, 0
	var upstream, downstream float64
	for _, item := range items {
		if item.Role != role {
			continue
		}
		found = true
		upstream += item.UpstreamMbps
		downstream += item.DownstreamMbps
		if current := healthRank(item.Health); current > rank {
			rank = current
		}
	}
	if !found {
		return "unknown", 0, 0
	}
	return healthFromRank(rank), upstream, downstream
}

func (s *Store) Topology(ctx context.Context) (TopologyResult, error) {
	result := TopologyResult{Devices: []TopologyDevice{}, Links: []TopologyLink{}}
	layouts, err := s.topologyLayouts(ctx)
	if err != nil {
		return result, err
	}
	devices, err := s.Devices(ctx)
	if err != nil {
		return result, err
	}
	for _, item := range devices {
		result.Devices = append(result.Devices, TopologyDevice{ID: item.ID, Name: item.Name, Status: item.Status,
			Health: item.LastHealth, Host: item.Host, PrivateIP: item.PrivateIP, Region: item.Region,
			Provider: item.Provider, OS: item.OS, Arch: item.Arch, LastSeenAt: item.LastSeenAt,
			Layout: layouts[item.ID]})
	}
	lines, err := s.Lines(ctx)
	if err != nil {
		return result, err
	}
	for _, line := range lines {
		spec, specErr := s.LineSpec(ctx, line.ID)
		if errors.Is(specErr, sql.ErrNoRows) {
			continue
		}
		if specErr != nil {
			return result, specErr
		}
		snapshots, snapshotErr := s.LatestSnapshots(ctx, line.ID)
		if snapshotErr != nil {
			return result, snapshotErr
		}
		for _, node := range spec.Nodes {
			if node.NextHopDevice == "" {
				continue
			}
			health, upstream, downstream := snapshotRoleHealth(snapshots, node.Role)
			result.Links = append(result.Links, TopologyLink{ID: line.ID + ":" + node.DeviceID + ":" + node.NextHopDevice,
				LineID: line.ID, LineName: line.Name, Source: node.DeviceID, Target: node.NextHopDevice,
				Role: node.Role, Health: health, UpstreamMbps: upstream, DownstreamMbps: downstream})
		}
	}
	return result, nil
}

func (s *Store) topologyLayouts(ctx context.Context) (map[string]*TopologyLayout, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT device_id,x,y,z,updated_by,updated_at FROM topology_layouts`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make(map[string]*TopologyLayout)
	for rows.Next() {
		item := &TopologyLayout{}
		if err = rows.Scan(&item.DeviceID, &item.X, &item.Y, &item.Z, &item.UpdatedBy, &item.UpdatedAt); err != nil {
			return nil, err
		}
		result[item.DeviceID] = item
	}
	return result, rows.Err()
}

func (s *Store) SaveTopologyLayouts(ctx context.Context, items []TopologyLayout) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	updatedAt := now()
	for _, item := range items {
		query := s.controlSQL(`INSERT INTO topology_layouts(device_id,x,y,z,updated_by,updated_at)
 VALUES(?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
	 x=excluded.x,y=excluded.y,z=excluded.z,updated_by=excluded.updated_by,updated_at=excluded.updated_at`,
			`INSERT INTO topology_layouts(device_id,x,y,z,updated_by,updated_at) VALUES(?,?,?,?,?,?)
 ON DUPLICATE KEY UPDATE x=VALUES(x),y=VALUES(y),z=VALUES(z),updated_by=VALUES(updated_by),updated_at=VALUES(updated_at)`)
		if _, err = tx.ExecContext(ctx, query,
			item.DeviceID, item.X, item.Y, item.Z, item.UpdatedBy, updatedAt); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (s *Store) ResetTopologyLayouts(ctx context.Context) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM topology_layouts`)
	return err
}
