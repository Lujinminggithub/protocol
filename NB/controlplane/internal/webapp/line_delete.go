package webapp

import (
	"database/sql"
	"errors"
	"net/http"
	"strings"

	"nb-controlplane/internal/central"
)

func (a *App) deleteLine(w http.ResponseWriter, r *http.Request) {
	var request central.LineDeletionRequest
	if !decode(w, r, &request) {
		return
	}
	if !safeID.MatchString(request.RequestedBy) || strings.TrimSpace(request.Reason) == "" || len(request.Reason) > 300 {
		problem(w, http.StatusBadRequest, "发起人和删除原因不能为空，删除原因不能超过 300 个字符")
		return
	}
	if err := a.store.DeleteLine(r.Context(), r.PathValue("id"), request); err != nil {
		switch {
		case errors.Is(err, sql.ErrNoRows):
			problem(w, http.StatusNotFound, "线路不存在")
		case errors.Is(err, central.ErrLineDeletionValidation):
			problem(w, http.StatusBadRequest, err.Error())
		default:
			problem(w, http.StatusConflict, err.Error())
		}
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
