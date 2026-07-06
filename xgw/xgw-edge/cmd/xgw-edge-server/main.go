package main

import (
	"flag"
	"os"
	"strings"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/native"
	"github.com/local/xgw-edge/internal/officialbridge"
	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
)

func main() {
	var configPath string
	flag.StringVar(&configPath, "config", "", "server config file")
	flag.Parse()

	cfg, err := config.LoadServer(configPath)
	if err != nil {
		panic(err)
	}
	logger, err := newServerLogger(cfg.LogLevel)
	if err != nil {
		panic(err)
	}
	defer logger.Sync()

	var server interface {
		Serve() error
	}
	if cfg.FrontendMode == "hy2-official-bridge" {
		server, err = officialbridge.NewOfficialBridgeServer(cfg, logger.Sugar())
	} else {
		server, err = native.NewServer(cfg, logger.Sugar())
	}
	if err != nil {
		panic(err)
	}
	if err := server.Serve(); err != nil {
		logger.Sugar().Errorf("xgw-edge server exited: %v", err)
		os.Exit(1)
	}
}

func newServerLogger(levelText string) (*zap.Logger, error) {
	level := zapcore.InfoLevel
	switch strings.ToLower(strings.TrimSpace(levelText)) {
	case "debug":
		level = zapcore.DebugLevel
	case "info", "":
		level = zapcore.InfoLevel
	case "warn", "warning":
		level = zapcore.WarnLevel
	case "error":
		level = zapcore.ErrorLevel
	default:
		level = zapcore.InfoLevel
	}
	cfg := zap.NewProductionConfig()
	cfg.Level = zap.NewAtomicLevelAt(level)
	return cfg.Build()
}
