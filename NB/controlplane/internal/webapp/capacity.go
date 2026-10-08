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
	a.operationMu.Lock()
	defer a.operationMu.Unlock()
	var link central.NetworkLink
	if !decode(w, r, &link) {
		return
	}
	link.ID = r.PathValue("id")
	if !safeID.MatchString(link.ID) {
		problem(w, http.StatusBadRequest, "invalid network link id")
		return
	}
	active, err := a.store.HasActiveOperationsUsingLink(r.Context(), link.FromDeviceID, link.FromRole,
		link.ToDeviceID, link.ToRole)
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	if active {
		problem(w, http.StatusConflict, "相关线路仍有排队中或执行中的任务，禁止修改物理链路")
		return
	}
	stored, err := a.store.UpsertNetworkLink(r.Context(), link)
	if err != nil {
		problem(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, stored)
}
