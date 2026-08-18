package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"nb-controlplane/internal/worker"
)

var version = "dev"

func duration(name string, fallback time.Duration) time.Duration {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	seconds, err := strconv.Atoi(value)
	if err != nil || seconds < 1 {
		log.Fatalf("%s must be a positive number of seconds", name)
	}
	return time.Duration(seconds) * time.Second
}

func main() {
	registryPath := os.Getenv("NB_WEB_WORKER_REGISTRY")
	if registryPath == "" {
		log.Fatal("NB_WEB_WORKER_REGISTRY is required")
	}
	registry, err := worker.LoadRegistry(registryPath)
	if err != nil {
		log.Fatal(err)
	}
	runner := worker.NewRunner(registry)
	client, err := worker.NewClient(registry, runner, worker.ClientConfig{
		BaseURL:          os.Getenv("NB_WEB_BASE_URL"),
		Token:            os.Getenv("NB_WEB_AGENT_TOKEN"),
		Version:          version,
		PollEvery:        duration("NB_WEB_WORKER_POLL_SECONDS", 2*time.Second),
		HeartbeatEvery:   duration("NB_WEB_WORKER_HEARTBEAT_SECONDS", 10*time.Second),
		OperationTimeout: duration("NB_WEB_WORKER_OPERATION_TIMEOUT_SECONDS", 45*time.Minute),
		MaintenanceEvery: duration("NB_WEB_WORKER_MAINTENANCE_SECONDS", 5*time.Minute),
		SnapshotEvery:    duration("NB_WEB_WORKER_SNAPSHOT_SECONDS", 15*time.Second),
		SnapshotTimeout:  duration("NB_WEB_WORKER_SNAPSHOT_TIMEOUT_SECONDS", 30*time.Second),
	})
	if err != nil {
		log.Fatal(err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	for ctx.Err() == nil {
		log.Printf("nb-web-worker starting id=%s version=%s", registry.WorkerID, version)
		if err = client.Run(ctx); err != nil && ctx.Err() == nil {
			log.Printf("worker cycle failed: %v; retrying", err)
			select {
			case <-ctx.Done():
			case <-time.After(5 * time.Second):
			}
		}
	}
}
