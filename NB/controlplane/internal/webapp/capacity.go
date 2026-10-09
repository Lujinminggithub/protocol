package webapp

import (
	"context"
	"fmt"
	"net/http"

	"nb-controlplane/internal/central"
)

func (a *App) validateProductionTopology(ctx context.Context, spec central.LineSpec) error {
	for _, node := range spec.Nodes {
		device, err := a.store.Device(ctx, node.DeviceID)
		if err != nil {
			return fmt.Errorf("生产线路设备 %s 不可用", node.DeviceID)
		}
		if device.Environment != "production" {
			return fmt.Errorf("生产线路设备 %s 必须属于 production 环境", node.DeviceID)
		}
	}
	return nil
}

func (a *App) networkLinks(w http.ResponseWriter, r *http.Request) {
	items, err := a.store.NetworkLinks(r.Context())
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"links": items})
}

func (a *App) upsertNetworkLink(w http.ResponseWriter, r *http.Request) {
	problem(w, http.StatusGone, "物理链路配置已退役，请在线路中分别管理上行和下行能力")
}
