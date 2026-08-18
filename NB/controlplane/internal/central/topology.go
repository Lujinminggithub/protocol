package central

import (
	"context"
	"database/sql"
	"errors"
)

type TopologyDevice struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Status     string `json:"status"`
	Health     string `json:"health"`
	Host       string `json:"host"`
	PrivateIP  string `json:"private_ip"`
	Region     string `json:"region"`
	Provider   string `json:"provider"`
	OS         string `json:"os"`
	Arch       string `json:"arch"`
	LastSeenAt string `json:"last_seen_at"`
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
	devices, err := s.Devices(ctx)
	if err != nil {
		return result, err
	}
	for _, item := range devices {
		result.Devices = append(result.Devices, TopologyDevice{ID: item.ID, Name: item.Name, Status: item.Status,
			Health: item.LastHealth, Host: item.Host, PrivateIP: item.PrivateIP, Region: item.Region,
			Provider: item.Provider, OS: item.OS, Arch: item.Arch, LastSeenAt: item.LastSeenAt})
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
