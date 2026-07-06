package sim

import (
	"context"
	"errors"
	"net"
	"strings"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/control"
	"github.com/local/xgw-edge/internal/edge"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

type inboundDatagram struct {
	Target  string
	Payload []byte
}

type sessionInfo struct {
	SessionID       string
	RouteName       string
	LineID          string
	Address         string
	Congestion      string
	DefaultBudget   control.StreamBudgetSnapshot
	Scheduler       control.SchedulerSnapshot
	Keepalive       control.Keepalive
	LastRouteReason string
}

type simClient interface {
	Connect(ctx context.Context) (sessionInfo, error)
	StartAutoReconnect(ctx context.Context)
	CurrentSession() sessionInfo
	DialTCP(ctx context.Context, target string) (net.Conn, error)
	SendDatagramTo(ctx context.Context, target string, payload []byte) error
	ReceiveDatagramFrom(ctx context.Context) (inboundDatagram, error)
	RouteCommand(ctx context.Context, lineID string) bool
}

func newSimClient(cfg config.ClientConfig, logger *zap.SugaredLogger) simClient {
	if strings.EqualFold(cfg.FrontendMode, "xgw-native") || cfg.FrontendMode == "" {
		return &nativeSimClient{inner: native.NewManagedClient(cfg, logger)}
	}
	return &edgeSimClient{inner: edge.NewManagedClient(cfg, logger)}
}

type nativeSimClient struct {
	inner *native.ManagedClient
}

func (c *nativeSimClient) Connect(ctx context.Context) (sessionInfo, error) {
	session, err := c.inner.Connect(ctx)
	if err != nil {
		return sessionInfo{}, err
	}
	return sessionInfo{
		SessionID:     session.Resp.SessionID,
		RouteName:     session.Resp.SelectedRoute.Name,
		LineID:        session.Resp.SelectedRoute.LineID,
		Address:       session.Resp.SelectedRoute.Address,
		Congestion:    session.Resp.SelectedCongestion,
		DefaultBudget: session.Resp.DefaultBudget,
		Scheduler:     session.Resp.Scheduler,
	}, nil
}

func (c *nativeSimClient) StartAutoReconnect(ctx context.Context) {
	c.inner.StartAutoReconnect(ctx)
}

func (c *nativeSimClient) CurrentSession() sessionInfo {
	session := c.inner.CurrentSession()
	if session == nil {
		return sessionInfo{}
	}
	return sessionInfo{
		SessionID:       session.Resp.SessionID,
		RouteName:       session.Resp.SelectedRoute.Name,
		LineID:          session.Resp.SelectedRoute.LineID,
		Address:         session.Resp.SelectedRoute.Address,
		Congestion:      session.Resp.SelectedCongestion,
		DefaultBudget:   session.Resp.DefaultBudget,
		Scheduler:       session.Resp.Scheduler,
		Keepalive:       session.LastKeepalive(),
		LastRouteReason: session.LastRouteReason(),
	}
}

func (c *nativeSimClient) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	return c.inner.DialTCP(ctx, target)
}

func (c *nativeSimClient) SendDatagramTo(ctx context.Context, target string, payload []byte) error {
	return c.inner.SendDatagramTo(ctx, target, payload)
}

func (c *nativeSimClient) ReceiveDatagramFrom(ctx context.Context) (inboundDatagram, error) {
	dg, err := c.inner.ReceiveDatagramFrom(ctx)
	if err != nil {
		return inboundDatagram{}, err
	}
	return inboundDatagram{Target: dg.Target, Payload: dg.Payload}, nil
}

func (c *nativeSimClient) RouteCommand(ctx context.Context, lineID string) bool {
	session := c.inner.CurrentSession()
	if session == nil {
		return false
	}
	update, err := session.FetchRouteUpdate(ctx)
	if err != nil || update == nil {
		return false
	}
	return update.SelectedRoute.LineID == lineID || update.SelectedRoute.Name == lineID
}

type edgeSimClient struct {
	inner *edge.ManagedClient
}

func (c *edgeSimClient) Connect(ctx context.Context) (sessionInfo, error) {
	session, err := c.inner.Connect(ctx)
	if err != nil {
		return sessionInfo{}, err
	}
	route := session.CurrentRoute()
	return sessionInfo{
		SessionID:     session.Resp.SessionID,
		RouteName:     route.Name,
		LineID:        route.LineID,
		Address:       route.Address,
		Congestion:    session.Resp.SelectedCongestion,
		DefaultBudget: session.Resp.DefaultBudget,
		Scheduler:     session.Resp.Scheduler,
	}, nil
}

func (c *edgeSimClient) StartAutoReconnect(ctx context.Context) {
	c.inner.StartAutoReconnect(ctx)
}

func (c *edgeSimClient) CurrentSession() sessionInfo {
	session := c.inner.CurrentSession()
	if session == nil {
		return sessionInfo{}
	}
	route := session.CurrentRoute()
	return sessionInfo{
		SessionID:     session.Resp.SessionID,
		RouteName:     route.Name,
		LineID:        route.LineID,
		Address:       route.Address,
		Congestion:    session.Resp.SelectedCongestion,
		DefaultBudget: session.Resp.DefaultBudget,
		Scheduler:     session.Resp.Scheduler,
	}
}

func (c *edgeSimClient) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	return c.inner.DialTCP(ctx, target)
}

func (c *edgeSimClient) SendDatagramTo(ctx context.Context, target string, payload []byte) error {
	if target != "" {
		return errors.New("hy2-official-bridge sim datagram target mode is not supported yet")
	}
	return c.inner.SendDatagram(ctx, payload)
}

func (c *edgeSimClient) ReceiveDatagramFrom(ctx context.Context) (inboundDatagram, error) {
	msg, err := c.inner.ReceiveDatagram(ctx)
	if err != nil {
		return inboundDatagram{}, err
	}
	return inboundDatagram{Payload: msg}, nil
}

func (c *edgeSimClient) RouteCommand(ctx context.Context, lineID string) bool {
	_ = ctx
	cur := c.CurrentSession()
	return cur.LineID == lineID || cur.RouteName == lineID
}
