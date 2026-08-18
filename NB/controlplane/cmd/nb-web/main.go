package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"nb-controlplane/internal/central"
	"nb-controlplane/internal/webapp"
)

func env(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func main() {
	stateDir := env("NB_WEB_STATE_DIR", "./build/nb-web")
	if err := os.MkdirAll(stateDir, 0700); err != nil {
		log.Fatal(err)
	}
	database, err := central.Open(filepath.Join(stateDir, "nb-web.db"))
	if err != nil {
		log.Fatal(err)
	}
	defer database.Close()
	secretsFile := env("NB_WEB_DEVICE_SECRETS_FILE", filepath.Join(stateDir, "device-secrets.json"))
	service := webapp.New(database, webapp.Config{AdminToken: os.Getenv("NB_WEB_ADMIN_TOKEN"),
		AgentToken: os.Getenv("NB_WEB_AGENT_TOKEN"), DeviceSecretsFile: secretsFile})
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		if pruneErr := database.PruneTraffic(ctx, time.Now().UTC()); pruneErr != nil && ctx.Err() == nil {
			log.Printf("component=nb-web traffic_retention_error=%q", pruneErr)
		}
		ticker := time.NewTicker(time.Hour)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case stamp := <-ticker.C:
				if pruneErr := database.PruneTraffic(ctx, stamp.UTC()); pruneErr != nil && ctx.Err() == nil {
					log.Printf("component=nb-web traffic_retention_error=%q", pruneErr)
				}
			}
		}
	}()
	server := &http.Server{Addr: env("NB_WEB_LISTEN", "127.0.0.1:9091"), Handler: service.Handler(),
		ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 15 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second}
	go func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdown)
	}()
	log.Printf("nb-web listening=%s", server.Addr)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
