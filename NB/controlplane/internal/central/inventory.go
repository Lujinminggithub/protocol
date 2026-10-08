package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

type Device struct {
	ID                    string          `json:"id"`
	Name                  string          `json:"name"`
	Status                string          `json:"status"`
	Environment           string          `json:"environment"`
	Host                  string          `json:"host"`
	SSHPort               int             `json:"ssh_port"`
	SSHUser               string          `json:"ssh_user"`
	SSHHostKey            string          `json:"ssh_host_key"`
	SSHHostKeyType        string          `json:"ssh_host_key_type"`
	SSHHostKeySHA256      string          `json:"ssh_host_key_sha256"`
	SSHHostKeyStatus      string          `json:"ssh_host_key_status"`
	SSHHostKeyConfirmedAt string          `json:"ssh_host_key_confirmed_at"`
	PrivateIP             string          `json:"private_ip"`
	Region                string          `json:"region"`
	Provider              string          `json:"provider"`
	OS                    string          `json:"os"`
	Arch                  string          `json:"arch"`
	SecretRef             string          `json:"secret_ref"`
	Labels                json.RawMessage `json:"labels"`
	LastHealth            string          `json:"last_health"`
	LastSeenAt            string          `json:"last_seen_at"`
	CreatedAt             string          `json:"created_at"`
	UpdatedAt             string          `json:"updated_at"`
}

type LineNode struct {
	DeviceID       string          `json:"device_id"`
	Role           string          `json:"role"`
	Ordinal        int             `json:"ordinal"`
	NextHopDevice  string          `json:"next_hop_device_id"`
	JumpCandidates json.RawMessage `json:"jump_candidates"`
	Config         json.RawMessage `json:"config"`
	Device         *Device         `json:"device,omitempty"`
}

type LineSpec struct {
	LineID         string          `json:"line_id"`
	Environment    string          `json:"environment"`
	TopologyMode   string          `json:"topology_mode"`
	ServiceProfile string          `json:"service_profile"`
	ResourceGroup  string          `json:"resource_group"`
	InstanceID     string          `json:"instance_id"`
	BandwidthMbps  int             `json:"bandwidth_mbps"`
	UpstreamMbps   int             `json:"upstream_mbps"`
	DownstreamMbps int             `json:"downstream_mbps"`
	SocksPort      int             `json:"socks_port"`
	SocksPortAuto  bool            `json:"socks_port_auto"`
	UDPPortMin     int             `json:"udp_port_min"`
	UDPPortMax     int             `json:"udp_port_max"`
	UDPPortsAuto   bool            `json:"udp_ports_auto"`
	RelayPort      int             `json:"relay_port"`
	RelayPortAuto  bool            `json:"relay_port_auto"`
	ExitPort       int             `json:"exit_port"`
	ExitPortAuto   bool            `json:"exit_port_auto"`
	ExitBindIP     string          `json:"exit_bind_ip"`
	DNSServers     json.RawMessage `json:"dns_servers"`
	Whitelist      json.RawMessage `json:"whitelist"`
	BuildMode      string          `json:"build_mode"`
	ArtifactRef    string          `json:"artifact_ref"`
	SourceRef      string          `json:"source_ref"`
	SRSRef         string          `json:"srs_ref"`
	JumpPolicy     string          `json:"jump_policy"`
	Nodes          []LineNode      `json:"nodes"`
	CreatedAt      string          `json:"created_at"`
	UpdatedAt      string          `json:"updated_at"`
}

type RuntimePortClaim struct {
	WorkerID     string `json:"worker_id"`
	DeviceID     string `json:"device_id"`
	Role         string `json:"role"`
	ResourceKind string `json:"resource_kind"`
	InstanceID   string `json:"instance_id"`
	PortStart    int    `json:"port_start"`
	PortEnd      int    `json:"port_end"`
	ObservedAt   string `json:"observed_at"`
	ExpiresAt    string `json:"expires_at"`
	Source       string `json:"source"`
}

func (spec *LineSpec) NormalizeRates() {
	if spec.Environment == "" {
		spec.Environment = "production"
	}
	if spec.UpstreamMbps <= 0 {
		spec.UpstreamMbps = spec.BandwidthMbps
	}
	if spec.DownstreamMbps <= 0 {
		spec.DownstreamMbps = spec.BandwidthMbps
	}
	if spec.UpstreamMbps > spec.DownstreamMbps {
		spec.BandwidthMbps = spec.UpstreamMbps
	} else {
		spec.BandwidthMbps = spec.DownstreamMbps
	}
	var servers []string
	if len(spec.DNSServers) == 0 || json.Unmarshal(spec.DNSServers, &servers) != nil || len(servers) == 0 {
		spec.DNSServers = json.RawMessage(`["1.1.1.1","8.8.8.8"]`)
	}
}

func (spec *LineSpec) NormalizeTopology() error {
	if spec.TopologyMode == "" {
		spec.TopologyMode = "trihop"
	}
	if spec.ServiceProfile == "" {
		spec.ServiceProfile = "general"
	}
	if spec.TopologyMode != "trihop" && spec.TopologyMode != "single_hk" {
		return errors.New("invalid topology mode")
	}
	if spec.ServiceProfile != "general" && spec.ServiceProfile != "tiktok_live" {
		return errors.New("invalid service profile")
	}
	return nil
}

func validEnvironment(value string) bool { return value == "production" || value == "test" }

type OperationEvent struct {
	ID          int64           `json:"id"`
	OperationID string          `json:"operation_id"`
	Sequence    int             `json:"sequence"`
	Stage       string          `json:"stage"`
	Status      string          `json:"status"`
	Message     string          `json:"message"`
	Parameters  json.RawMessage `json:"parameters"`
	CreatedAt   string          `json:"created_at"`
}

func normalizedJSON(value json.RawMessage, fallback string) []byte {
	if len(value) == 0 || !json.Valid(value) {
		return []byte(fallback)
	}
	return []byte(value)
}

func scanDevice(row scanner, item *Device) error {
	var labels []byte
	err := row.Scan(&item.ID, &item.Name, &item.Status, &item.Host, &item.SSHPort,
		&item.SSHUser, &item.SSHHostKey, &item.SSHHostKeyType, &item.SSHHostKeySHA256,
		&item.SSHHostKeyStatus, &item.SSHHostKeyConfirmedAt, &item.PrivateIP, &item.Region, &item.Provider, &item.OS, &item.Arch,
		&item.SecretRef, &labels, &item.LastHealth, &item.LastSeenAt, &item.CreatedAt, &item.UpdatedAt, &item.Environment)
	item.Labels = json.RawMessage(labels)
	return err
}

const deviceColumns = `id,name,status,host,ssh_port,ssh_user,ssh_host_key,ssh_host_key_type,
 ssh_host_key_sha256,ssh_host_key_status,ssh_host_key_confirmed_at,private_ip,region,provider,os,arch,
 secret_ref,labels,last_health,last_seen_at,created_at,updated_at,environment`

func (s *Store) UpsertDevice(ctx context.Context, item Device) (Device, error) {
	if item.Environment == "" {
		item.Environment = "production"
	}
	if !validEnvironment(item.Environment) {
		return Device{}, errors.New("invalid device environment")
	}
	if item.SSHHostKeyStatus == "" {
		item.SSHHostKeyStatus = "pending"
	}
	stamp := now()
	query := s.controlSQL(`INSERT INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
 ON CONFLICT(id) DO UPDATE SET name=excluded.name,status=excluded.status,host=excluded.host,
	 ssh_port=excluded.ssh_port,ssh_user=excluded.ssh_user,ssh_host_key=excluded.ssh_host_key,
	 ssh_host_key_type=excluded.ssh_host_key_type,ssh_host_key_sha256=excluded.ssh_host_key_sha256,
	 ssh_host_key_status=excluded.ssh_host_key_status,ssh_host_key_confirmed_at=excluded.ssh_host_key_confirmed_at,
	 private_ip=excluded.private_ip,
	 region=excluded.region,provider=excluded.provider,os=excluded.os,arch=excluded.arch,
	 secret_ref=excluded.secret_ref,labels=excluded.labels,updated_at=excluded.updated_at,environment=excluded.environment`, `INSERT INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
 ON DUPLICATE KEY UPDATE name=VALUES(name),status=VALUES(status),host=VALUES(host),ssh_port=VALUES(ssh_port),
 ssh_user=VALUES(ssh_user),ssh_host_key=VALUES(ssh_host_key),ssh_host_key_type=VALUES(ssh_host_key_type),
 ssh_host_key_sha256=VALUES(ssh_host_key_sha256),ssh_host_key_status=VALUES(ssh_host_key_status),
 ssh_host_key_confirmed_at=VALUES(ssh_host_key_confirmed_at),private_ip=VALUES(private_ip),region=VALUES(region),
	 provider=VALUES(provider),os=VALUES(os),arch=VALUES(arch),secret_ref=VALUES(secret_ref),labels=VALUES(labels),
	 updated_at=VALUES(updated_at),environment=VALUES(environment)`)
	_, err := s.db.ExecContext(ctx, query,
		item.ID, item.Name, item.Status, item.Host, item.SSHPort, item.SSHUser,
		item.SSHHostKey, item.SSHHostKeyType, item.SSHHostKeySHA256, item.SSHHostKeyStatus, item.SSHHostKeyConfirmedAt, item.PrivateIP,
		item.Region, item.Provider, item.OS, item.Arch, item.SecretRef,
		normalizedJSON(item.Labels, `{}`), item.LastHealth, item.LastSeenAt, stamp, stamp, item.Environment)
	if err != nil {
		return Device{}, err
	}
	return s.Device(ctx, item.ID)
}

// EnsureDevice adds worker-discovered inventory without overwriting operator-managed fields.
func (s *Store) EnsureDevice(ctx context.Context, item Device) (Device, bool, error) {
	if item.Environment == "" {
		item.Environment = "production"
	}
	if !validEnvironment(item.Environment) {
		return Device{}, false, errors.New("invalid device environment")
	}
	if item.SSHHostKeyStatus == "" {
		item.SSHHostKeyStatus = "pending"
	}
	stamp := now()
	query := s.controlSQL(`INSERT OR IGNORE INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		`INSERT IGNORE INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`)
	result, err := s.db.ExecContext(ctx, query,
		item.ID, item.Name, item.Status, item.Host, item.SSHPort, item.SSHUser,
		item.SSHHostKey, item.SSHHostKeyType, item.SSHHostKeySHA256, item.SSHHostKeyStatus, item.SSHHostKeyConfirmedAt, item.PrivateIP,
		item.Region, item.Provider, item.OS, item.Arch, item.SecretRef,
		normalizedJSON(item.Labels, `{}`), item.LastHealth, item.LastSeenAt, stamp, stamp, item.Environment)
	if err != nil {
		return Device{}, false, err
	}
	count, err := result.RowsAffected()
	if err != nil {
		return Device{}, false, err
	}
	loaded, err := s.Device(ctx, item.ID)
	return loaded, count == 1, err
}

func (s *Store) Device(ctx context.Context, id string) (Device, error) {
	var item Device
	err := scanDevice(s.db.QueryRowContext(ctx, `SELECT `+deviceColumns+` FROM devices WHERE id=?`, id), &item)
	return item, err
}

func (s *Store) Devices(ctx context.Context) ([]Device, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+deviceColumns+` FROM devices ORDER BY region,name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Device
	for rows.Next() {
		var item Device
		if err = scanDevice(rows, &item); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) DeleteDevice(ctx context.Context, id string) error {
	result, err := s.db.ExecContext(ctx, `DELETE FROM devices WHERE id=? AND NOT EXISTS
 (SELECT 1 FROM line_nodes WHERE device_id=?)`, id, id)
	if err != nil {
		return err
	}
	count, _ := result.RowsAffected()
	if count != 1 {
		return errors.New("device is assigned to a line or does not exist")
	}
	return nil
}

func (s *Store) UpdateDeviceHealth(ctx context.Context, id, health string) error {
	seen := ""
	if health == "healthy" {
		seen = now()
	}
	_, err := s.db.ExecContext(ctx, `UPDATE devices SET last_health=?,last_seen_at=CASE WHEN ?='' THEN last_seen_at ELSE ? END,updated_at=? WHERE id=?`, health, seen, seen, now(), id)
	return err
}

func (s *Store) SaveLineSpec(ctx context.Context, spec LineSpec) (LineSpec, error) {
	spec.NormalizeRates()
	if err := spec.NormalizeTopology(); err != nil {
		return LineSpec{}, err
	}
	if !validEnvironment(spec.Environment) {
		return LineSpec{}, errors.New("invalid line environment")
	}
	s.specMu.Lock()
	defer s.specMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return LineSpec{}, err
	}
	defer tx.Rollback()
	if conflict, conflictErr := lineSpecConflict(ctx, tx, spec); conflictErr != nil {
		return LineSpec{}, conflictErr
	} else if conflict != "" {
		return LineSpec{}, errors.New(conflict)
	}
	stamp := now()
	query := s.controlSQL(`INSERT INTO line_specs
	 (line_id,resource_group,instance_id,bandwidth_mbps,upstream_mbps,downstream_mbps,socks_port,udp_port_min,udp_port_max,
	 relay_port,exit_port,exit_bind_ip,dns_servers,whitelist,build_mode,artifact_ref,source_ref,srs_ref,jump_policy,created_at,updated_at,environment,topology_mode,service_profile)
	 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(line_id) DO UPDATE SET
 resource_group=excluded.resource_group,instance_id=excluded.instance_id,
	 bandwidth_mbps=excluded.bandwidth_mbps,upstream_mbps=excluded.upstream_mbps,
	 downstream_mbps=excluded.downstream_mbps,socks_port=excluded.socks_port,
 udp_port_min=excluded.udp_port_min,udp_port_max=excluded.udp_port_max,
	 relay_port=excluded.relay_port,exit_port=excluded.exit_port,exit_bind_ip=excluded.exit_bind_ip,dns_servers=excluded.dns_servers,whitelist=excluded.whitelist,
 build_mode=excluded.build_mode,artifact_ref=excluded.artifact_ref,source_ref=excluded.source_ref,
	 srs_ref=excluded.srs_ref,jump_policy=excluded.jump_policy,updated_at=excluded.updated_at,environment=excluded.environment,
	 topology_mode=excluded.topology_mode,service_profile=excluded.service_profile`, `INSERT INTO line_specs
 (line_id,resource_group,instance_id,bandwidth_mbps,upstream_mbps,downstream_mbps,socks_port,udp_port_min,udp_port_max,
	 relay_port,exit_port,exit_bind_ip,dns_servers,whitelist,build_mode,artifact_ref,source_ref,srs_ref,jump_policy,created_at,updated_at,environment,topology_mode,service_profile)
	 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE resource_group=VALUES(resource_group),
 instance_id=VALUES(instance_id),bandwidth_mbps=VALUES(bandwidth_mbps),upstream_mbps=VALUES(upstream_mbps),
 downstream_mbps=VALUES(downstream_mbps),socks_port=VALUES(socks_port),udp_port_min=VALUES(udp_port_min),
 udp_port_max=VALUES(udp_port_max),relay_port=VALUES(relay_port),exit_port=VALUES(exit_port),
 exit_bind_ip=VALUES(exit_bind_ip),dns_servers=VALUES(dns_servers),whitelist=VALUES(whitelist),build_mode=VALUES(build_mode),
 artifact_ref=VALUES(artifact_ref),source_ref=VALUES(source_ref),srs_ref=VALUES(srs_ref),
	 jump_policy=VALUES(jump_policy),updated_at=VALUES(updated_at),environment=VALUES(environment),
	 topology_mode=VALUES(topology_mode),service_profile=VALUES(service_profile)`)
	_, err = tx.ExecContext(ctx, query,
		spec.LineID, spec.ResourceGroup, spec.InstanceID, spec.BandwidthMbps, spec.UpstreamMbps, spec.DownstreamMbps, spec.SocksPort,
		spec.UDPPortMin, spec.UDPPortMax, spec.RelayPort, spec.ExitPort, spec.ExitBindIP, normalizedJSON(spec.DNSServers, `["1.1.1.1","8.8.8.8"]`),
		normalizedJSON(spec.Whitelist, `[]`), spec.BuildMode, spec.ArtifactRef, spec.SourceRef,
		spec.SRSRef, spec.JumpPolicy, stamp, stamp, spec.Environment, spec.TopologyMode, spec.ServiceProfile)
	if err != nil {
		return LineSpec{}, err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE `+s.linesTable()+` SET environment=?,updated_at=? WHERE id=?`, spec.Environment, stamp, spec.LineID); err != nil {
		return LineSpec{}, err
	}
	allocationQuery := s.controlSQL(`INSERT INTO line_port_allocation
	 (line_id,socks_port_auto,relay_port_auto,exit_port_auto,udp_ports_auto,updated_at)
	 VALUES(?,?,?,?,?,?) ON CONFLICT(line_id) DO UPDATE SET socks_port_auto=excluded.socks_port_auto,
	 relay_port_auto=excluded.relay_port_auto,exit_port_auto=excluded.exit_port_auto,
	 udp_ports_auto=excluded.udp_ports_auto,updated_at=excluded.updated_at`, `INSERT INTO line_port_allocation
	 (line_id,socks_port_auto,relay_port_auto,exit_port_auto,udp_ports_auto,updated_at)
	 VALUES(?,?,?,?,?,?) ON DUPLICATE KEY UPDATE socks_port_auto=VALUES(socks_port_auto),
	 relay_port_auto=VALUES(relay_port_auto),exit_port_auto=VALUES(exit_port_auto),
	 udp_ports_auto=VALUES(udp_ports_auto),updated_at=VALUES(updated_at)`)
	if _, err = tx.ExecContext(ctx, allocationQuery, spec.LineID, spec.SocksPortAuto, spec.RelayPortAuto,
		spec.ExitPortAuto, spec.UDPPortsAuto, stamp); err != nil {
		return LineSpec{}, err
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM line_nodes WHERE line_id=?`, spec.LineID); err != nil {
		return LineSpec{}, err
	}
	for _, node := range spec.Nodes {
		_, err = tx.ExecContext(ctx, `INSERT INTO line_nodes
 (line_id,device_id,role,ordinal,next_hop_device_id,jump_candidates,config) VALUES(?,?,?,?,?,?,?)`,
			spec.LineID, node.DeviceID, node.Role, node.Ordinal, node.NextHopDevice,
			normalizedJSON(node.JumpCandidates, `[]`), normalizedJSON(node.Config, `{}`))
		if err != nil {
			return LineSpec{}, err
		}
	}
	if err = tx.Commit(); err != nil {
		return LineSpec{}, err
	}
	return s.LineSpec(ctx, spec.LineID)
}

func (s *Store) LineSpec(ctx context.Context, lineID string) (LineSpec, error) {
	var item LineSpec
	var dnsServers, whitelist []byte
	err := s.db.QueryRowContext(ctx, `SELECT line_id,resource_group,instance_id,bandwidth_mbps,upstream_mbps,downstream_mbps,
 socks_port,udp_port_min,udp_port_max,relay_port,exit_port,exit_bind_ip,dns_servers,whitelist,build_mode,artifact_ref,
	 source_ref,srs_ref,jump_policy,created_at,updated_at,environment,topology_mode,service_profile FROM line_specs WHERE line_id=?`, lineID).Scan(
		&item.LineID, &item.ResourceGroup, &item.InstanceID, &item.BandwidthMbps, &item.UpstreamMbps, &item.DownstreamMbps, &item.SocksPort,
		&item.UDPPortMin, &item.UDPPortMax, &item.RelayPort, &item.ExitPort, &item.ExitBindIP, &dnsServers, &whitelist,
		&item.BuildMode, &item.ArtifactRef, &item.SourceRef, &item.SRSRef, &item.JumpPolicy,
		&item.CreatedAt, &item.UpdatedAt, &item.Environment, &item.TopologyMode, &item.ServiceProfile)
	if err != nil {
		return LineSpec{}, err
	}
	item.Whitelist = json.RawMessage(whitelist)
	item.DNSServers = json.RawMessage(dnsServers)
	if err = item.NormalizeTopology(); err != nil {
		return LineSpec{}, err
	}
	var socksAuto, relayAuto, exitAuto, udpAuto bool
	allocationErr := s.db.QueryRowContext(ctx, `SELECT socks_port_auto,relay_port_auto,exit_port_auto,udp_ports_auto
	 FROM line_port_allocation WHERE line_id=?`, lineID).Scan(&socksAuto, &relayAuto, &exitAuto, &udpAuto)
	if allocationErr != nil && !errors.Is(allocationErr, sql.ErrNoRows) {
		return LineSpec{}, allocationErr
	}
	item.SocksPortAuto, item.RelayPortAuto = socksAuto, relayAuto
	item.ExitPortAuto, item.UDPPortsAuto = exitAuto, udpAuto
	rows, err := s.db.QueryContext(ctx, `SELECT device_id,role,ordinal,next_hop_device_id,jump_candidates,config
 FROM line_nodes WHERE line_id=? ORDER BY CASE role WHEN 'entry' THEN 1 WHEN 'relay' THEN 2 ELSE 3 END,ordinal`, lineID)
	if err != nil {
		return LineSpec{}, err
	}
	for rows.Next() {
		var node LineNode
		var jumps, config []byte
		if err = rows.Scan(&node.DeviceID, &node.Role, &node.Ordinal, &node.NextHopDevice, &jumps, &config); err != nil {
			return LineSpec{}, err
		}
		node.JumpCandidates, node.Config = json.RawMessage(jumps), json.RawMessage(config)
		item.Nodes = append(item.Nodes, node)
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return LineSpec{}, err
	}
	if err = rows.Close(); err != nil {
		return LineSpec{}, err
	}
	for index := range item.Nodes {
		if device, loadErr := s.Device(ctx, item.Nodes[index].DeviceID); loadErr == nil {
			item.Nodes[index].Device = &device
		}
	}
	return item, nil
}

// LinesSharingDeviceRoles returns the exact line blast radius of a device-role change.
func (s *Store) LinesSharingDeviceRoles(ctx context.Context, lineID string) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT candidate.line_id
 FROM line_nodes target
 JOIN line_nodes candidate ON candidate.device_id=target.device_id AND candidate.role=target.role
 WHERE target.line_id=? ORDER BY candidate.line_id`, lineID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := []string{}
	for rows.Next() {
		var candidate string
		if err = rows.Scan(&candidate); err != nil {
			return nil, err
		}
		result = append(result, candidate)
	}
	return result, rows.Err()
}

// ActiveLineSpecs returns only deployed lines. Agents use this to rebuild
// ephemeral worker state after a controller migration or restart.
func (s *Store) ActiveLineSpecs(ctx context.Context) ([]LineSpec, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id FROM `+s.linesTable()+` WHERE status IN ('active','maintenance') ORDER BY id`)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			_ = rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	result := make([]LineSpec, 0, len(ids))
	for _, id := range ids {
		spec, loadErr := s.LineSpec(ctx, id)
		if errors.Is(loadErr, sql.ErrNoRows) {
			continue
		}
		if loadErr != nil {
			return nil, loadErr
		}
		result = append(result, spec)
	}
	return result, nil
}

func (s *Store) AllLineSpecs(ctx context.Context) ([]LineSpec, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT line_id FROM line_specs ORDER BY line_id`)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			_ = rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	result := make([]LineSpec, 0, len(ids))
	for _, id := range ids {
		spec, loadErr := s.LineSpec(ctx, id)
		if loadErr != nil {
			return nil, loadErr
		}
		result = append(result, spec)
	}
	return result, nil
}

func lineDevice(spec LineSpec, role string) string {
	for _, node := range spec.Nodes {
		if node.Role == role {
			return node.DeviceID
		}
	}
	return ""
}

// Every NB transport instance currently starts two workers. Each worker binds
// base+workerIndex, so relay and exit reservations must cover both lane ports.
const ManagedTransportWorkerLanes = 2

const transportWorkerLanes = ManagedTransportWorkerLanes

const (
	ManagedSocksPortMin = 1082
	ManagedSocksPortMax = 1199
	ManagedRelayPortMin = 4445
	ManagedRelayPortMax = 4599
	ManagedExitPortMin  = 4443
	ManagedExitPortMax  = 4599
	ManagedUDPPortMin   = 22048
	ManagedUDPPortMax   = 65535
)

func nextFreePortSpan(used map[int]bool, first, last, width int) (int, error) {
	for port := first; port+width-1 <= last; port++ {
		available := true
		for lane := 0; lane < width; lane++ {
			if used[port+lane] {
				available = false
				break
			}
		}
		if available {
			return port, nil
		}
	}
	return 0, errors.New("no internal port is available")
}

func (s *Store) usedRolePortSpans(ctx context.Context, lineID, instanceID, deviceID, role, resourceKind, column string, width int) (map[int]bool, error) {
	query := fmt.Sprintf(`SELECT s.%s FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role=?
 WHERE s.line_id<>? AND n.device_id=?`, column)
	rows, err := s.db.QueryContext(ctx, query, role, lineID, deviceID)
	if err != nil {
		return nil, err
	}
	used := map[int]bool{}
	for rows.Next() {
		var port int
		if err = rows.Scan(&port); err != nil {
			return nil, err
		}
		for lane := 0; lane < width; lane++ {
			used[port+lane] = true
		}
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	claims, err := s.db.QueryContext(ctx, `SELECT port_start,port_end FROM runtime_port_claims
 WHERE device_id=? AND role=? AND resource_kind=? AND instance_id<>?`, deviceID, role, resourceKind, instanceID)
	if err != nil {
		return nil, err
	}
	defer claims.Close()
	for claims.Next() {
		var first, last int
		if err = claims.Scan(&first, &last); err != nil {
			return nil, err
		}
		for port := first; port <= last; port++ {
			used[port] = true
		}
	}
	return used, claims.Err()
}

// AllocateLineSpec fills control-plane-owned ports without changing an explicitly supplied value.
func (s *Store) AllocateLineSpec(ctx context.Context, spec LineSpec) (LineSpec, error) {
	spec.NormalizeRates()
	if err := spec.NormalizeTopology(); err != nil {
		return LineSpec{}, err
	}
	spec.SocksPortAuto = spec.SocksPortAuto || spec.SocksPort == 0
	spec.RelayPortAuto = spec.TopologyMode == "trihop" && (spec.RelayPortAuto || spec.RelayPort == 0)
	spec.ExitPortAuto = spec.ExitPortAuto || spec.ExitPort == 0
	spec.UDPPortsAuto = spec.UDPPortsAuto || (spec.UDPPortMin == 0 && spec.UDPPortMax == 0)
	if spec.InstanceID == "" {
		if len(spec.LineID)+2 > 48 {
			return LineSpec{}, errors.New("线路 ID 过长，无法生成部署实例名称")
		}
		spec.InstanceID = spec.LineID + "_1"
	}
	entryDevice := lineDevice(spec, "entry")
	relayDevice := lineDevice(spec, "relay")
	exitDevice := lineDevice(spec, "exit")
	if entryDevice == "" || exitDevice == "" || (spec.TopologyMode == "trihop" && relayDevice == "") {
		return LineSpec{}, errors.New("分配端口前必须指定 Entry、Relay 和 Exit 设备")
	}
	if spec.TopologyMode == "single_hk" && entryDevice != exitDevice {
		return LineSpec{}, errors.New("香港单节点线路的 Entry 和 Exit 必须是同一台香港设备")
	}
	if spec.SocksPort == 0 {
		used, err := s.usedRolePortSpans(ctx, spec.LineID, spec.InstanceID, entryDevice, "entry", "socks", "socks_port", 1)
		if err != nil {
			return LineSpec{}, err
		}
		if spec.SocksPort, err = nextFreePortSpan(used, ManagedSocksPortMin, ManagedSocksPortMax, 1); err != nil {
			return LineSpec{}, errors.New("没有可用的入口 SOCKS 端口")
		}
	}
	if spec.TopologyMode == "trihop" && spec.RelayPort == 0 {
		used, err := s.usedRolePortSpans(ctx, spec.LineID, spec.InstanceID, relayDevice, "relay", "transport", "relay_port", transportWorkerLanes)
		if err != nil {
			return LineSpec{}, err
		}
		if spec.RelayPort, err = nextFreePortSpan(used, ManagedRelayPortMin, ManagedRelayPortMax, transportWorkerLanes); err != nil {
			return LineSpec{}, err
		}
	}
	if spec.ExitPort == 0 {
		used, err := s.usedRolePortSpans(ctx, spec.LineID, spec.InstanceID, exitDevice, "exit", "transport", "exit_port", transportWorkerLanes)
		if err != nil {
			return LineSpec{}, err
		}
		if spec.ExitPort, err = nextFreePortSpan(used, ManagedExitPortMin, ManagedExitPortMax, transportWorkerLanes); err != nil {
			return LineSpec{}, err
		}
	}
	if spec.UDPPortMin == 0 && spec.UDPPortMax == 0 {
		rows, err := s.db.QueryContext(ctx, `SELECT s.udp_port_min,s.udp_port_max FROM line_specs s
	 JOIN line_nodes n ON n.line_id=s.line_id AND n.role='entry'
	 WHERE s.line_id<>? AND n.device_id=?`, spec.LineID, entryDevice)
		if err != nil {
			return LineSpec{}, err
		}
		var ranges [][2]int
		for rows.Next() {
			var item [2]int
			if err = rows.Scan(&item[0], &item[1]); err != nil {
				_ = rows.Close()
				return LineSpec{}, err
			}
			ranges = append(ranges, item)
		}
		if err = rows.Close(); err != nil {
			return LineSpec{}, err
		}
		claimRows, err := s.db.QueryContext(ctx, `SELECT port_start,port_end FROM runtime_port_claims
		 WHERE device_id=? AND role='entry' AND resource_kind='udp' AND instance_id<>?`, entryDevice, spec.InstanceID)
		if err != nil {
			return LineSpec{}, err
		}
		for claimRows.Next() {
			var item [2]int
			if err = claimRows.Scan(&item[0], &item[1]); err != nil {
				_ = claimRows.Close()
				return LineSpec{}, err
			}
			ranges = append(ranges, item)
		}
		if err = claimRows.Close(); err != nil {
			return LineSpec{}, err
		}
		for first := ManagedUDPPortMin; first+1023 <= 65023; first += 1024 {
			last, available := first+1023, true
			for _, item := range ranges {
				if first <= item[1] && item[0] <= last {
					available = false
					break
				}
			}
			if available {
				spec.UDPPortMin, spec.UDPPortMax = first, last
				break
			}
		}
		if spec.UDPPortMin == 0 {
			return LineSpec{}, errors.New("没有可用的 UDP Relay 端口段")
		}
	}
	return spec, nil
}

type rowQuerier interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func lineSpecConflict(ctx context.Context, queryer rowQuerier, spec LineSpec) (string, error) {
	checks := []struct {
		role, resource, column, label string
		port                          int
		width                         int
	}{{"entry", "socks", "socks_port", "入口", spec.SocksPort, 1}, {"relay", "transport", "relay_port", "Relay", spec.RelayPort, transportWorkerLanes}, {"exit", "transport", "exit_port", "Exit", spec.ExitPort, transportWorkerLanes}}
	for _, check := range checks {
		deviceID := lineDevice(spec, check.role)
		if check.port == 0 || deviceID == "" {
			continue
		}
		var conflictingLine string
		query := fmt.Sprintf(`SELECT s.line_id FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role=?
	 WHERE s.line_id<>? AND ?<=s.%s+? AND s.%s<=? AND n.device_id=? LIMIT 1`, check.column, check.column)
		err := queryer.QueryRowContext(ctx, query, check.role, spec.LineID, check.port, check.width-1, check.port+check.width-1, deviceID).Scan(&conflictingLine)
		if err == nil {
			return fmt.Sprintf("%s端口与线路 %s 在设备 %s 上冲突", check.label, conflictingLine, deviceID), nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return "", err
		}
		if instanceID, claimErr := runtimeClaimConflict(ctx, queryer, spec, check.role, check.resource,
			check.port, check.port+check.width-1); claimErr != nil {
			return "", claimErr
		} else if instanceID != "" {
			return fmt.Sprintf("%s端口与运行实例 %s 在设备 %s 上冲突", check.label, instanceID, deviceID), nil
		}
	}
	entryDevice := lineDevice(spec, "entry")
	var conflictingLine string
	err := queryer.QueryRowContext(ctx, `SELECT s.line_id FROM line_specs s
	 JOIN line_nodes n ON n.line_id=s.line_id AND n.role='entry'
 WHERE s.line_id<>? AND n.device_id=? AND ?<=s.udp_port_max AND s.udp_port_min<=? LIMIT 1`,
		spec.LineID, entryDevice, spec.UDPPortMin, spec.UDPPortMax).Scan(&conflictingLine)
	if err == nil {
		return fmt.Sprintf("UDP Relay 端口段与线路 %s 在设备 %s 上冲突", conflictingLine, entryDevice), nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", err
	}
	if instanceID, claimErr := runtimeClaimConflict(ctx, queryer, spec, "entry", "udp", spec.UDPPortMin, spec.UDPPortMax); claimErr != nil {
		return "", claimErr
	} else if instanceID != "" {
		return fmt.Sprintf("UDP Relay 端口段与运行实例 %s 在设备 %s 上冲突", instanceID, lineDevice(spec, "entry")), nil
	}
	return "", nil
}

func (s *Store) LineSpecConflict(ctx context.Context, spec LineSpec) (string, error) {
	return lineSpecConflict(ctx, s.db, spec)
}

func (s *Store) RecordOperationEvent(ctx context.Context, event OperationEvent) error {
	unlock := s.lockWrite()
	defer unlock()
	if len(event.Parameters) == 0 {
		event.Parameters = json.RawMessage(`{}`)
	}
	if len(event.Message) > 32<<10 {
		event.Message = strings.ToValidUTF8(event.Message[:32<<10], "")
	}
	if len(event.Parameters) > 64<<10 {
		event.Parameters = json.RawMessage(`{"truncated":true}`)
	}
	query := s.controlSQL(`INSERT OR REPLACE INTO operation_events
	 (operation_id,sequence,stage,status,message,parameters,created_at) VALUES(?,?,?,?,?,?,?)`, `INSERT INTO operation_events
	 (operation_id,sequence,stage,status,message,parameters,created_at) VALUES(?,?,?,?,?,?,?)
	 ON DUPLICATE KEY UPDATE stage=VALUES(stage),status=VALUES(status),message=VALUES(message),
	 parameters=VALUES(parameters),created_at=VALUES(created_at)`)
	_, err := s.db.ExecContext(ctx, query,
		event.OperationID, event.Sequence, event.Stage, event.Status, event.Message,
		normalizedJSON(event.Parameters, `{}`), now())
	if err == nil {
		_, err = s.db.ExecContext(ctx, `UPDATE operations SET status=CASE WHEN ?='running' THEN 'running' ELSE status END,
 updated_at=? WHERE id=?`, event.Status, now(), event.OperationID)
	}
	return err
}

func (s *Store) OperationEvents(ctx context.Context, operationID string) ([]OperationEvent, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,operation_id,sequence,stage,status,message,parameters,created_at
 FROM operation_events WHERE operation_id=? ORDER BY sequence`, operationID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []OperationEvent
	for rows.Next() {
		var item OperationEvent
		var params []byte
		if err = rows.Scan(&item.ID, &item.OperationID, &item.Sequence, &item.Stage, &item.Status, &item.Message, &params, &item.CreatedAt); err != nil {
			return nil, err
		}
		item.Parameters = json.RawMessage(params)
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) LatestSnapshots(ctx context.Context, lineID string) ([]Snapshot, error) {
	rows, err := s.telemetryDB.QueryContext(ctx, `SELECT s.line_id,s.node_id,s.role,s.worker_id,s.observed_at,s.health,s.deployment,s.profile,
	 s.sessions,s.throughput_mbps,s.upstream_mbps,s.downstream_mbps,s.queue_age_p95_us,s.effective_loss_pct,
	 s.fec_observe,s.fec_active,COALESCE(payloads.payload,s.payload),s.received_at
	 FROM latest_snapshots latest JOIN snapshots s ON s.id=latest.snapshot_id
	 LEFT JOIN latest_snapshot_payloads payloads ON payloads.line_id=s.line_id AND payloads.node_id=s.node_id
	  AND payloads.observed_at=s.observed_at WHERE latest.line_id=?
	 ORDER BY CASE s.role WHEN 'entry' THEN 1 WHEN 'middle' THEN 2 WHEN 'relay' THEN 2 ELSE 3 END,s.node_id`, lineID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Snapshot
	for rows.Next() {
		var item Snapshot
		if err = rows.Scan(&item.LineID, &item.NodeID, &item.Role, &item.WorkerID, &item.ObservedAt, &item.Health, &item.Deployment, &item.Profile, &item.Sessions, &item.ThroughputMbps, &item.UpstreamMbps, &item.DownstreamMbps, &item.QueueAgeP95US, &item.EffectiveLoss, &item.FECObserve, &item.FECActive, &item.Payload, &item.ReceivedAt); err != nil {
			return nil, err
		}
		result = append(result, item)
	}
	return result, rows.Err()
}

func (s *Store) Operation(ctx context.Context, id string) (Operation, error) {
	var item Operation
	err := scanOperation(s.db.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,
 request,result,created_at,updated_at FROM operations WHERE id=?`, id), &item)
	return item, err
}

func (s *Store) DebugCounts(ctx context.Context) (map[string]int64, error) {
	result := map[string]int64{}
	for _, table := range []string{"devices", "line_specs", "line_nodes", "operation_events"} {
		var count int64
		if err := s.db.QueryRowContext(ctx, fmt.Sprintf("SELECT COUNT(*) FROM %s", table)).Scan(&count); err != nil && !errors.Is(err, sql.ErrNoRows) {
			return nil, err
		}
		result[table] = count
	}
	return result, nil
}
