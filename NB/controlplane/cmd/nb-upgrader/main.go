package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"nb-controlplane/internal/upgrader"
	"nb-controlplane/internal/worker"
)

var version = "dev"

func main() {
	root := os.Getenv("NB_CONTROLPLANE_ROOT")
	if root == "" {
		root = "/opt/nb-controlplane/repo"
	}
	state := os.Getenv("NB_UPGRADER_STATE_DIR")
	if state == "" {
		state = "/opt/nb-controlplane/data/upgrader"
	}
	registry := worker.Registry{WorkerID: "platform-upgrader", Root: root, Python: "python3", StateDir: state}
	runner := upgrader.NewRunner(root, "python3", state)
	client, err := worker.NewClient(registry, runner, worker.ClientConfig{
		BaseURL: os.Getenv("NB_WEB_BASE_URL"), Token: os.Getenv("NB_WEB_AGENT_TOKEN"), Version: version,
		PollEvery: time.Second, HeartbeatEvery: 5 * time.Second, OperationTimeout: 2 * time.Hour,
		MaintenanceEvery: time.Hour, SnapshotEvery: time.Hour, OperationConcurrency: 1,
		Capabilities: []worker.ExecutorCapability{{LineID: "__platform__", Operations: []string{"platform.upgrade", "platform.rollback"}}},
		PollLineIDs:  []string{"__platform__"}, DisableBackground: true,
	})
	if err != nil {
		log.Fatal(err)
	}
	if err = os.MkdirAll(state, 0700); err != nil {
		log.Fatal(err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	for ctx.Err() == nil {
		if err = client.Run(ctx); err != nil && ctx.Err() == nil {
			log.Printf("upgrader cycle failed: %v; retrying", err)
			select {
			case <-ctx.Done():
			case <-time.After(2 * time.Second):
			}
		}
	}
}
