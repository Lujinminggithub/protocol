package central

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strings"
)

const (
	LinkBillingIndependent = "independent-directions"
	LinkBillingAggregate   = "aggregate-bidirectional"
	LinkBillingUnknown     = "unknown"
)

type NetworkLink struct {
	ID                   string   `json:"id"`
	FromDeviceID         string   `json:"from_device_id"`
	FromRole             string   `json:"from_role"`
	ToDeviceID           string   `json:"to_device_id"`
	ToRole               string   `json:"to_role"`
	ForwardCapacityMbps  int      `json:"forward_capacity_mbps"`
	ReverseCapacityMbps  int      `json:"reverse_capacity_mbps"`
	ForwardReservedMbps  int      `json:"forward_reserved_mbps"`
	ReverseReservedMbps  int      `json:"reverse_reserved_mbps"`
	ForwardAvailableMbps int      `json:"forward_available_mbps"`
	ReverseAvailableMbps int      `json:"reverse_available_mbps"`
	BillingMode          string   `json:"billing_mode"`
	Environment          string   `json:"environment"`
	Status               string   `json:"status"`
	CreatedAt            string   `json:"created_at"`
	UpdatedAt            string   `json:"updated_at"`
	AffectedLines        []string `json:"affected_lines"`
}

type LineCapacityReservation struct {
	LineID      string `json:"line_id"`
	LinkID      string `json:"link_id"`
	ForwardMbps int    `json:"forward_mbps"`
	ReverseMbps int    `json:"reverse_mbps"`
	State       string `json:"state"`
	OperationID string `json:"operation_id"`
	CreatedAt   string `json:"created_at"`
	UpdatedAt   string `json:"updated_at"`
}

type CapacityError struct {
	LinkID    string
	Direction string
	Required  int
	Available int
	Reason    string
}

func (err *CapacityError) Error() string {
	if err.Reason != "" {
		return fmt.Sprintf("link %s %s", err.LinkID, err.Reason)
	}
	return fmt.Sprintf("link %s %s required=%d available=%d", err.LinkID, err.Direction,
		err.Required, err.Available)
}

func validLinkRole(role string) bool {
	return role == "entry" || role == "relay" || role == "exit"
}

func validateNetworkLink(link NetworkLink) error {
	if strings.TrimSpace(link.ID) == "" || strings.TrimSpace(link.FromDeviceID) == "" ||
		strings.TrimSpace(link.ToDeviceID) == "" || link.FromDeviceID == link.ToDeviceID ||
		!validLinkRole(link.FromRole) || !validLinkRole(link.ToRole) {
		return errors.New("invalid network link identity")
	}
	if link.ForwardCapacityMbps < 1 || link.ForwardCapacityMbps > 100000 ||
		link.ReverseCapacityMbps < 1 || link.ReverseCapacityMbps > 100000 {
		return errors.New("invalid network link capacity")
	}
	if link.BillingMode != LinkBillingIndependent && link.BillingMode != LinkBillingAggregate &&
		link.BillingMode != LinkBillingUnknown {
		return errors.New("invalid network link billing mode")
	}
	if link.Environment != "production" && link.Environment != "test" {
		return errors.New("invalid network link environment")
	}
	if link.Status != "ready" && link.Status != "maintenance" && link.Status != "disabled" {
		return errors.New("invalid network link status")
	}
	return nil
}

func scanNetworkLink(row scanner, link *NetworkLink) error {
	return row.Scan(&link.ID, &link.FromDeviceID, &link.FromRole, &link.ToDeviceID, &link.ToRole,
		&link.ForwardCapacityMbps, &link.ReverseCapacityMbps, &link.BillingMode,
		&link.Environment, &link.Status, &link.CreatedAt, &link.UpdatedAt)
}

func (s *Store) UpsertNetworkLink(ctx context.Context, link NetworkLink) (NetworkLink, error) {
	if err := validateNetworkLink(link); err != nil {
		return NetworkLink{}, err
	}
	if _, err := s.Device(ctx, link.FromDeviceID); err != nil {
		return NetworkLink{}, fmt.Errorf("from device is unavailable: %w", err)
	}
	if _, err := s.Device(ctx, link.ToDeviceID); err != nil {
		return NetworkLink{}, fmt.Errorf("to device is unavailable: %w", err)
	}
	unlock := s.lockWrite()
	defer unlock()
	stamp := now()
	if link.CreatedAt == "" {
		link.CreatedAt = stamp
	}
	link.UpdatedAt = stamp
	query := s.controlSQL(`INSERT INTO network_links
 (id,from_device_id,from_role,to_device_id,to_role,forward_capacity_mbps,reverse_capacity_mbps,
  billing_mode,environment,status,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
 ON CONFLICT(id) DO UPDATE SET from_device_id=excluded.from_device_id,from_role=excluded.from_role,
 to_device_id=excluded.to_device_id,to_role=excluded.to_role,
 forward_capacity_mbps=excluded.forward_capacity_mbps,reverse_capacity_mbps=excluded.reverse_capacity_mbps,
 billing_mode=excluded.billing_mode,environment=excluded.environment,status=excluded.status,updated_at=excluded.updated_at`,
		`INSERT INTO network_links
 (id,from_device_id,from_role,to_device_id,to_role,forward_capacity_mbps,reverse_capacity_mbps,
  billing_mode,environment,status,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
 ON DUPLICATE KEY UPDATE from_device_id=VALUES(from_device_id),from_role=VALUES(from_role),
 to_device_id=VALUES(to_device_id),to_role=VALUES(to_role),
 forward_capacity_mbps=VALUES(forward_capacity_mbps),reverse_capacity_mbps=VALUES(reverse_capacity_mbps),
 billing_mode=VALUES(billing_mode),environment=VALUES(environment),status=VALUES(status),updated_at=VALUES(updated_at)`)
	_, err := s.db.ExecContext(ctx, query, link.ID, link.FromDeviceID, link.FromRole, link.ToDeviceID,
		link.ToRole, link.ForwardCapacityMbps, link.ReverseCapacityMbps, link.BillingMode,
		link.Environment, link.Status, link.CreatedAt, link.UpdatedAt)
	return link, err
}

func (s *Store) NetworkLinks(ctx context.Context) ([]NetworkLink, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,from_device_id,from_role,to_device_id,to_role,
 forward_capacity_mbps,reverse_capacity_mbps,billing_mode,environment,status,created_at,updated_at
 FROM network_links ORDER BY id`)
	if err != nil {
		return nil, err
	}
	result := []NetworkLink{}
	for rows.Next() {
		var link NetworkLink
		if err = scanNetworkLink(rows, &link); err != nil {
			_ = rows.Close()
			return nil, err
		}
		result = append(result, link)
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	for index := range result {
		link := &result[index]
		if err = s.db.QueryRowContext(ctx, `SELECT COALESCE(SUM(forward_mbps),0),COALESCE(SUM(reverse_mbps),0)
 FROM line_capacity_reservations WHERE link_id=? AND state IN ('reserved','active')`, link.ID).
			Scan(&link.ForwardReservedMbps, &link.ReverseReservedMbps); err != nil {
			return nil, err
		}
		link.ForwardAvailableMbps = link.ForwardCapacityMbps - link.ForwardReservedMbps
		link.ReverseAvailableMbps = link.ReverseCapacityMbps - link.ReverseReservedMbps
		affectedRows, affectedErr := s.db.QueryContext(ctx, `SELECT line_id FROM line_capacity_reservations
 WHERE link_id=? AND state IN ('reserved','active') ORDER BY line_id`, link.ID)
		if affectedErr != nil {
			return nil, affectedErr
		}
		for affectedRows.Next() {
			var lineID string
			if affectedErr = affectedRows.Scan(&lineID); affectedErr != nil {
				_ = affectedRows.Close()
				return nil, affectedErr
			}
			link.AffectedLines = append(link.AffectedLines, lineID)
		}
		if affectedErr = affectedRows.Err(); affectedErr != nil {
			_ = affectedRows.Close()
			return nil, affectedErr
		}
		if affectedErr = affectedRows.Close(); affectedErr != nil {
			return nil, affectedErr
		}
	}
	return result, nil
}

type linkDemand struct {
	fromDevice, fromRole, toDevice, toRole string
	forward, reverse                       int
}

func lineLinkDemands(ctx context.Context, tx *sql.Tx, lineID string) ([]linkDemand, error) {
	var up, down int
	var topologyMode string
	if err := tx.QueryRowContext(ctx, `SELECT upstream_mbps,downstream_mbps,topology_mode FROM line_specs WHERE line_id=?`,
		lineID).Scan(&up, &down, &topologyMode); err != nil {
		return nil, err
	}
	rows, err := tx.QueryContext(ctx, `SELECT role,device_id FROM line_nodes WHERE line_id=? ORDER BY
 CASE role WHEN 'entry' THEN 0 WHEN 'relay' THEN 1 WHEN 'exit' THEN 2 ELSE 3 END,ordinal`, lineID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := map[string]string{}
	for rows.Next() {
		var role, device string
		if err = rows.Scan(&role, &device); err != nil {
			return nil, err
		}
		if devices[role] == "" {
			devices[role] = device
		}
	}
	if err = rows.Err(); err != nil {
		return nil, err
	}
	if topologyMode == "single_hk" {
		if devices["entry"] == "" || devices["entry"] != devices["exit"] || devices["relay"] != "" {
			return nil, errors.New("single-HK capacity requires colocated entry and exit without relay")
		}
		return []linkDemand{}, nil
	}
	if devices["entry"] == "" || devices["relay"] == "" || devices["exit"] == "" {
		return nil, errors.New("line capacity requires entry, relay and exit")
	}
	return []linkDemand{
		{devices["entry"], "entry", devices["relay"], "relay", up, down},
		{devices["relay"], "relay", devices["exit"], "exit", up, down},
	}, nil
}

func reservedTopologyLinkIDs(ctx context.Context, tx *sql.Tx, lineID string) ([]string, error) {
	demands, err := lineLinkDemands(ctx, tx, lineID)
	if err != nil {
		return nil, err
	}
	if len(demands) != 0 && len(demands) != 2 {
		return nil, errors.New("production qualification requires two physical topology links")
	}
	var held int
	if err = tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM line_capacity_reservations
 WHERE line_id=? AND state IN ('reserved','active')`, lineID).Scan(&held); err != nil {
		return nil, err
	}
	if held != len(demands) {
		return nil, fmt.Errorf("production qualification has %d held reservations for %d topology links", held, len(demands))
	}
	linkIDs := make([]string, 0, len(demands))
	for _, demand := range demands {
		var linkID string
		err = tx.QueryRowContext(ctx, `SELECT r.link_id FROM line_capacity_reservations r
 JOIN network_links l ON l.id=r.link_id
 WHERE r.line_id=? AND r.state='reserved' AND r.forward_mbps=? AND r.reverse_mbps=?
 AND l.from_device_id=? AND l.from_role=? AND l.to_device_id=? AND l.to_role=?`,
			lineID, demand.forward, demand.reverse, demand.fromDevice, demand.fromRole,
			demand.toDevice, demand.toRole).Scan(&linkID)
		if errors.Is(err, sql.ErrNoRows) {
			return nil, fmt.Errorf("current topology link %s:%s->%s:%s is not reserved",
				demand.fromDevice, demand.fromRole, demand.toDevice, demand.toRole)
		}
		if err != nil {
			return nil, err
		}
		linkIDs = append(linkIDs, linkID)
	}
	return linkIDs, nil
}

func (s *Store) ReserveLineCapacity(ctx context.Context, lineID, operationID string) ([]LineCapacityReservation, error) {
	if strings.TrimSpace(lineID) == "" || strings.TrimSpace(operationID) == "" {
		return nil, errors.New("line capacity reservation identity is required")
	}
	unlock := s.lockWrite()
	defer unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	demands, err := lineLinkDemands(ctx, tx, lineID)
	if err != nil {
		return nil, err
	}
	result := make([]LineCapacityReservation, 0, len(demands))
	for _, demand := range demands {
		query := `SELECT id,from_device_id,from_role,to_device_id,to_role,forward_capacity_mbps,
 reverse_capacity_mbps,billing_mode,environment,status,created_at,updated_at FROM network_links
 WHERE from_device_id=? AND from_role=? AND to_device_id=? AND to_role=?`
		if s.dialect == "mysql" {
			query += " FOR UPDATE"
		}
		var link NetworkLink
		if err = scanNetworkLink(tx.QueryRowContext(ctx, query, demand.fromDevice, demand.fromRole,
			demand.toDevice, demand.toRole), &link); errors.Is(err, sql.ErrNoRows) {
			identity := fmt.Sprintf("%s:%s->%s:%s", demand.fromDevice, demand.fromRole,
				demand.toDevice, demand.toRole)
			return nil, &CapacityError{LinkID: identity, Reason: "is not registered"}
		} else if err != nil {
			return nil, err
		}
		if link.Status != "ready" || link.Environment != "production" || link.BillingMode != LinkBillingIndependent {
			return nil, &CapacityError{LinkID: link.ID, Reason: "is not ready for independent production directions"}
		}
		var existing LineCapacityReservation
		existingErr := tx.QueryRowContext(ctx, `SELECT line_id,link_id,forward_mbps,reverse_mbps,state,
 operation_id,created_at,updated_at FROM line_capacity_reservations WHERE line_id=? AND link_id=?`,
			lineID, link.ID).Scan(&existing.LineID, &existing.LinkID, &existing.ForwardMbps,
			&existing.ReverseMbps, &existing.State, &existing.OperationID, &existing.CreatedAt, &existing.UpdatedAt)
		if existingErr == nil && existing.State != "released" {
			if existing.ForwardMbps != demand.forward || existing.ReverseMbps != demand.reverse {
				return nil, &CapacityError{LinkID: link.ID, Reason: "has an incompatible existing reservation"}
			}
			result = append(result, existing)
			continue
		}
		if existingErr != nil && !errors.Is(existingErr, sql.ErrNoRows) {
			return nil, existingErr
		}
		var usedForward, usedReverse int
		if err = tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(forward_mbps),0),COALESCE(SUM(reverse_mbps),0)
 FROM line_capacity_reservations WHERE link_id=? AND line_id<>? AND state IN ('reserved','active')`,
			link.ID, lineID).Scan(&usedForward, &usedReverse); err != nil {
			return nil, err
		}
		availableForward := link.ForwardCapacityMbps - usedForward
		availableReverse := link.ReverseCapacityMbps - usedReverse
		if demand.forward > availableForward {
			return nil, &CapacityError{LinkID: link.ID, Direction: "forward", Required: demand.forward, Available: availableForward}
		}
		if demand.reverse > availableReverse {
			return nil, &CapacityError{LinkID: link.ID, Direction: "reverse", Required: demand.reverse, Available: availableReverse}
		}
		stamp := now()
		reservation := LineCapacityReservation{LineID: lineID, LinkID: link.ID, ForwardMbps: demand.forward,
			ReverseMbps: demand.reverse, State: "reserved", OperationID: operationID,
			CreatedAt: stamp, UpdatedAt: stamp}
		upsert := s.controlSQL(`INSERT INTO line_capacity_reservations
 (line_id,link_id,forward_mbps,reverse_mbps,state,operation_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(line_id,link_id) DO UPDATE SET
 forward_mbps=excluded.forward_mbps,reverse_mbps=excluded.reverse_mbps,state=excluded.state,
 operation_id=excluded.operation_id,updated_at=excluded.updated_at`, `INSERT INTO line_capacity_reservations
 (line_id,link_id,forward_mbps,reverse_mbps,state,operation_id,created_at,updated_at)
 VALUES(?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE forward_mbps=VALUES(forward_mbps),
 reverse_mbps=VALUES(reverse_mbps),state=VALUES(state),operation_id=VALUES(operation_id),updated_at=VALUES(updated_at)`)
		if _, err = tx.ExecContext(ctx, upsert, reservation.LineID, reservation.LinkID,
			reservation.ForwardMbps, reservation.ReverseMbps, reservation.State, reservation.OperationID,
			reservation.CreatedAt, reservation.UpdatedAt); err != nil {
			return nil, err
		}
		result = append(result, reservation)
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	sort.Slice(result, func(i, j int) bool { return result[i].LinkID < result[j].LinkID })
	return result, nil
}

func (s *Store) setLineCapacityState(ctx context.Context, lineID, operationID, state string) error {
	if state != "active" && state != "released" {
		return errors.New("invalid line capacity state")
	}
	unlock := s.lockWrite()
	defer unlock()
	_, err := s.db.ExecContext(ctx, `UPDATE line_capacity_reservations SET state=?,operation_id=?,updated_at=?
 WHERE line_id=? AND state<>?`, state, operationID, now(), lineID, state)
	return err
}

func (s *Store) ActivateLineCapacity(ctx context.Context, lineID, operationID string) error {
	return s.setLineCapacityState(ctx, lineID, operationID, "active")
}

func (s *Store) ReleaseLineCapacity(ctx context.Context, lineID, operationID string) error {
	return s.setLineCapacityState(ctx, lineID, operationID, "released")
}
