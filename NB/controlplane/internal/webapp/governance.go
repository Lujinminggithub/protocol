package webapp

import (
	"net/http"
)

func (a *App) productionLineGovernance(w http.ResponseWriter, r *http.Request) {
	findings, err := a.store.AuditProductionLines(r.Context())
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"findings": findings})
}
