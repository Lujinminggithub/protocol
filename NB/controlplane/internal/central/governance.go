package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
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
	filter := ""
	args := []any{}
	if lineID != "" {
		filter = ` AND id=?`
		args = append(args, lineID)
	}
	query := `SELECT id,active_deployment,profile FROM ` + s.linesTable() + `
 WHERE environment='production' AND status<>'archived'`
	query += filter + ` ORDER BY id`
	rows, err := tx.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	type auditedLine struct{ id, deployment, profile string }
	lines := []auditedLine{}
	for rows.Next() {
		var line auditedLine
		if err = rows.Scan(&line.id, &line.deployment, &line.profile); err != nil {
			_ = rows.Close()
			return nil, err
		}
		lines = append(lines, line)
	}
	if err = rows.Err(); err != nil {
		_ = rows.Close()
		return nil, err
	}
	if err = rows.Close(); err != nil {
		return nil, err
	}
	if len(lines) == 0 {
		return []GovernanceFinding{}, tx.Commit()
	}

	type nodeRecord struct{ deviceID, role, environment string }
	nodesByLine := map[string]map[string]nodeRecord{}
	nodeCounts := map[string]map[string]int{}
	nonProductionNodes := map[string][]nodeRecord{}
	nodeQuery := `SELECT n.line_id,n.device_id,n.role,d.environment FROM line_nodes n
 JOIN devices d ON d.id=n.device_id JOIN ` + s.linesTable() + ` l ON l.id=n.line_id
 WHERE l.environment='production' AND l.status<>'archived'`
	nodeArgs := []any{}
	if lineID != "" {
		nodeQuery += ` AND n.line_id=?`
		nodeArgs = append(nodeArgs, lineID)
	}
	nodeRows, err := tx.QueryContext(ctx, nodeQuery, nodeArgs...)
	if err != nil {
		return nil, err
	}
	for nodeRows.Next() {
		var record nodeRecord
		var currentLine string
		if err = nodeRows.Scan(&currentLine, &record.deviceID, &record.role, &record.environment); err != nil {
			_ = nodeRows.Close()
			return nil, err
		}
		if nodesByLine[currentLine] == nil {
			nodesByLine[currentLine] = map[string]nodeRecord{}
			nodeCounts[currentLine] = map[string]int{}
		}
		nodesByLine[currentLine][record.role] = record
		nodeCounts[currentLine][record.role]++
		if record.environment != "production" {
			nonProductionNodes[currentLine] = append(nonProductionNodes[currentLine], record)
		}
	}
	if err = nodeRows.Err(); err != nil {
		_ = nodeRows.Close()
		return nil, err
	}
	if err = nodeRows.Close(); err != nil {
		return nil, err
	}

	type rates struct{ upstream, downstream int }
	ratesByLine := map[string]rates{}
	specQuery := `SELECT s.line_id,CASE WHEN s.upstream_mbps>0 THEN s.upstream_mbps ELSE s.bandwidth_mbps END,
 CASE WHEN s.downstream_mbps>0 THEN s.downstream_mbps ELSE s.bandwidth_mbps END FROM line_specs s
 JOIN ` + s.linesTable() + ` l ON l.id=s.line_id WHERE l.environment='production' AND l.status<>'archived'`
	specArgs := []any{}
	if lineID != "" {
		specQuery += ` AND s.line_id=?`
		specArgs = append(specArgs, lineID)
	}
	specRows, err := tx.QueryContext(ctx, specQuery, specArgs...)
	if err != nil {
		return nil, err
	}
	for specRows.Next() {
		var currentLine string
		var item rates
		if err = specRows.Scan(&currentLine, &item.upstream, &item.downstream); err != nil {
			_ = specRows.Close()
			return nil, err
		}
		ratesByLine[currentLine] = item
	}
	if err = specRows.Err(); err != nil {
		_ = specRows.Close()
		return nil, err
	}
	if err = specRows.Close(); err != nil {
		return nil, err
	}

	linkByIdentity := map[string]NetworkLink{}
	linkRows, err := tx.QueryContext(ctx, `SELECT id,from_device_id,from_role,to_device_id,to_role,
 forward_capacity_mbps,reverse_capacity_mbps,billing_mode,environment,status,created_at,updated_at FROM network_links`)
	if err != nil {
		return nil, err
	}
	for linkRows.Next() {
		var link NetworkLink
		if err = scanNetworkLink(linkRows, &link); err != nil {
			_ = linkRows.Close()
			return nil, err
		}
		linkByIdentity[link.FromDeviceID+"\x00"+link.FromRole+"\x00"+link.ToDeviceID+"\x00"+link.ToRole] = link
	}
	if err = linkRows.Err(); err != nil {
		_ = linkRows.Close()
		return nil, err
	}
	if err = linkRows.Close(); err != nil {
		return nil, err
	}

	latestQualification := map[string]LineQualification{}
	qualificationQuery := `SELECT q.operation_id,q.line_id,q.deployment_id,q.target_upstream_mbps,
 q.target_downstream_mbps,q.achieved_upstream_mbps,q.achieved_downstream_mbps,q.duration_seconds,
 q.required_ratio,q.status,q.reasons,q.evidence,q.created_at FROM line_qualifications q
 JOIN ` + s.linesTable() + ` l ON l.id=q.line_id WHERE l.environment='production' AND l.status<>'archived'`
	qualificationArgs := []any{}
	if lineID != "" {
		qualificationQuery += ` AND q.line_id=?`
		qualificationArgs = append(qualificationArgs, lineID)
	}
	qualificationQuery += ` ORDER BY q.line_id,q.created_at DESC,q.operation_id DESC`
	qualificationRows, err := tx.QueryContext(ctx, qualificationQuery, qualificationArgs...)
	if err != nil {
		return nil, err
	}
	for qualificationRows.Next() {
		var item LineQualification
		if err = scanLineQualification(qualificationRows, &item); err != nil {
			_ = qualificationRows.Close()
			return nil, err
		}
		if _, exists := latestQualification[item.LineID]; !exists {
			latestQualification[item.LineID] = item
		}
	}
	if err = qualificationRows.Err(); err != nil {
		_ = qualificationRows.Close()
		return nil, err
	}
	if err = qualificationRows.Close(); err != nil {
		return nil, err
	}

	operationProfiles := map[string]string{}
	operationQuery := `SELECT o.id,o.result FROM operations o JOIN line_qualifications q ON q.operation_id=o.id
 JOIN ` + s.linesTable() + ` l ON l.id=q.line_id WHERE l.environment='production' AND l.status<>'archived'`
	operationArgs := []any{}
	if lineID != "" {
		operationQuery += ` AND q.line_id=?`
		operationArgs = append(operationArgs, lineID)
	}
	operationRows, err := tx.QueryContext(ctx, operationQuery, operationArgs...)
	if err != nil {
		return nil, err
	}
	for operationRows.Next() {
		var operationID string
		var raw []byte
		if err = operationRows.Scan(&operationID, &raw); err != nil {
			_ = operationRows.Close()
			return nil, err
		}
		var result struct {
			Profile string `json:"profile"`
		}
		if json.Unmarshal(raw, &result) == nil {
			operationProfiles[operationID] = result.Profile
		}
	}
	if err = operationRows.Err(); err != nil {
		_ = operationRows.Close()
		return nil, err
	}
	if err = operationRows.Close(); err != nil {
		return nil, err
	}

	findings := []GovernanceFinding{}
	for _, line := range lines {
		nodes, counts, rate := nodesByLine[line.id], nodeCounts[line.id], ratesByLine[line.id]
		for _, node := range nonProductionNodes[line.id] {
			findings = append(findings, GovernanceFinding{LineID: line.id, Code: "maintenance_required",
				ResourceID: node.deviceID, Message: fmt.Sprintf("生产线路引用了 %s 环境设备 %s", node.environment, node.deviceID),
				RequiredAction: "将线路迁移到生产设备，或把线路明确改为测试环境"})
		}
		if counts["entry"] != 1 || counts["relay"] != 1 || counts["exit"] != 1 ||
			rate.upstream < 1 || rate.downstream < 1 {
			findings = append(findings, GovernanceFinding{LineID: line.id, Code: "capacity_unknown",
				Message: "无法从当前三节点拓扑确定两段物理链路", RequiredAction: "修复线路拓扑并登记两段物理链路"})
		} else {
			for _, endpoints := range [][4]string{{nodes["entry"].deviceID, "entry", nodes["relay"].deviceID, "relay"},
				{nodes["relay"].deviceID, "relay", nodes["exit"].deviceID, "exit"}} {
				identity := endpoints[0] + "\x00" + endpoints[1] + "\x00" + endpoints[2] + "\x00" + endpoints[3]
				link, ok := linkByIdentity[identity]
				resourceID := link.ID
				if !ok {
					resourceID = fmt.Sprintf("%s:%s->%s:%s", endpoints[0], endpoints[1], endpoints[2], endpoints[3])
				}
				if !ok || link.Environment != "production" || link.Status != "ready" ||
					link.ForwardCapacityMbps < rate.upstream || link.ReverseCapacityMbps < rate.downstream {
					findings = append(findings, GovernanceFinding{LineID: line.id, Code: "capacity_unknown",
						ResourceID: resourceID, Message: "当前拓扑链路未登记、不可用或方向容量不足",
						RequiredAction: "登记并启用足够的生产物理链路前向和反向容量"})
					continue
				}
				if link.BillingMode != LinkBillingIndependent {
					findings = append(findings, GovernanceFinding{LineID: line.id, Code: "full_duplex_unqualified",
						ResourceID: link.ID, Message: "链路不是按方向独立计费，不能证明 10 Mbps 全双工",
						RequiredAction: "向供应商确认并登记按方向独立的链路容量，不可用双向合计替代"})
				}
			}
		}

		qualification, hasQualification := latestQualification[line.id]
		var reasons []string
		validReasons := json.Unmarshal(qualification.Reasons, &reasons) == nil && len(reasons) == 0
		qualificationCurrent := hasQualification && qualification.Status == "admitted" && validReasons &&
			qualification.DeploymentID == line.deployment && qualification.DurationSeconds == 90 &&
			math.Abs(qualification.RequiredRatio-0.95) <= 0.000001 &&
			math.Abs(qualification.TargetUpstreamMbps-float64(rate.upstream)) <= 0.000001 &&
			math.Abs(qualification.TargetDownstreamMbps-float64(rate.downstream)) <= 0.000001 &&
			qualification.AchievedUpstreamMbps >= qualification.TargetUpstreamMbps*qualification.RequiredRatio &&
			qualification.AchievedDownstreamMbps >= qualification.TargetDownstreamMbps*qualification.RequiredRatio &&
			(line.profile == "" || operationProfiles[qualification.OperationID] == line.profile)
		if !qualificationCurrent {
			findings = append(findings, GovernanceFinding{LineID: line.id, Code: "qualification_required",
				ResourceID: line.deployment, Message: "当前 deployment/profile 缺少有效的 90 秒全双工资格证据",
				RequiredAction: "执行验证并调优，两方向分别达到配置速率的 95%"})
		}
	}
	sort.Slice(findings, func(i, j int) bool {
		if findings[i].LineID != findings[j].LineID {
			return findings[i].LineID < findings[j].LineID
		}
		if findings[i].Code != findings[j].Code {
			return findings[i].Code < findings[j].Code
		}
		return findings[i].ResourceID < findings[j].ResourceID
	})
	return findings, tx.Commit()
}

func (s *Store) ProductionLineFindings(ctx context.Context, lineID string) ([]GovernanceFinding, error) {
	return s.auditProductionLines(ctx, lineID)
}
