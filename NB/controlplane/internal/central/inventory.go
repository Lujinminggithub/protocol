package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
)

type Device struct {
	ID         string          `json:"id"`
	Name       string          `json:"name"`
	Status     string          `json:"status"`
	Host       string          `json:"host"`
	SSHPort    int             `json:"ssh_port"`
	SSHUser    string          `json:"ssh_user"`
	PrivateIP  string          `json:"private_ip"`
	Region     string          `json:"region"`
	Provider   string          `json:"provider"`
	OS         string          `json:"os"`
	Arch       string          `json:"arch"`
	SecretRef  string          `json:"secret_ref"`
	Labels     json.RawMessage `json:"labels"`
	LastHealth string          `json:"last_health"`
	LastSeenAt string          `json:"last_seen_at"`
	CreatedAt  string          `json:"created_at"`
	UpdatedAt  string          `json:"updated_at"`
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
	LineID        string          `json:"line_id"`
	ResourceGroup string          `json:"resource_group"`
	InstanceID    string          `json:"instance_id"`
	BandwidthMbps int             `json:"bandwidth_mbps"`
	SocksPort     int             `json:"socks_port"`
	UDPPortMin    int             `json:"udp_port_min"`
	UDPPortMax    int             `json:"udp_port_max"`
	RelayPort     int             `json:"relay_port"`
	ExitPort      int             `json:"exit_port"`
	Whitelist     json.RawMessage `json:"whitelist"`
	BuildMode     string          `json:"build_mode"`
	ArtifactRef   string          `json:"artifact_ref"`
	SourceRef     string          `json:"source_ref"`
	SRSRef        string          `json:"srs_ref"`
	JumpPolicy    string          `json:"jump_policy"`
	Nodes         []LineNode      `json:"nodes"`
	CreatedAt     string          `json:"created_at"`
	UpdatedAt     string          `json:"updated_at"`
}

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
		&item.SSHUser, &item.PrivateIP, &item.Region, &item.Provider, &item.OS, &item.Arch,
		&item.SecretRef, &labels, &item.LastHealth, &item.LastSeenAt, &item.CreatedAt, &item.UpdatedAt)
	item.Labels = json.RawMessage(labels)
	return err
}

const deviceColumns = `id,name,status,host,ssh_port,ssh_user,private_ip,region,provider,os,arch,
 secret_ref,labels,last_health,last_seen_at,created_at,updated_at`

func (s *Store) UpsertDevice(ctx context.Context, item Device) (Device, error) {
	stamp := now()
	_, err := s.db.ExecContext(ctx, `INSERT INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
 ON CONFLICT(id) DO UPDATE SET name=excluded.name,status=excluded.status,host=excluded.host,
 ssh_port=excluded.ssh_port,ssh_user=excluded.ssh_user,private_ip=excluded.private_ip,
 region=excluded.region,provider=excluded.provider,os=excluded.os,arch=excluded.arch,
 secret_ref=excluded.secret_ref,labels=excluded.labels,updated_at=excluded.updated_at`,
		item.ID, item.Name, item.Status, item.Host, item.SSHPort, item.SSHUser, item.PrivateIP,
		item.Region, item.Provider, item.OS, item.Arch, item.SecretRef,
		normalizedJSON(item.Labels, `{}`), item.LastHealth, item.LastSeenAt, stamp, stamp)
	if err != nil {
		return Device{}, err
	}
	return s.Device(ctx, item.ID)
}

// EnsureDevice adds worker-discovered inventory without overwriting operator-managed fields.
func (s *Store) EnsureDevice(ctx context.Context, item Device) (Device, bool, error) {
	stamp := now()
	result, err := s.db.ExecContext(ctx, `INSERT OR IGNORE INTO devices (`+deviceColumns+`) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		item.ID, item.Name, item.Status, item.Host, item.SSHPort, item.SSHUser, item.PrivateIP,
		item.Region, item.Provider, item.OS, item.Arch, item.SecretRef,
		normalizedJSON(item.Labels, `{}`), item.LastHealth, item.LastSeenAt, stamp, stamp)
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
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return LineSpec{}, err
	}
	defer tx.Rollback()
	stamp := now()
	_, err = tx.ExecContext(ctx, `INSERT INTO line_specs
 (line_id,resource_group,instance_id,bandwidth_mbps,socks_port,udp_port_min,udp_port_max,
 relay_port,exit_port,whitelist,build_mode,artifact_ref,source_ref,srs_ref,jump_policy,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(line_id) DO UPDATE SET
 resource_group=excluded.resource_group,instance_id=excluded.instance_id,
 bandwidth_mbps=excluded.bandwidth_mbps,socks_port=excluded.socks_port,
 udp_port_min=excluded.udp_port_min,udp_port_max=excluded.udp_port_max,
 relay_port=excluded.relay_port,exit_port=excluded.exit_port,whitelist=excluded.whitelist,
 build_mode=excluded.build_mode,artifact_ref=excluded.artifact_ref,source_ref=excluded.source_ref,
 srs_ref=excluded.srs_ref,jump_policy=excluded.jump_policy,updated_at=excluded.updated_at`,
		spec.LineID, spec.ResourceGroup, spec.InstanceID, spec.BandwidthMbps, spec.SocksPort,
		spec.UDPPortMin, spec.UDPPortMax, spec.RelayPort, spec.ExitPort,
		normalizedJSON(spec.Whitelist, `[]`), spec.BuildMode, spec.ArtifactRef, spec.SourceRef,
		spec.SRSRef, spec.JumpPolicy, stamp, stamp)
	if err != nil {
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
	var whitelist []byte
	err := s.db.QueryRowContext(ctx, `SELECT line_id,resource_group,instance_id,bandwidth_mbps,
 socks_port,udp_port_min,udp_port_max,relay_port,exit_port,whitelist,build_mode,artifact_ref,
 source_ref,srs_ref,jump_policy,created_at,updated_at FROM line_specs WHERE line_id=?`, lineID).Scan(
		&item.LineID, &item.ResourceGroup, &item.InstanceID, &item.BandwidthMbps, &item.SocksPort,
		&item.UDPPortMin, &item.UDPPortMax, &item.RelayPort, &item.ExitPort, &whitelist,
		&item.BuildMode, &item.ArtifactRef, &item.SourceRef, &item.SRSRef, &item.JumpPolicy,
		&item.CreatedAt, &item.UpdatedAt)
	if err != nil {
		return LineSpec{}, err
	}
	item.Whitelist = json.RawMessage(whitelist)
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
const transportWorkerLanes = 2

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

func (s *Store) usedRolePortSpans(ctx context.Context, lineID, deviceID, role, column string, width int) (map[int]bool, error) {
	query := fmt.Sprintf(`SELECT s.%s FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role=?
 WHERE s.line_id<>? AND n.device_id=?`, column)
	rows, err := s.db.QueryContext(ctx, query, role, lineID, deviceID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
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
	return used, rows.Err()
}

// AllocateLineSpec fills control-plane-owned ports without changing an explicitly supplied value.
func (s *Store) AllocateLineSpec(ctx context.Context, spec LineSpec) (LineSpec, error) {
	if spec.InstanceID == "" {
		if len(spec.LineID)+2 > 48 {
			return LineSpec{}, errors.New("line ID is too long for an automatic deployment instance")
		}
		spec.InstanceID = spec.LineID + "_1"
	}
	relayDevice := lineDevice(spec, "relay")
	exitDevice := lineDevice(spec, "exit")
	if relayDevice == "" || exitDevice == "" {
		return LineSpec{}, errors.New("internal port allocation requires relay and exit devices")
	}
	if spec.RelayPort == 0 {
		used, err := s.usedRolePortSpans(ctx, spec.LineID, relayDevice, "relay", "relay_port", transportWorkerLanes)
		if err != nil {
			return LineSpec{}, err
		}
		if spec.RelayPort, err = nextFreePortSpan(used, 4445, 4599, transportWorkerLanes); err != nil {
			return LineSpec{}, err
		}
	}
	if spec.ExitPort == 0 {
		used, err := s.usedRolePortSpans(ctx, spec.LineID, exitDevice, "exit", "exit_port", transportWorkerLanes)
		if err != nil {
			return LineSpec{}, err
		}
		if spec.ExitPort, err = nextFreePortSpan(used, 4443, 4599, transportWorkerLanes); err != nil {
			return LineSpec{}, err
		}
	}
	if spec.UDPPortMin == 0 && spec.UDPPortMax == 0 {
		rows, err := s.db.QueryContext(ctx, `SELECT s.udp_port_min,s.udp_port_max FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role='relay'
 WHERE s.line_id<>? AND n.device_id=?`, spec.LineID, relayDevice)
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
		for first := 22048; first+1023 <= 65023; first += 1024 {
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
			return LineSpec{}, errors.New("no UDP relay range is available")
		}
	}
	return spec, nil
}

func (s *Store) LineSpecConflict(ctx context.Context, spec LineSpec) (string, error) {
	checks := []struct {
		role, column, label string
		port                int
		width               int
	}{{"entry", "socks_port", "entry", spec.SocksPort, 1}, {"relay", "relay_port", "relay", spec.RelayPort, transportWorkerLanes}, {"exit", "exit_port", "exit", spec.ExitPort, transportWorkerLanes}}
	for _, check := range checks {
		deviceID := lineDevice(spec, check.role)
		var conflictingLine string
		query := fmt.Sprintf(`SELECT s.line_id FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role=?
	 WHERE s.line_id<>? AND ?<=s.%s+? AND s.%s<=? AND n.device_id=? LIMIT 1`, check.column, check.column)
		err := s.db.QueryRowContext(ctx, query, check.role, spec.LineID, check.port, check.width-1, check.port+check.width-1, deviceID).Scan(&conflictingLine)
		if err == nil {
			return fmt.Sprintf("%s port conflicts with %s on device %s", check.label, conflictingLine, deviceID), nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return "", err
		}
	}
	relayDevice := lineDevice(spec, "relay")
	var conflictingLine string
	err := s.db.QueryRowContext(ctx, `SELECT s.line_id FROM line_specs s
 JOIN line_nodes n ON n.line_id=s.line_id AND n.role='relay'
 WHERE s.line_id<>? AND n.device_id=? AND ?<=s.udp_port_max AND s.udp_port_min<=? LIMIT 1`,
		spec.LineID, relayDevice, spec.UDPPortMin, spec.UDPPortMax).Scan(&conflictingLine)
	if err == nil {
		return fmt.Sprintf("UDP relay range conflicts with %s on device %s", conflictingLine, relayDevice), nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", err
	}
	return "", nil
}

func (s *Store) RecordOperationEvent(ctx context.Context, event OperationEvent) error {
	if len(event.Parameters) == 0 {
		event.Parameters = json.RawMessage(`{}`)
	}
	_, err := s.db.ExecContext(ctx, `INSERT OR REPLACE INTO operation_events
 (operation_id,sequence,stage,status,message,parameters,created_at) VALUES(?,?,?,?,?,?,?)`,
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
	rows, err := s.db.QueryContext(ctx, `WITH ranked AS (
	 SELECT line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
	 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at,
	 ROW_NUMBER() OVER (PARTITION BY node_id ORDER BY observed_at DESC,id DESC) AS rn
	 FROM snapshots WHERE line_id=?
	) SELECT line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
	 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at FROM ranked WHERE rn=1
	 ORDER BY CASE role WHEN 'entry' THEN 1 WHEN 'middle' THEN 2 WHEN 'relay' THEN 2 ELSE 3 END,node_id`, lineID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []Snapshot
	for rows.Next() {
		var item Snapshot
		if err = rows.Scan(&item.LineID, &item.NodeID, &item.Role, &item.WorkerID, &item.ObservedAt, &item.Health, &item.Deployment, &item.Profile, &item.Sessions, &item.ThroughputMbps, &item.QueueAgeP95US, &item.EffectiveLoss, &item.FECObserve, &item.FECActive, &item.Payload, &item.ReceivedAt); err != nil {
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
