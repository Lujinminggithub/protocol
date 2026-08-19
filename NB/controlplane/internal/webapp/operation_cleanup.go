package webapp

import (
	"database/sql"
	"errors"
	"net/http"

	"nb-controlplane/internal/central"
)

func (a *App) deleteOperation(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !safeID.MatchString(id) {
		problem(w, http.StatusBadRequest, "任务 ID 无效")
		return
	}
	a.deleteOperationIDs(w, r, []string{id})
}

func (a *App) deleteOperations(w http.ResponseWriter, r *http.Request) {
	var request struct {
		IDs []string `json:"ids"`
	}
	if !decode(w, r, &request) {
		return
	}
	if len(request.IDs) == 0 || len(request.IDs) > 500 {
		problem(w, http.StatusBadRequest, "每次请选择 1 到 500 个任务")
		return
	}
	for _, id := range request.IDs {
		if !safeID.MatchString(id) {
			problem(w, http.StatusBadRequest, "任务 ID 无效")
			return
		}
	}
	a.deleteOperationIDs(w, r, request.IDs)
}

func (a *App) deleteOperationIDs(w http.ResponseWriter, r *http.Request, ids []string) {
	deleted, err := a.store.DeleteOperations(r.Context(), ids)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		problem(w, http.StatusNotFound, "任务不存在或已被清理")
	case errors.Is(err, central.ErrOperationActive):
		problem(w, http.StatusConflict, err.Error())
	case err != nil:
		problem(w, http.StatusConflict, err.Error())
	default:
		writeJSON(w, http.StatusOK, map[string]int64{"deleted": deleted})
	}
}
