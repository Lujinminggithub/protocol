package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

var (
	ErrLineDeletionValidation = errors.New("线路删除确认无效")
	ErrLineDeletionConflict   = errors.New("线路当前不能删除")
)

type LineDeletionRequest struct {
	RequestedBy        string `json:"requested_by"`
	Reason             string `json:"reason"`
	Force              bool   `json:"force"`
	Confirmation       string `json:"confirmation"`
	AcknowledgeOrphans bool   `json:"acknowledge_orphans"`
}

type lineDeletionDevice struct {
	ID       string `json:"id"`
	Role     string `json:"role"`
	Health   string `json:"health"`
	LastSeen string `json:"last_seen_at"`
}

func validateLineDeletion(id string, request LineDeletionRequest) error {
	if strings.TrimSpace(request.RequestedBy) == "" || strings.TrimSpace(request.Reason) == "" {
		return fmt.Errorf("%w：发起人和删除原因不能为空", ErrLineDeletionValidation)
	}
	if !request.Force {
		return nil
	}
	if request.Confirmation != id {
		return fmt.Errorf("%w：请输入完整线路 ID %s", ErrLineDeletionValidation, id)
	}
	if !request.AcknowledgeOrphans {
		return fmt.Errorf("%w：必须确认不可达节点可能残留远端进程和文件", ErrLineDeletionValidation)
	}
	return nil
}

func (s *Store) DeleteLine(ctx context.Context, id string, request LineDeletionRequest) error {
	if err := validateLineDeletion(id, request); err != nil {
		return err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var line Line
	err = tx.QueryRowContext(ctx, `SELECT id,name,status,entry_region,exit_region,provider,capacity_mbps,
 active_deployment,profile,secret_ref,created_at,updated_at FROM lines WHERE id=?`, id).Scan(
		&line.ID, &line.Name, &line.Status, &line.EntryRegion, &line.ExitRegion, &line.Provider,
		&line.CapacityMbps, &line.ActiveDeployment, &line.Profile, &line.SecretRef, &line.CreatedAt, &line.UpdatedAt)
	if err != nil {
		return err
	}
	var active, failedStops, specs, snapshots, incidents, operations int
	for query, target := range map[string]*int{
		`SELECT COUNT(*) FROM operations WHERE line_id=? AND status IN ('queued','dispatched','running')`: &active,
		`SELECT COUNT(*) FROM operations WHERE line_id=? AND kind='line.disable' AND status='failed'`:     &failedStops,
		`SELECT COUNT(*) FROM line_specs WHERE line_id=?`:                                                 &specs,
		`SELECT COUNT(*) FROM snapshots WHERE line_id=?`:                                                  &snapshots,
		`SELECT COUNT(*) FROM incidents WHERE line_id=?`:                                                  &incidents,
		`SELECT COUNT(*) FROM operations WHERE line_id=?`:                                                 &operations,
	} {
		if err = tx.QueryRowContext(ctx, query, id).Scan(target); err != nil {
			return err
		}
	}
	if active > 0 {
		return fmt.Errorf("%w：线路仍有排队中或执行中的任务", ErrLineDeletionConflict)
	}
	configuredActive := specs > 0 && line.Status != "draft" && line.Status != "disabled" && line.Status != "archived"
	if configuredActive && !request.Force {
		return fmt.Errorf("%w：已配置线路必须先停用；节点不可达时请使用人工确认强制删除", ErrLineDeletionConflict)
	}
	devices, err := deletionDevices(ctx, tx, id)
	if err != nil {
		return err
	}
	if configuredActive && request.Force && failedStops == 0 && !hasDeletionBlockingDevice(devices) {
		return fmt.Errorf("%w：线路没有异常节点或失败停用任务，请先正常停用", ErrLineDeletionConflict)
	}
	audit, err := json.Marshal(map[string]any{
		"force": request.Force, "confirmation": request.Confirmation,
		"acknowledge_orphans": request.AcknowledgeOrphans, "devices": devices,
		"line": map[string]any{"id": line.ID, "name": line.Name, "status": line.Status,
			"entry_region": line.EntryRegion, "exit_region": line.ExitRegion, "provider": line.Provider,
			"capacity_mbps": line.CapacityMbps, "active_deployment": line.ActiveDeployment, "profile": line.Profile},
		"counts": map[string]int{"specs": specs, "snapshots": snapshots, "incidents": incidents,
			"operations": operations, "failed_disable_operations": failedStops},
	})
	if err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO line_deletion_audit
 (line_id,line_name,requested_by,reason,snapshot,deleted_at) VALUES(?,?,?,?,?,?)`,
		line.ID, line.Name, request.RequestedBy, request.Reason, audit, now()); err != nil {
		return err
	}
	for _, query := range []string{
		`DELETE FROM operation_events WHERE operation_id IN (SELECT id FROM operations WHERE line_id=?)`,
		`DELETE FROM operations WHERE line_id=?`, `DELETE FROM snapshots WHERE line_id=?`,
		`DELETE FROM incidents WHERE line_id=?`, `DELETE FROM line_specs WHERE line_id=?`,
		`DELETE FROM lines WHERE id=?`,
	} {
		if _, err = tx.ExecContext(ctx, query, id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func hasDeletionBlockingDevice(devices []lineDeletionDevice) bool {
	for _, device := range devices {
		switch device.Health {
		case "unreachable", "down", "degraded", "unhealthy", "offline":
			return true
		}
	}
	return false
}

func deletionDevices(ctx context.Context, tx *sql.Tx, lineID string) ([]lineDeletionDevice, error) {
	rows, err := tx.QueryContext(ctx, `SELECT d.id,n.role,d.last_health,d.last_seen_at
 FROM line_nodes n JOIN devices d ON d.id=n.device_id WHERE n.line_id=?
 ORDER BY CASE n.role WHEN 'entry' THEN 1 WHEN 'relay' THEN 2 ELSE 3 END,n.ordinal,d.id`, lineID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := make([]lineDeletionDevice, 0)
	for rows.Next() {
		var device lineDeletionDevice
		if err = rows.Scan(&device.ID, &device.Role, &device.Health, &device.LastSeen); err != nil {
			return nil, err
		}
		devices = append(devices, device)
	}
	return devices, rows.Err()
}
