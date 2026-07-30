package central

import (
	"context"
	"path/filepath"
	"testing"
	"time"
)

func TestOpenAllowsConcurrentDatabaseWork(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	if got := store.db.Stats().MaxOpenConnections; got < 2 {
		t.Fatalf("MaxOpenConnections=%d, want at least 2", got)
	}
	held, err := store.db.Conn(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer held.Close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err = store.Ping(ctx); err != nil {
		t.Fatalf("ping blocked behind an occupied connection: %v", err)
	}
}

func TestOpenCreatesLatestSnapshotIndex(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	var count int
	err = store.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='snapshots_latest_node'`).Scan(&count)
	if err != nil {
		t.Fatal(err)
	}
	if count != 1 {
		t.Fatalf("snapshots_latest_node count=%d, want 1", count)
	}
}

func TestDashboardHandlesSnapshotHistory(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	stamp := now()
	_, err = store.db.Exec(`INSERT INTO lines
	 (id,name,status,entry_region,exit_region,provider,capacity_mbps,active_deployment,profile,secret_ref,created_at,updated_at)
	 VALUES('line-1','test','active','entry','exit','test',10,'deployment-1','profile-1','',?,?)`, stamp, stamp)
	if err != nil {
		t.Fatal(err)
	}
	_, err = store.db.Exec(`WITH RECURSIVE a(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM a WHERE i<250),
	 b(j) AS (VALUES(1) UNION ALL SELECT j+1 FROM b WHERE j<100)
	 INSERT INTO snapshots
	 (line_id,node_id,role,worker_id,observed_at,health,deployment,profile,sessions,throughput_mbps,
	 queue_age_p95_us,effective_loss_pct,fec_observe,fec_active,payload,received_at)
	 SELECT 'line-1','node-' || ((a.i*100+b.j)%6),'entry','worker-1',printf('%08d',a.i*100+b.j),
	 'ok','deployment-1','profile-1',1,1.0,1.0,0.0,1,0,'{}',? FROM a CROSS JOIN b`, stamp)
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	view, err := store.Dashboard(ctx)
	if err != nil {
		t.Fatalf("dashboard over snapshot history: %v", err)
	}
	if view.LinesTotal != 1 || len(view.Lines) != 1 || view.Lines[0].Workers != 6 {
		t.Fatalf("unexpected dashboard summary: %+v", view)
	}
}
