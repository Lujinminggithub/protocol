package central

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
	"strings"
	"time"
)

type LineQualification struct {
	LineID                 string          `json:"line_id"`
	DeploymentID           string          `json:"deployment_id"`
	OperationID            string          `json:"operation_id"`
	TargetUpstreamMbps     float64         `json:"target_upstream_mbps"`
	TargetDownstreamMbps   float64         `json:"target_downstream_mbps"`
	AchievedUpstreamMbps   float64         `json:"achieved_upstream_mbps"`
	AchievedDownstreamMbps float64         `json:"achieved_downstream_mbps"`
	DurationSeconds        int             `json:"duration_seconds"`
	RequiredRatio          float64         `json:"required_ratio"`
	Status                 string          `json:"status"`
	Reasons                json.RawMessage `json:"reasons"`
	Evidence               json.RawMessage `json:"evidence"`
	CreatedAt              string          `json:"created_at"`
}

func validateLineQualification(item LineQualification) error {
	if strings.TrimSpace(item.LineID) == "" || strings.TrimSpace(item.DeploymentID) == "" ||
		strings.TrimSpace(item.OperationID) == "" {
		return fmt.Errorf("line, deployment, and operation IDs are required")
	}
	for name, value := range map[string]float64{
		"target_upstream_mbps": item.TargetUpstreamMbps, "target_downstream_mbps": item.TargetDownstreamMbps,
		"achieved_upstream_mbps": item.AchievedUpstreamMbps, "achieved_downstream_mbps": item.AchievedDownstreamMbps,
	} {
		if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 || value > 100000 ||
			(strings.HasPrefix(name, "target_") && value == 0) {
			return fmt.Errorf("invalid %s", name)
		}
	}
	if item.DurationSeconds < 10 || item.DurationSeconds > 600 {
		return fmt.Errorf("duration_seconds must be in 10..600")
	}
	if math.IsNaN(item.RequiredRatio) || math.IsInf(item.RequiredRatio, 0) ||
		item.RequiredRatio < 0.5 || item.RequiredRatio > 1.0 {
		return fmt.Errorf("required_ratio must be in 0.5..1.0")
	}
	if item.Status != "admitted" && item.Status != "rejected" {
		return fmt.Errorf("status must be admitted or rejected")
	}
	var reasons []string
	if len(item.Reasons) == 0 || json.Unmarshal(item.Reasons, &reasons) != nil {
		return fmt.Errorf("reasons must be a JSON string array")
	}
	var evidence map[string]any
	if len(item.Evidence) == 0 || json.Unmarshal(item.Evidence, &evidence) != nil || evidence == nil {
		return fmt.Errorf("evidence must be a JSON object")
	}
	return nil
}

func scanLineQualification(row scanner, item *LineQualification) error {
	return row.Scan(&item.OperationID, &item.LineID, &item.DeploymentID,
		&item.TargetUpstreamMbps, &item.TargetDownstreamMbps, &item.AchievedUpstreamMbps,
		&item.AchievedDownstreamMbps, &item.DurationSeconds, &item.RequiredRatio,
		&item.Status, &item.Reasons, &item.Evidence, &item.CreatedAt)
}

const lineQualificationColumns = `operation_id,line_id,deployment_id,target_upstream_mbps,
 target_downstream_mbps,achieved_upstream_mbps,achieved_downstream_mbps,duration_seconds,
 required_ratio,status,reasons,evidence,created_at`

func (s *Store) SaveLineQualification(ctx context.Context, item LineQualification) (LineQualification, error) {
	if err := validateLineQualification(item); err != nil {
		return LineQualification{}, err
	}
	var existing LineQualification
	err := scanLineQualification(s.db.QueryRowContext(ctx, `SELECT `+lineQualificationColumns+
		` FROM line_qualifications WHERE operation_id=?`, item.OperationID), &existing)
	if err == nil {
		return existing, nil
	}
	if err != sql.ErrNoRows {
		return LineQualification{}, err
	}
	item.CreatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	_, err = s.db.ExecContext(ctx, `INSERT INTO line_qualifications (`+lineQualificationColumns+
		`) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`, item.OperationID, item.LineID, item.DeploymentID,
		item.TargetUpstreamMbps, item.TargetDownstreamMbps, item.AchievedUpstreamMbps,
		item.AchievedDownstreamMbps, item.DurationSeconds, item.RequiredRatio, item.Status,
		[]byte(item.Reasons), []byte(item.Evidence), item.CreatedAt)
	return item, err
}

func lineQualificationFromResult(lineID, deploymentID, operationID string, expectedUpstream,
	expectedDownstream float64, result json.RawMessage) (LineQualification, error) {
	var envelope struct {
		Evidence json.RawMessage `json:"evidence"`
	}
	if json.Unmarshal(result, &envelope) != nil || len(envelope.Evidence) == 0 {
		return LineQualification{}, fmt.Errorf("qualification result requires evidence")
	}
	var probe struct {
		Admission struct {
			Status                 string   `json:"status"`
			Reasons                []string `json:"reasons"`
			TargetUpstreamMbps     float64  `json:"target_upstream_mbps"`
			TargetDownstreamMbps   float64  `json:"target_downstream_mbps"`
			AchievedUpstreamMbps   float64  `json:"achieved_upstream_mbps"`
			AchievedDownstreamMbps float64  `json:"achieved_downstream_mbps"`
			RequiredRatio          float64  `json:"required_ratio"`
			MinimumThroughputRatio float64  `json:"minimum_throughput_ratio"`
			DurationSeconds        int      `json:"duration_seconds"`
		} `json:"admission"`
	}
	if json.Unmarshal(envelope.Evidence, &probe) != nil {
		return LineQualification{}, fmt.Errorf("qualification evidence is invalid")
	}
	if probe.Admission.RequiredRatio == 0 {
		probe.Admission.RequiredRatio = probe.Admission.MinimumThroughputRatio
	}
	reasons, err := json.Marshal(probe.Admission.Reasons)
	if err != nil {
		return LineQualification{}, err
	}
	item := LineQualification{LineID: lineID, DeploymentID: deploymentID, OperationID: operationID,
		TargetUpstreamMbps:     probe.Admission.TargetUpstreamMbps,
		TargetDownstreamMbps:   probe.Admission.TargetDownstreamMbps,
		AchievedUpstreamMbps:   probe.Admission.AchievedUpstreamMbps,
		AchievedDownstreamMbps: probe.Admission.AchievedDownstreamMbps,
		DurationSeconds:        probe.Admission.DurationSeconds, RequiredRatio: probe.Admission.RequiredRatio,
		Status: probe.Admission.Status, Reasons: reasons, Evidence: envelope.Evidence}
	if err = validateLineQualification(item); err != nil {
		return LineQualification{}, err
	}
	if item.DurationSeconds != 90 || math.Abs(item.RequiredRatio-0.95) > 0.000001 {
		return LineQualification{}, fmt.Errorf("production qualification requires 90 seconds at ratio 0.95")
	}
	if math.Abs(item.TargetUpstreamMbps-expectedUpstream) > 0.000001 ||
		math.Abs(item.TargetDownstreamMbps-expectedDownstream) > 0.000001 {
		return LineQualification{}, fmt.Errorf("qualification targets do not match configured directional rates")
	}
	meetsThreshold := item.AchievedUpstreamMbps >= item.TargetUpstreamMbps*item.RequiredRatio &&
		item.AchievedDownstreamMbps >= item.TargetDownstreamMbps*item.RequiredRatio
	if item.Status == "admitted" && (!meetsThreshold || len(probe.Admission.Reasons) != 0) {
		return LineQualification{}, fmt.Errorf("admitted qualification contradicts measured evidence")
	}
	if item.Status == "rejected" && len(probe.Admission.Reasons) == 0 {
		return LineQualification{}, fmt.Errorf("rejected qualification requires reasons")
	}
	return item, nil
}

func saveLineQualificationTx(ctx context.Context, tx *sql.Tx, item LineQualification) error {
	item.CreatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	_, err := tx.ExecContext(ctx, `INSERT INTO line_qualifications (`+lineQualificationColumns+
		`) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`, item.OperationID, item.LineID, item.DeploymentID,
		item.TargetUpstreamMbps, item.TargetDownstreamMbps, item.AchievedUpstreamMbps,
		item.AchievedDownstreamMbps, item.DurationSeconds, item.RequiredRatio, item.Status,
		[]byte(item.Reasons), []byte(item.Evidence), item.CreatedAt)
	return err
}

func (s *Store) ClientDeliveryAllowed(ctx context.Context, lineID string) error {
	line, err := s.Line(ctx, lineID)
	if err != nil {
		return err
	}
	if line.Status != "active" {
		return fmt.Errorf("client configuration requires an active line")
	}
	if line.Environment != "production" {
		return nil
	}
	findings, err := s.ProductionLineFindings(ctx, lineID)
	if err != nil {
		return err
	}
	if len(findings) > 0 {
		return fmt.Errorf("client configuration is blocked by production governance: %s", findings[0].Code)
	}
	qualification, err := s.LatestLineQualification(ctx, lineID)
	if err != nil {
		return fmt.Errorf("client configuration requires admitted qualification")
	}
	if qualification.Status != "admitted" || qualification.DeploymentID != line.ActiveDeployment {
		return fmt.Errorf("client configuration requires admitted qualification for current deployment")
	}
	return nil
}

func (s *Store) LatestLineQualification(ctx context.Context, lineID string) (LineQualification, error) {
	var item LineQualification
	err := scanLineQualification(s.db.QueryRowContext(ctx, `SELECT `+lineQualificationColumns+
		` FROM line_qualifications WHERE line_id=? ORDER BY created_at DESC,operation_id DESC LIMIT 1`, lineID), &item)
	return item, err
}
