package central

import (
	"context"
	"database/sql"
	"fmt"
	"sort"
)

type GovernanceFinding struct {
	LineID         string `json:"line_id"`
	Code           string `json:"code"`
	ResourceID     string `json:"resource_id,omitempty"`
	Message        string `json:"message"`
	RequiredAction string `json:"required_action"`
}

func (s *Store) AuditProductionLines(ctx context.Context) ([]GovernanceFinding, error) {
	return s.auditProductionLines(ctx, "")
}

func (s *Store) auditProductionLines(ctx context.Context, lineID string) ([]GovernanceFinding, error) {
	tx, err := s.db.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	query := `SELECT n.line_id,n.device_id,d.environment FROM line_nodes n
 JOIN devices d ON d.id=n.device_id JOIN ` + s.linesTable() + ` l ON l.id=n.line_id
 WHERE l.environment='production' AND l.status<>'archived'`
	args := []any{}
	if lineID != "" {
		query += ` AND n.line_id=?`
		args = append(args, lineID)
	}
	query += ` ORDER BY n.line_id,n.role,n.ordinal`
	rows, err := tx.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	findings := []GovernanceFinding{}
	for rows.Next() {
		var currentLine, deviceID, environment string
		if err = rows.Scan(&currentLine, &deviceID, &environment); err != nil {
			return nil, err
		}
		if environment != "production" {
			findings = append(findings, GovernanceFinding{LineID: currentLine, Code: "maintenance_required",
				ResourceID: deviceID, Message: fmt.Sprintf("生产线路引用了 %s 环境设备 %s", environment, deviceID),
				RequiredAction: "将线路迁移到生产设备，或把线路明确改为测试环境"})
		}
	}
	if err = rows.Err(); err != nil {
		return nil, err
	}
	sort.Slice(findings, func(i, j int) bool {
		if findings[i].LineID != findings[j].LineID {
			return findings[i].LineID < findings[j].LineID
		}
		return findings[i].ResourceID < findings[j].ResourceID
	})
	return findings, tx.Commit()
}

func (s *Store) ProductionLineFindings(ctx context.Context, lineID string) ([]GovernanceFinding, error) {
	return s.auditProductionLines(ctx, lineID)
}
