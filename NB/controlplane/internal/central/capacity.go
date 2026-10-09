package central

import (
	"context"
	"errors"
	"fmt"
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

func (s *Store) ReserveLineCapacity(ctx context.Context, lineID, operationID string) ([]LineCapacityReservation, error) {
	if strings.TrimSpace(lineID) == "" || strings.TrimSpace(operationID) == "" {
		return nil, errors.New("line capacity reservation identity is required")
	}
	return []LineCapacityReservation{}, nil
}

func (s *Store) ActivateLineCapacity(ctx context.Context, lineID, operationID string) error {
	return nil
}

func (s *Store) ReleaseLineCapacity(ctx context.Context, lineID, operationID string) error {
	return nil
}
