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
	var database *central.Store
	var err error
	if mysqlDSN := os.Getenv("NB_WEB_MYSQL_DSN"); mysqlDSN != "" {
		database, err = central.OpenMySQL(mysqlDSN, filepath.Join(stateDir, "nb-telemetry.db"))
	} else {
		database, err = central.Open(filepath.Join(stateDir, "nb-web.db"))
	}
	if err != nil {
		log.Fatal(err)
	}
	defer database.Close()
	secretsFile := env("NB_WEB_DEVICE_SECRETS_FILE", filepath.Join(stateDir, "device-secrets.json"))
	uploadDir := env("NB_NODE_SOURCE_UPLOAD_DIR", filepath.Join(filepath.Dir(stateDir), "source-uploads"))
	service := webapp.New(database, webapp.Config{AdminToken: os.Getenv("NB_WEB_ADMIN_TOKEN"),
		AgentToken: os.Getenv("NB_WEB_AGENT_TOKEN"), DeviceSecretsFile: secretsFile, NodeSourceUploadDir: uploadDir})
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		backfillComplete := false
		pruneDue := true
		ticker := time.NewTicker(time.Minute)
		defer ticker.Stop()
		for {
			if !backfillComplete {
				log.Printf("component=nb-web traffic_backfill=start")
				changed, backfillErr := database.BackfillTrafficHistory(ctx)
				if backfillErr != nil {
					if ctx.Err() == nil {
						log.Printf("component=nb-web traffic_backfill_error=%q", backfillErr)
					}
				} else {
					backfillComplete = true
					log.Printf("component=nb-web traffic_backfill=complete changed=%t", changed)
				}
			}
			if backfillComplete {
				if pruneDue {
					pruneCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
					nowAt := time.Now().UTC()
					pruneErr := database.PruneTraffic(pruneCtx, nowAt)
					if pruneErr == nil {
						pruneErr = database.PruneControlHistory(pruneCtx, nowAt)
					}
					cancel()
					if pruneErr != nil && ctx.Err() == nil {
						log.Printf("component=nb-web traffic_retention_error=%q", pruneErr)
					}
					pruneDue = false
				}
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				pruneDue = true
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
