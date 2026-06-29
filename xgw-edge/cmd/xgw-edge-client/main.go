package main

import (
	"context"
	"flag"
	"os"
	"os/signal"
	"syscall"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/edge"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

func main() {
	var configPath string
	flag.StringVar(&configPath, "config", "", "client config file")
	flag.Parse()

	cfg, err := config.LoadClient(configPath)
	if err != nil {
		panic(err)
	}
	logger, err := zap.NewProduction()
	if err != nil {
		panic(err)
	}
	defer logger.Sync()

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	if cfg.FrontendMode == "xgw-native" || cfg.FrontendMode == "" {
		client := native.NewManagedClient(cfg, logger.Sugar())
		session, err := client.Connect(ctx)
		if err != nil {
			logger.Sugar().Errorf("xgw-native client connect failed: %v", err)
			os.Exit(1)
		}
		logger.Sugar().Infof("xgw-native client connected session=%s route=%s", session.Resp.SessionID, session.Resp.SelectedRoute.Name)
		client.StartAutoReconnect(ctx)
		if err := client.StartLocalProxy(ctx, cfg.LocalSOCKS5Listen, cfg.LocalHTTPListen); err != nil {
			logger.Sugar().Errorf("xgw-native local proxy failed: %v", err)
			os.Exit(1)
		}
		switch cfg.LocalMode {
		case "tun":
			go func() {
				if err := client.LocalTUNProxy(ctx, cfg.LocalTUNName, cfg.LocalTUNAddress); err != nil {
					logger.Sugar().Errorf("xgw-native tun proxy failed: %v", err)
					cancel()
				}
			}()
		default:
			go func() {
				if err := client.LocalUDPProxy(ctx, cfg.LocalUDPListen, cfg.FixedBackendUDP); err != nil {
					logger.Sugar().Errorf("xgw-native udp proxy failed: %v", err)
					cancel()
				}
			}()
		}
		<-ctx.Done()
		return
	}

	client := edge.NewManagedClient(cfg, logger.Sugar())
	session, err := client.Connect(ctx)
	if err != nil {
		logger.Sugar().Errorf("xgw-edge client connect failed: %v", err)
		os.Exit(1)
	}
	logger.Sugar().Infof("xgw-edge client connected session=%s route=%s", session.Resp.SessionID, session.Resp.SelectedRoute.Name)
	client.StartAutoReconnect(ctx)
	if err := client.StartLocalProxy(ctx, cfg.LocalSOCKS5Listen, cfg.LocalHTTPListen); err != nil {
		logger.Sugar().Errorf("xgw-edge local proxy failed: %v", err)
		os.Exit(1)
	}

	switch cfg.LocalMode {
	case "tun":
		go func() {
			if err := client.LocalTUNProxy(ctx, cfg.LocalTUNName, cfg.LocalTUNAddress); err != nil {
				logger.Sugar().Errorf("xgw-edge tun proxy failed: %v", err)
				cancel()
			}
		}()
	default:
		go func() {
			if err := client.LocalUDPProxy(ctx, cfg.LocalUDPListen); err != nil {
				logger.Sugar().Errorf("xgw-edge udp proxy failed: %v", err)
				cancel()
			}
		}()
	}
	<-ctx.Done()
}
