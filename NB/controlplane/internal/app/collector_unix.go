//go:build !windows

package app

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type nodeCollection struct {
	path    string
	nodeID  string
	health  map[string]any
	metrics map[string]any
	tenants map[string]any
	routes  map[string]any
	err     error
}

func controlCommand(path, command string) (map[string]any, error) {
	connection, err := net.DialTimeout("unix", path, 2*time.Second)
	if err != nil {
		return nil, err
	}
	defer connection.Close()
	_ = connection.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err = io.WriteString(connection, command+"\n"); err != nil {
		return nil, err
	}
	data, err := io.ReadAll(io.LimitReader(connection, 1<<20))
	if err != nil {
		return nil, err
	}
	var result map[string]any
	if err = json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("%s invalid JSON: %w", command, err)
	}
	return result, nil
}

func collectionID(prefix string, parts ...string) string {
	digest := sha256.Sum256([]byte(strings.Join(parts, "\x00")))
	return prefix + "-" + hex.EncodeToString(digest[:12])
}
func nestedMap(parent map[string]any, key string) map[string]any {
	value, _ := parent[key].(map[string]any)
	return value
}
func number(parent map[string]any, key string) float64 {
	value, _ := parent[key].(float64)
	return value
}

func (a *App) collectNode(path, observed string) nodeCollection {
	nodeID := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	result := nodeCollection{path: path, nodeID: nodeID}
	var err error
	if result.health, err = controlCommand(path, "health"); err != nil {
		result.err = err
		return result
	}
	if result.metrics, err = controlCommand(path, "metrics"); err != nil {
		result.err = err
		return result
	}
	if result.tenants, err = controlCommand(path, "tenants"); err != nil {
		result.err = err
		return result
	}
	if result.routes, err = controlCommand(path, "routes"); err != nil {
		result.err = err
		return result
	}
	worker := fmt.Sprint(result.health["worker"])
	boot, _ := result.health["boot_id"].(string)
	if boot == "" {
		boot = "unknown-" + collectionID("boot", fmt.Sprint(result.health["release_id"]), path)
	}
	if values, ok := result.tenants["tenants"].([]any); ok {
		for _, value := range values {
			tenant, ok := value.(map[string]any)
			if !ok {
				continue
			}
			name, _ := tenant["name"].(string)
			if !safeID.MatchString(name) {
				continue
			}
			up := int64(number(tenant, "bytes_up"))
			down := int64(number(tenant, "bytes_down"))
			_, _ = a.store.RecordTenantCounters(context.Background(), nodeID, worker, boot, name, up, down, observed)
		}
	}
	payload := map[string]any{"node_id": nodeID, "line_id": a.cfg.LineID, "observed_at": observed, "health": result.health, "metrics": result.metrics, "tenants": result.tenants, "routes": result.routes}
	encoded, _ := json.Marshal(payload)
	key := collectionID("snapshot", nodeID, observed)
	_, result.err = a.store.RecordSnapshot(context.Background(), key, nodeID, observed, encoded)
	return result
}

func (a *App) recordCollectorIncident(kind, severity, nodeID, observed, message string, details any) {
	bucket := observed
	if parsed, err := time.Parse(time.RFC3339Nano, observed); err == nil {
		bucket = parsed.Truncate(5 * time.Minute).Format(time.RFC3339)
	}
	id := collectionID("incident", kind, nodeID, bucket)
	payload, _ := json.Marshal(map[string]any{"incident_id": id, "kind": kind, "severity": severity, "status": "firing", "node_id": nodeID, "line_id": a.cfg.LineID, "observed_at": observed, "message": message, "details": details})
	_, _ = a.store.RecordIncident(context.Background(), id, severity, "firing", payload)
}

func (a *App) evaluateNode(result nodeCollection, observed string) {
	if result.err != nil {
		a.recordCollectorIncident("collection-error", "critical", result.nodeID, observed, result.err.Error(), nil)
		return
	}
	if status, _ := result.health["status"].(string); status != "ok" {
		a.recordCollectorIncident("node-unhealthy", "critical", result.nodeID, observed, "node health is not ok", result.health)
	}
	fec := nestedMap(result.metrics, "fec")
	if number(fec, "active") != 0 {
		a.recordCollectorIncident("fec-active", "critical", result.nodeID, observed, "production FEC active must remain disabled", fec)
	}
	ages := nestedMap(result.metrics, "queue_age_max_us")
	maxAge := number(ages, "down")
	if value := number(ages, "up"); value > maxAge {
		maxAge = value
	}
	if value := number(ages, "q2t"); value > maxAge {
		maxAge = value
	}
	if maxAge > 750000 {
		a.recordCollectorIncident("queue-age", "warning", result.nodeID, observed, "queue age exceeds 750ms", ages)
	}
	link := nestedMap(result.metrics, "link")
	if loss := number(link, "effective_loss_max_pct"); loss > 5 {
		a.recordCollectorIncident("effective-loss", "critical", result.nodeID, observed, "effective loss exceeds 5 percent", link)
	}
	loop := nestedMap(result.metrics, "event_loop")
	if number(loop, "wake_late_max_us") > 20000 {
		a.recordCollectorIncident("event-loop-delay", "warning", result.nodeID, observed, "event loop wake delay exceeds 20ms", loop)
	}
}

func (a *App) collectCycle() {
	paths, _ := filepath.Glob(a.cfg.ControlGlob)
	sort.Strings(paths)
	observed := time.Now().UTC().Format(time.RFC3339Nano)
	results := make([]nodeCollection, 0, len(paths))
	identities := map[string]bool{}
	for _, path := range paths {
		result := a.collectNode(path, observed)
		results = append(results, result)
		a.evaluateNode(result, observed)
		if result.err == nil {
			identity := fmt.Sprint(result.health["release_id"]) + "/" + fmt.Sprint(result.health["line_profile"])
			identities[identity] = true
		}
	}
	if len(paths) == 0 {
		a.recordCollectorIncident("no-control-sockets", "critical", "collector", observed, "no NB control sockets matched", map[string]string{"glob": a.cfg.ControlGlob})
	}
	if len(identities) > 1 {
		keys := make([]string, 0, len(identities))
		for key := range identities {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		a.recordCollectorIncident("deployment-drift", "critical", "line", observed, "deployment or profile drift detected", keys)
	}
}

func (a *App) RunCollector(ctx context.Context) {
	if a.cfg.ControlGlob == "" {
		return
	}
	interval := a.cfg.CollectEvery
	if interval <= 0 {
		interval = 15 * time.Second
	}
	a.collectCycle()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			a.collectCycle()
		}
	}
}
