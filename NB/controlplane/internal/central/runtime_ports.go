package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

const RuntimePortClaimTTL = 5 * time.Minute

func validRuntimeClaim(claim RuntimePortClaim, workerID, deviceID, role string) error {
	if strings.TrimSpace(workerID) == "" || claim.WorkerID != workerID || claim.DeviceID != deviceID || claim.Role != role ||
		(claim.ResourceKind != "socks" && claim.ResourceKind != "transport" && claim.ResourceKind != "udp") ||
		strings.TrimSpace(claim.InstanceID) == "" || claim.PortStart < 1 || claim.PortEnd < claim.PortStart || claim.PortEnd > 65535 ||
		claim.Source != "runtime" {
		return errors.New("运行时端口声明无效")
	}
	observed, observedErr := time.Parse(time.RFC3339Nano, claim.ObservedAt)
	expires, expiresErr := time.Parse(time.RFC3339Nano, claim.ExpiresAt)
	if observedErr != nil || expiresErr != nil || !expires.After(observed) {
		return errors.New("运行时端口声明时间无效")
	}
	return nil
}

// ReplaceRuntimePortClaims is called only after a complete device-role scan.
// A failed scan never calls this method, so its previous conservative claims remain.
func (s *Store) ReplaceRuntimePortClaims(ctx context.Context, workerID, deviceID, role string, claims []RuntimePortClaim) error {
	if strings.TrimSpace(workerID) == "" || strings.TrimSpace(deviceID) == "" || (role != "entry" && role != "relay" && role != "exit") {
		return errors.New("运行时端口扫描标识无效")
	}
	for _, claim := range claims {
		if err := validRuntimeClaim(claim, workerID, deviceID, role); err != nil {
			return err
		}
	}
	unlock := s.lockWrite()
	defer unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, `DELETE FROM runtime_port_claims WHERE worker_id=? AND device_id=? AND role=? AND source='runtime'`, workerID, deviceID, role); err != nil {
		return err
	}
	query := s.controlSQL(`INSERT INTO runtime_port_claims
 (worker_id,device_id,role,resource_kind,instance_id,port_start,port_end,observed_at,expires_at,source)
 VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(worker_id,device_id,role,resource_kind,instance_id,port_start,port_end)
 DO UPDATE SET observed_at=excluded.observed_at,expires_at=excluded.expires_at,source=excluded.source`, `INSERT INTO runtime_port_claims
 (worker_id,device_id,role,resource_kind,instance_id,port_start,port_end,observed_at,expires_at,source)
 VALUES(?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE observed_at=VALUES(observed_at),expires_at=VALUES(expires_at),source=VALUES(source)`)
	for _, claim := range claims {
		if _, err = tx.ExecContext(ctx, query, claim.WorkerID, claim.DeviceID, claim.Role, claim.ResourceKind,
			claim.InstanceID, claim.PortStart, claim.PortEnd, claim.ObservedAt, claim.ExpiresAt, claim.Source); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (s *Store) RuntimePortClaims(ctx context.Context, deviceID, role string) ([]RuntimePortClaim, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT worker_id,device_id,role,resource_kind,instance_id,
 port_start,port_end,observed_at,expires_at,source FROM runtime_port_claims WHERE device_id=? AND role=?
 ORDER BY resource_kind,port_start,instance_id`, deviceID, role)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := []RuntimePortClaim{}
	for rows.Next() {
		var claim RuntimePortClaim
		if err = rows.Scan(&claim.WorkerID, &claim.DeviceID, &claim.Role, &claim.ResourceKind, &claim.InstanceID,
			&claim.PortStart, &claim.PortEnd, &claim.ObservedAt, &claim.ExpiresAt, &claim.Source); err != nil {
			return nil, err
		}
		result = append(result, claim)
	}
	return result, rows.Err()
}

func runtimeClaimConflict(ctx context.Context, queryer rowQuerier, spec LineSpec, role, resourceKind string, first, last int) (string, error) {
	deviceID := lineDevice(spec, role)
	var instanceID string
	err := queryer.QueryRowContext(ctx, `SELECT instance_id FROM runtime_port_claims
 WHERE device_id=? AND role=? AND resource_kind=? AND instance_id<>? AND ?<=port_end AND port_start<=?
 ORDER BY port_start LIMIT 1`, deviceID, role, resourceKind, spec.InstanceID, first, last).Scan(&instanceID)
	if err == nil {
		return instanceID, nil
	}
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	return "", fmt.Errorf("查询运行时端口声明失败: %w", err)
}

func (s *Store) PrepareLineOpenOperation(ctx context.Context, operationID, lineID string) (Operation, error) {
	s.specMu.Lock()
	defer s.specMu.Unlock()
	unlock := s.lockWrite()
	defer unlock()
	var operation Operation
	if err := scanOperation(s.db.QueryRowContext(ctx, `SELECT id,line_id,kind,status,requested_by,idempotency_key,
	 request,result,created_at,updated_at FROM operations WHERE id=? AND line_id=?`, operationID, lineID), &operation); err != nil {
		return Operation{}, err
	}
	if operation.Kind != "line.open" || (operation.Status != "dispatched" && operation.Status != "running") {
		return Operation{}, errors.New("开线任务未进入端口预占阶段")
	}
	spec, err := s.LineSpec(ctx, lineID)
	if err != nil {
		return Operation{}, err
	}
	candidate := spec
	if candidate.SocksPortAuto {
		candidate.SocksPort = 0
	}
	if candidate.RelayPortAuto {
		candidate.RelayPort = 0
	}
	if candidate.ExitPortAuto {
		candidate.ExitPort = 0
	}
	if candidate.UDPPortsAuto {
		candidate.UDPPortMin, candidate.UDPPortMax = 0, 0
	}
	candidate, err = s.AllocateLineSpec(ctx, candidate)
	if err != nil {
		return Operation{}, err
	}
	if conflict, conflictErr := s.LineSpecConflict(ctx, candidate); conflictErr != nil {
		return Operation{}, conflictErr
	} else if conflict != "" {
		return Operation{}, errors.New(conflict)
	}
	var request map[string]any
	if len(operation.Request) == 0 || json.Unmarshal(operation.Request, &request) != nil {
		return Operation{}, errors.New("开线任务计划无效")
	}
	request["plan"] = candidate
	encoded, err := json.Marshal(request)
	if err != nil {
		return Operation{}, err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Operation{}, err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, `UPDATE line_specs SET socks_port=?,relay_port=?,exit_port=?,udp_port_min=?,udp_port_max=?,updated_at=? WHERE line_id=?`,
		candidate.SocksPort, candidate.RelayPort, candidate.ExitPort, candidate.UDPPortMin, candidate.UDPPortMax, now(), lineID); err != nil {
		return Operation{}, err
	}
	updated, err := tx.ExecContext(ctx, `UPDATE operations SET request=?,updated_at=? WHERE id=? AND line_id=? AND status IN ('dispatched','running')`,
		encoded, now(), operationID, lineID)
	if err != nil {
		return Operation{}, err
	}
	if count, countErr := updated.RowsAffected(); countErr != nil || count != 1 {
		return Operation{}, errors.New("开线任务端口预占冲突")
	}
	if err = tx.Commit(); err != nil {
		return Operation{}, err
	}
	operation.Request = encoded
	return operation, nil
}
