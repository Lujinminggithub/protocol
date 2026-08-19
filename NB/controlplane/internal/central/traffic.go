package central

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"time"
)

const maxTrafficRange = 365 * 24 * time.Hour

type TrafficPoint struct {
	ObservedAt     string  `json:"observed_at"`
	UpstreamMbps   float64 `json:"upstream_mbps"`
	DownstreamMbps float64 `json:"downstream_mbps"`
	Sessions       int64   `json:"sessions"`
	QueueAgeMaxUS  float64 `json:"queue_age_max_us"`
	EffectiveLoss  float64 `json:"effective_loss_pct"`
	Health         string  `json:"health"`
	Deployment     string  `json:"deployment"`
	Profile        string  `json:"profile"`
}

type TrafficSeries struct {
	Key      string         `json:"key"`
	Role     string         `json:"role"`
	NodeID   string         `json:"node_id,omitempty"`
	WorkerID string         `json:"worker_id,omitempty"`
	Points   []TrafficPoint `json:"points"`
}

type TrafficMarker struct {
	ObservedAt string `json:"observed_at"`
	Kind       string `json:"kind"`
	Label      string `json:"label"`
}

type TrafficHistoryResult struct {
	LineID            string          `json:"line_id"`
	From              string          `json:"from"`
	To                string          `json:"to"`
	ResolutionSeconds int             `json:"resolution_s"`
	Roles             []TrafficSeries `json:"roles"`
	Workers           []TrafficSeries `json:"workers"`
	Markers           []TrafficMarker `json:"markers"`
}

func (h TrafficHistoryResult) Role(role string) TrafficSeries {
	for _, series := range h.Roles {
		if series.Role == role {
			return series
		}
	}
	return TrafficSeries{Key: role, Role: role, Points: []TrafficPoint{}}
}

func TrafficResolution(span time.Duration) int {
	switch {
	case span <= 6*time.Hour:
		return 15
	case span <= 7*24*time.Hour:
		return 60
	default:
		return 300
	}
}

func healthRank(health string) int {
	switch health {
	case "down", "offline", "unhealthy":
		return 2
	case "degraded":
		return 1
	default:
		return 0
	}
}

func (s *Store) backfillTrafficRollups(ctx context.Context) error {
	for _, resolution := range []int{60, 300} {
		_, err := s.db.ExecContext(ctx, `WITH source AS (
 SELECT line_id,node_id,role,worker_id,
  strftime('%Y-%m-%dT%H:%M:%SZ',(CAST(strftime('%s',observed_at) AS INTEGER)/?)*?,'unixepoch') AS bucket_at,
  observed_at,upstream_mbps,downstream_mbps,sessions,queue_age_p95_us,effective_loss_pct,health,deployment,profile
 FROM snapshots
), ranked AS (
 SELECT *,ROW_NUMBER() OVER (
  PARTITION BY line_id,node_id,worker_id,bucket_at ORDER BY observed_at DESC
 ) AS rn FROM source
), aggregate AS (
 SELECT line_id,node_id,role,worker_id,bucket_at,COUNT(*) AS sample_count,
  SUM(upstream_mbps) AS upstream_sum,SUM(downstream_mbps) AS downstream_sum,
  MAX(sessions) AS sessions_max,MAX(queue_age_p95_us) AS queue_age_max_us,
  MAX(effective_loss_pct) AS effective_loss_max_pct,
  MAX(CASE WHEN health IN ('down','offline','unhealthy') THEN 2 WHEN health='degraded' THEN 1 ELSE 0 END) AS health_rank,
  MAX(observed_at) AS last_observed_at
 FROM source GROUP BY line_id,node_id,role,worker_id,bucket_at
)
INSERT OR IGNORE INTO traffic_rollups
 (line_id,node_id,role,worker_id,resolution_s,bucket_at,sample_count,upstream_sum,downstream_sum,
  sessions_max,queue_age_max_us,effective_loss_max_pct,health_rank,deployment,profile,last_observed_at)
SELECT aggregate.line_id,aggregate.node_id,aggregate.role,aggregate.worker_id,?,aggregate.bucket_at,
 aggregate.sample_count,aggregate.upstream_sum,aggregate.downstream_sum,aggregate.sessions_max,
 aggregate.queue_age_max_us,aggregate.effective_loss_max_pct,aggregate.health_rank,
 ranked.deployment,ranked.profile,aggregate.last_observed_at
FROM aggregate JOIN ranked ON ranked.line_id=aggregate.line_id AND ranked.node_id=aggregate.node_id
 AND ranked.worker_id=aggregate.worker_id AND ranked.bucket_at=aggregate.bucket_at AND ranked.rn=1`,
			resolution, resolution, resolution)
		if err != nil {
			return fmt.Errorf("backfill %ds traffic rollups: %w", resolution, err)
		}
	}
	return nil
}

// BackfillTrafficHistory performs the one-time historical rollup outside the
// startup migration path. A large production snapshot table must not delay the
// web listener or trigger a systemd restart loop.
func (s *Store) BackfillTrafficHistory(ctx context.Context) (bool, error) {
	const job = "traffic_rollups_v1"
	var completed string
	err := s.db.QueryRowContext(ctx, `SELECT completed_at FROM maintenance_jobs WHERE name=?`, job).Scan(&completed)
	if err == nil {
		return false, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return false, fmt.Errorf("read traffic backfill state: %w", err)
	}
	if err = s.backfillTrafficRollups(ctx); err != nil {
		return false, err
	}
	if _, err = s.db.ExecContext(ctx, `INSERT OR IGNORE INTO maintenance_jobs(name,completed_at) VALUES(?,?)`, job, now()); err != nil {
		return false, fmt.Errorf("record traffic backfill completion: %w", err)
	}
	return true, nil
}

func healthFromRank(rank int) string {
	switch rank {
	case 2:
		return "down"
	case 1:
		return "degraded"
	default:
		return "ok"
	}
}

func upsertTrafficRollup(ctx context.Context, tx *sql.Tx, item Snapshot, bucket time.Time, resolution int) error {
	_, err := tx.ExecContext(ctx, `INSERT INTO traffic_rollups
 (line_id,node_id,role,worker_id,resolution_s,bucket_at,sample_count,upstream_sum,downstream_sum,
  sessions_max,queue_age_max_us,effective_loss_max_pct,health_rank,deployment,profile,last_observed_at)
 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
 ON CONFLICT(line_id,node_id,worker_id,resolution_s,bucket_at) DO UPDATE SET
  sample_count=traffic_rollups.sample_count+1,
  upstream_sum=traffic_rollups.upstream_sum+excluded.upstream_sum,
  downstream_sum=traffic_rollups.downstream_sum+excluded.downstream_sum,
  sessions_max=MAX(traffic_rollups.sessions_max,excluded.sessions_max),
  queue_age_max_us=MAX(traffic_rollups.queue_age_max_us,excluded.queue_age_max_us),
  effective_loss_max_pct=MAX(traffic_rollups.effective_loss_max_pct,excluded.effective_loss_max_pct),
  health_rank=MAX(traffic_rollups.health_rank,excluded.health_rank),
  deployment=CASE WHEN excluded.last_observed_at>=traffic_rollups.last_observed_at THEN excluded.deployment ELSE traffic_rollups.deployment END,
  profile=CASE WHEN excluded.last_observed_at>=traffic_rollups.last_observed_at THEN excluded.profile ELSE traffic_rollups.profile END,
  last_observed_at=MAX(traffic_rollups.last_observed_at,excluded.last_observed_at)`,
		item.LineID, item.NodeID, item.Role, item.WorkerID, resolution, bucket.UTC().Format(time.RFC3339), 1,
		item.UpstreamMbps, item.DownstreamMbps, item.Sessions, item.QueueAgeP95US, item.EffectiveLoss,
		healthRank(item.Health), item.Deployment, item.Profile, item.ObservedAt)
	return err
}

type trafficAggregate struct {
	count      int64
	upstream   float64
	downstream float64
	sessions   int64
	queue      float64
	loss       float64
	health     int
	deployment string
	profile    string
	latest     string
}

func (a *trafficAggregate) add(count int64, upstream, downstream float64, sessions int64, queue, loss float64, health int, deployment, profile, latest string) {
	a.count += count
	a.upstream += upstream
	a.downstream += downstream
	if sessions > a.sessions {
		a.sessions = sessions
	}
	if queue > a.queue {
		a.queue = queue
	}
	if loss > a.loss {
		a.loss = loss
	}
	if health > a.health {
		a.health = health
	}
	if latest >= a.latest {
		a.latest, a.deployment, a.profile = latest, deployment, profile
	}
}

type workerBucket struct {
	role, nodeID, workerID, bucket string
	aggregate                      trafficAggregate
}

func (s *Store) TrafficHistory(ctx context.Context, lineID string, from, to time.Time, resolution int) (TrafficHistoryResult, error) {
	if !from.Before(to) || to.Sub(from) > maxTrafficRange {
		return TrafficHistoryResult{}, errors.New("invalid traffic history range")
	}
	if resolution == 0 {
		resolution = TrafficResolution(to.Sub(from))
	}
	if resolution != 15 && resolution != 60 && resolution != 300 {
		return TrafficHistoryResult{}, errors.New("invalid traffic history resolution")
	}
	if _, err := s.Line(ctx, lineID); err != nil {
		return TrafficHistoryResult{}, err
	}
	workers := map[string]*workerBucket{}
	if resolution == 15 {
		rows, err := s.db.QueryContext(ctx, `SELECT node_id,role,worker_id,observed_at,health,deployment,profile,
 sessions,upstream_mbps,downstream_mbps,queue_age_p95_us,effective_loss_pct
 FROM snapshots WHERE line_id=? AND observed_at>=? AND observed_at<? ORDER BY observed_at`,
			lineID, from.UTC().Format(time.RFC3339Nano), to.UTC().Format(time.RFC3339Nano))
		if err != nil {
			return TrafficHistoryResult{}, err
		}
		defer rows.Close()
		for rows.Next() {
			var nodeID, role, workerID, observedAt, health, deployment, profile string
			var sessions int64
			var upstream, downstream, queue, loss float64
			if err = rows.Scan(&nodeID, &role, &workerID, &observedAt, &health, &deployment, &profile,
				&sessions, &upstream, &downstream, &queue, &loss); err != nil {
				return TrafficHistoryResult{}, err
			}
			parsed, parseErr := time.Parse(time.RFC3339Nano, observedAt)
			if parseErr != nil {
				continue
			}
			bucket := parsed.Truncate(15 * time.Second).UTC().Format(time.RFC3339)
			key := nodeID + "\x00" + workerID + "\x00" + bucket
			item := workers[key]
			if item == nil {
				item = &workerBucket{role: role, nodeID: nodeID, workerID: workerID, bucket: bucket}
				workers[key] = item
			}
			item.aggregate.add(1, upstream, downstream, sessions, queue, loss, healthRank(health), deployment, profile, observedAt)
		}
		if err := rows.Err(); err != nil {
			return TrafficHistoryResult{}, err
		}
	} else {
		rows, err := s.db.QueryContext(ctx, `SELECT node_id,role,worker_id,bucket_at,sample_count,upstream_sum,
 downstream_sum,sessions_max,queue_age_max_us,effective_loss_max_pct,health_rank,deployment,profile,last_observed_at
 FROM traffic_rollups WHERE line_id=? AND resolution_s=? AND bucket_at>=? AND bucket_at<? ORDER BY bucket_at`,
			lineID, resolution, from.UTC().Format(time.RFC3339), to.UTC().Format(time.RFC3339))
		if err != nil {
			return TrafficHistoryResult{}, err
		}
		defer rows.Close()
		for rows.Next() {
			item := &workerBucket{}
			var count int64
			var upstream, downstream, queue, loss float64
			var sessions int64
			var health int
			var deployment, profile, latest string
			if err = rows.Scan(&item.nodeID, &item.role, &item.workerID, &item.bucket, &count, &upstream, &downstream,
				&sessions, &queue, &loss, &health, &deployment, &profile, &latest); err != nil {
				return TrafficHistoryResult{}, err
			}
			item.aggregate.add(count, upstream, downstream, sessions, queue, loss, health, deployment, profile, latest)
			workers[item.nodeID+"\x00"+item.workerID+"\x00"+item.bucket] = item
		}
		if err := rows.Err(); err != nil {
			return TrafficHistoryResult{}, err
		}
	}

	workerSeries := map[string]*TrafficSeries{}
	roleBuckets := map[string]*trafficAggregate{}
	for _, item := range workers {
		if item.aggregate.count == 0 {
			continue
		}
		point := TrafficPoint{ObservedAt: item.bucket,
			UpstreamMbps:   item.aggregate.upstream / float64(item.aggregate.count),
			DownstreamMbps: item.aggregate.downstream / float64(item.aggregate.count),
			Sessions:       item.aggregate.sessions, QueueAgeMaxUS: item.aggregate.queue,
			EffectiveLoss: item.aggregate.loss, Health: healthFromRank(item.aggregate.health),
			Deployment: item.aggregate.deployment, Profile: item.aggregate.profile}
		seriesKey := item.nodeID + ":" + item.workerID
		series := workerSeries[seriesKey]
		if series == nil {
			series = &TrafficSeries{Key: seriesKey, Role: item.role, NodeID: item.nodeID, WorkerID: item.workerID}
			workerSeries[seriesKey] = series
		}
		series.Points = append(series.Points, point)
		roleKey := item.role + "\x00" + item.bucket
		role := roleBuckets[roleKey]
		if role == nil {
			role = &trafficAggregate{}
			roleBuckets[roleKey] = role
		}
		role.add(1, point.UpstreamMbps, point.DownstreamMbps, point.Sessions, point.QueueAgeMaxUS,
			point.EffectiveLoss, healthRank(point.Health), point.Deployment, point.Profile, point.ObservedAt)
	}

	result := TrafficHistoryResult{LineID: lineID, From: from.UTC().Format(time.RFC3339Nano),
		To: to.UTC().Format(time.RFC3339Nano), ResolutionSeconds: resolution,
		Roles: []TrafficSeries{}, Workers: []TrafficSeries{}, Markers: []TrafficMarker{}}
	roleSeries := map[string]*TrafficSeries{}
	for key, aggregate := range roleBuckets {
		role := key[:len(key)-len(key[stringsLastSeparator(key):])]
		bucket := key[stringsLastSeparator(key)+1:]
		series := roleSeries[role]
		if series == nil {
			series = &TrafficSeries{Key: role, Role: role}
			roleSeries[role] = series
		}
		series.Points = append(series.Points, TrafficPoint{ObservedAt: bucket, UpstreamMbps: aggregate.upstream,
			DownstreamMbps: aggregate.downstream, Sessions: aggregate.sessions, QueueAgeMaxUS: aggregate.queue,
			EffectiveLoss: aggregate.loss, Health: healthFromRank(aggregate.health), Deployment: aggregate.deployment, Profile: aggregate.profile})
	}
	for _, role := range []string{"entry", "middle", "exit"} {
		if series := roleSeries[role]; series != nil {
			sort.Slice(series.Points, func(i, j int) bool { return series.Points[i].ObservedAt < series.Points[j].ObservedAt })
			result.Roles = append(result.Roles, *series)
		}
	}
	keys := make([]string, 0, len(workerSeries))
	for key := range workerSeries {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		series := workerSeries[key]
		sort.Slice(series.Points, func(i, j int) bool { return series.Points[i].ObservedAt < series.Points[j].ObservedAt })
		result.Workers = append(result.Workers, *series)
	}
	result.Markers = append(result.Markers, transportMarkers(result.Roles)...)
	operationRows, err := s.db.QueryContext(ctx, `SELECT created_at,kind,status FROM operations
 WHERE line_id=? AND created_at>=? AND created_at<? ORDER BY created_at`, lineID,
		from.UTC().Format(time.RFC3339Nano), to.UTC().Format(time.RFC3339Nano))
	if err != nil {
		return TrafficHistoryResult{}, err
	}
	for operationRows.Next() {
		var marker TrafficMarker
		var status string
		if err = operationRows.Scan(&marker.ObservedAt, &marker.Kind, &status); err != nil {
			return TrafficHistoryResult{}, err
		}
		marker.Label = marker.Kind + " · " + status
		marker.Kind = "operation"
		result.Markers = append(result.Markers, marker)
	}
	if err = operationRows.Err(); err != nil {
		_ = operationRows.Close()
		return TrafficHistoryResult{}, err
	}
	_ = operationRows.Close()
	eventRows, err := s.db.QueryContext(ctx, `SELECT MIN(events.created_at),events.stage,events.status
 FROM operation_events events JOIN operations operation ON operation.id=events.operation_id
 WHERE operation.line_id=? AND events.created_at>=? AND events.created_at<?
 GROUP BY events.operation_id,events.stage,events.status ORDER BY MIN(events.created_at)`, lineID,
		from.UTC().Format(time.RFC3339Nano), to.UTC().Format(time.RFC3339Nano))
	if err != nil {
		return TrafficHistoryResult{}, err
	}
	for eventRows.Next() {
		var observedAt, stage, status string
		if err = eventRows.Scan(&observedAt, &stage, &status); err != nil {
			_ = eventRows.Close()
			return TrafficHistoryResult{}, err
		}
		result.Markers = append(result.Markers, TrafficMarker{ObservedAt: observedAt, Kind: "operation_event", Label: stage + " · " + status})
	}
	if err = eventRows.Err(); err != nil {
		_ = eventRows.Close()
		return TrafficHistoryResult{}, err
	}
	_ = eventRows.Close()
	incidentRows, err := s.db.QueryContext(ctx, `SELECT observed_at,kind,severity,status FROM incidents
 WHERE line_id=? AND observed_at>=? AND observed_at<? ORDER BY observed_at`, lineID,
		from.UTC().Format(time.RFC3339Nano), to.UTC().Format(time.RFC3339Nano))
	if err != nil {
		return TrafficHistoryResult{}, err
	}
	for incidentRows.Next() {
		var observedAt, kind, severity, status string
		if err = incidentRows.Scan(&observedAt, &kind, &severity, &status); err != nil {
			_ = incidentRows.Close()
			return TrafficHistoryResult{}, err
		}
		result.Markers = append(result.Markers, TrafficMarker{ObservedAt: observedAt, Kind: "incident", Label: kind + " · " + severity + " · " + status})
	}
	if err = incidentRows.Err(); err != nil {
		_ = incidentRows.Close()
		return TrafficHistoryResult{}, err
	}
	_ = incidentRows.Close()
	sort.Slice(result.Markers, func(i, j int) bool { return result.Markers[i].ObservedAt < result.Markers[j].ObservedAt })
	return result, nil
}

func transportMarkers(roles []TrafficSeries) []TrafficMarker {
	var points []TrafficPoint
	for _, preferredRole := range []string{"entry", "middle", "exit"} {
		for _, series := range roles {
			if series.Role == preferredRole && len(series.Points) > 0 {
				points = series.Points
				break
			}
		}
		if len(points) > 0 {
			break
		}
	}
	var result []TrafficMarker
	lastDeployment, lastProfile := "", ""
	for _, point := range points {
		if point.Deployment != "" && lastDeployment != "" && point.Deployment != lastDeployment {
			result = append(result, TrafficMarker{ObservedAt: point.ObservedAt, Kind: "deployment", Label: "Deployment " + point.Deployment})
		}
		if point.Profile != "" && lastProfile != "" && point.Profile != lastProfile {
			result = append(result, TrafficMarker{ObservedAt: point.ObservedAt, Kind: "profile", Label: "Profile " + point.Profile})
		}
		if point.Deployment != "" {
			lastDeployment = point.Deployment
		}
		if point.Profile != "" {
			lastProfile = point.Profile
		}
	}
	return result
}

func stringsLastSeparator(value string) int {
	for index := len(value) - 1; index >= 0; index-- {
		if value[index] == 0 {
			return index
		}
	}
	return -1
}

func (s *Store) PruneTraffic(ctx context.Context, nowAt time.Time) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	queries := []struct {
		query string
		args  []any
	}{
		{`DELETE FROM snapshots WHERE observed_at<? AND id NOT IN (SELECT snapshot_id FROM latest_snapshots)`, []any{nowAt.Add(-7 * 24 * time.Hour).UTC().Format(time.RFC3339Nano)}},
		{`DELETE FROM traffic_rollups WHERE resolution_s=60 AND bucket_at<?`, []any{nowAt.Add(-90 * 24 * time.Hour).UTC().Format(time.RFC3339)}},
		{`DELETE FROM traffic_rollups WHERE resolution_s=300 AND bucket_at<?`, []any{nowAt.Add(-365 * 24 * time.Hour).UTC().Format(time.RFC3339)}},
	}
	for _, item := range queries {
		if _, err = tx.ExecContext(ctx, item.query, item.args...); err != nil {
			return fmt.Errorf("prune traffic history: %w", err)
		}
	}
	return tx.Commit()
}
