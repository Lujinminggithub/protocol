package webapp

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"nb-controlplane/internal/central"
)

func trafficTime(value string, fallback time.Time) (time.Time, error) {
	if value == "" {
		return fallback, nil
	}
	parsed, err := time.Parse(time.RFC3339, value)
	if err != nil {
		return time.Time{}, errors.New("时间参数必须使用 RFC3339 格式")
	}
	return parsed, nil
}

func trafficResolution(value string, span time.Duration) (int, error) {
	if value == "" || value == "auto" {
		return central.TrafficResolution(span), nil
	}
	resolution, err := strconv.Atoi(value)
	if err != nil || (resolution != 15 && resolution != 60 && resolution != 300) {
		return 0, errors.New("resolution 仅支持 auto、15、60 或 300")
	}
	return resolution, nil
}

func (a *App) trafficHistory(w http.ResponseWriter, r *http.Request) {
	nowAt := time.Now().UTC()
	to, err := trafficTime(r.URL.Query().Get("to"), nowAt)
	if err != nil {
		problem(w, http.StatusBadRequest, err.Error())
		return
	}
	from, err := trafficTime(r.URL.Query().Get("from"), to.Add(-2*time.Hour))
	if err != nil {
		problem(w, http.StatusBadRequest, err.Error())
		return
	}
	resolution, err := trafficResolution(r.URL.Query().Get("resolution"), to.Sub(from))
	if err != nil {
		problem(w, http.StatusBadRequest, err.Error())
		return
	}
	result, err := a.store.TrafficHistory(r.Context(), r.PathValue("id"), from, to, resolution)
	if err != nil {
		switch {
		case errors.Is(err, sql.ErrNoRows):
			problem(w, http.StatusNotFound, "线路不存在")
		case err.Error() == "invalid traffic history range":
			problem(w, http.StatusBadRequest, "时间范围无效，且最多查询 365 天")
		default:
			problem(w, http.StatusInternalServerError, err.Error())
		}
		return
	}
	writeJSON(w, http.StatusOK, result)
}

func (a *App) trafficStream(w http.ResponseWriter, r *http.Request) {
	lineID := r.PathValue("id")
	if _, err := a.store.Line(r.Context(), lineID); err != nil {
		problem(w, http.StatusNotFound, "线路不存在")
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		problem(w, http.StatusInternalServerError, "当前 HTTP 服务不支持实时流")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("X-Accel-Buffering", "no")
	_ = http.NewResponseController(w).SetWriteDeadline(time.Time{})
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprint(w, ": connected\n\n")
	flusher.Flush()

	snapshots, unsubscribe := a.snapshotHub.subscribe(lineID)
	defer unsubscribe()
	keepAlive := time.NewTicker(15 * time.Second)
	defer keepAlive.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case snapshot := <-snapshots:
			payload, err := json.Marshal(snapshot)
			if err != nil {
				continue
			}
			if _, err = fmt.Fprintf(w, "event: snapshot\ndata: %s\n\n", payload); err != nil {
				return
			}
			flusher.Flush()
		case <-keepAlive.C:
			if _, err := fmt.Fprint(w, ": keepalive\n\n"); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}
