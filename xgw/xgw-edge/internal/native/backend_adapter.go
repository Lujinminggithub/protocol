package native

import (
	"context"
	"errors"
	"net"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/coremodel"
)

// BackendAdapter is the single backend-facing adapter used by both the
// official HY2 compatibility frontend and the native xgw frontend.
//
// It intentionally exposes only canonical xgw flow/session entrypoints.
// Frontends may differ at the access layer, but must not maintain separate
// backend traffic-control semantics.
type BackendAdapter struct {
	cfg    config.ServerConfig
	bridge BridgeTransport
}

func NewBackendAdapter(cfg config.ServerConfig) (*BackendAdapter, error) {
	cfg.Normalize()
	adapter := &BackendAdapter{cfg: cfg}
	if cfg.ConnectType == connectTypeBridge {
		bridge, err := NewBridgeTransport(cfg)
		if err != nil {
			return nil, err
		}
		adapter.bridge = bridge
	}
	return adapter, nil
}

func (a *BackendAdapter) Close() error {
	if a == nil || a.bridge == nil {
		return nil
	}
	return a.bridge.Close()
}

func (a *BackendAdapter) OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, coremodel.FlowRequest, error) {
	flow := req.Flow
	if a == nil {
		return nil, flow, errors.New("backend adapter is nil")
	}
	if a.cfg.ConnectType == connectTypeBridge {
		if a.bridge == nil {
			return nil, flow, errors.New("bridge transport is not initialized")
		}
		conn, err := a.bridge.OpenTCP(ctx, req)
		return conn, flow, err
	}
	conn, err := net.DialTimeout("tcp", flow.Target, defaultDialTimeout)
	return conn, flow, err
}

func (a *BackendAdapter) OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, coremodel.FlowRequest, error) {
	flow := req.Flow
	if a == nil {
		return nil, flow, errors.New("backend adapter is nil")
	}
	if a.cfg.ConnectType == connectTypeBridge {
		if a.bridge == nil {
			return nil, flow, errors.New("bridge transport is not initialized")
		}
		conn, err := a.bridge.OpenUDP(ctx, req)
		return conn, flow, err
	}
	udpAddr, err := net.ResolveUDPAddr("udp", flow.Target)
	if err != nil {
		return nil, flow, err
	}
	conn, err := net.DialUDP("udp", nil, udpAddr)
	if err != nil {
		return nil, flow, err
	}
	return conn, flow, nil
}

func (a *BackendAdapter) ConnectType() string {
	if a == nil {
		return ""
	}
	return a.cfg.ConnectType
}

func (a *BackendAdapter) BridgeTransportName() string {
	if a == nil {
		return ""
	}
	return a.cfg.BridgeTransport
}
