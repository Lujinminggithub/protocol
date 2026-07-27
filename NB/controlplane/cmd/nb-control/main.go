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

	"nb-controlplane/internal/app"
	"nb-controlplane/internal/store"
)

func env(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func main() {
	stateDir := env("NB_CONTROL_STATE_DIR", "/var/lib/nb-control")
	if err := os.MkdirAll(stateDir, 0700); err != nil {
		log.Fatal(err)
	}
	db, err := store.Open(filepath.Join(stateDir, "control.db"))
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()
	cfg := app.Config{Listen: env("NB_CONTROL_LISTEN", "127.0.0.1:9080"), APIToken: os.Getenv("NB_CONTROL_API_TOKEN"),
		WebBaseURL: os.Getenv("NB_WEB_BASE_URL"), WebToken: os.Getenv("NB_WEB_TOKEN"), WebSigningKey: os.Getenv("NB_WEB_SIGNING_KEY"),
		UsersFile: env("NB_CONTROL_USERS_FILE", "/etc/NB/users.conf"), TenantsFile: env("NB_CONTROL_TENANTS_FILE", "/etc/NB/tenants.conf"), ReloadPIDFile: os.Getenv("NB_CONTROL_RELOAD_PID_FILE")}
	cfg.ControlGlob = env("NB_CONTROL_SOCKET_GLOB", "/run/nb-*.ctl")
	cfg.LineID = env("NB_LINE_ID", "unversioned")
	cfg.CollectEvery = 15 * time.Second
	service := app.New(db, cfg)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err = service.Bootstrap(ctx); err != nil {
		log.Fatal(err)
	}
	go service.RunDispatcher(ctx)
	go service.RunCollector(ctx)
	server := &http.Server{Addr: cfg.Listen, Handler: service.Handler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 15 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()
	log.Printf("nb-control listening=%s", cfg.Listen)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
