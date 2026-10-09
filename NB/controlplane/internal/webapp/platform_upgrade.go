package webapp

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"strings"

	"nb-controlplane/internal/central"
)

type platformUpgradePreview struct {
	ReleaseID     string                          `json:"release_id"`
	SeedLineID    string                          `json:"seed_line_id"`
	UnitID        string                          `json:"unit_id"`
	AffectedLines []string                        `json:"affected_lines"`
	Devices       []central.PlatformUpgradeDevice `json:"devices"`
	PreviewDigest string                          `json:"preview_digest"`
	Warning       string                          `json:"warning"`
}

func (a *App) latestPlatformRelease(r *http.Request) (map[string]any, error) {
	node, err := a.latestNodeRelease(r.Context())
	if err != nil {
		return nil, err
	}
	release, ok := node["platform_release"].(map[string]any)
	if !ok || strings.TrimSpace(stringValue(release["release_id"])) == "" {
		return nil, sql.ErrNoRows
	}
	return release, nil
}

func stringValue(value any) string {
	text, _ := value.(string)
	return text
}

func (a *App) platformReleaseStatus(w http.ResponseWriter, r *http.Request) {
	release, err := a.latestPlatformRelease(r)
	if errors.Is(err, sql.ErrNoRows) {
		writeJSON(w, http.StatusOK, map[string]any{"available": false})
		return
	}
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"available": true, "release": release})
}

func previewDigest(preview platformUpgradePreview) string {
	copy := preview
	copy.PreviewDigest = ""
	data, _ := json.Marshal(copy)
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}

func (a *App) buildPlatformUpgradePreview(r *http.Request, releaseID, seedLineID string) (platformUpgradePreview, error) {
	release, err := a.latestPlatformRelease(r)
	if err != nil {
		return platformUpgradePreview{}, err
	}
	if stringValue(release["release_id"]) != releaseID {
		return platformUpgradePreview{}, errors.New("平台候选已变化，请重新预览")
	}
	impact, err := a.store.PlatformUpgradeImpact(r.Context(), seedLineID)
	if err != nil {
		return platformUpgradePreview{}, err
	}
	preview := platformUpgradePreview{ReleaseID: releaseID, SeedLineID: seedLineID,
		UnitID: impact.UnitID, AffectedLines: impact.Lines, Devices: impact.Devices,
		Warning: "升级会终止活动连接并由客户端自动重连，单进程影响目标不超过 2 秒"}
	preview.PreviewDigest = previewDigest(preview)
	return preview, nil
}

func (a *App) platformUpgradePreview(w http.ResponseWriter, r *http.Request) {
	releaseID := strings.TrimSpace(r.URL.Query().Get("release_id"))
	lineID := strings.TrimSpace(r.URL.Query().Get("line_id"))
	if !safeID.MatchString(releaseID) || !safeID.MatchString(lineID) {
		problem(w, http.StatusBadRequest, "升级预览参数无效")
		return
	}
	preview, err := a.buildPlatformUpgradePreview(r, releaseID, lineID)
	if errors.Is(err, sql.ErrNoRows) {
		problem(w, http.StatusNotFound, "升级线路或候选不存在")
		return
	}
	if err != nil {
		problem(w, http.StatusConflict, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, preview)
}

func (a *App) createPlatformUpgrade(w http.ResponseWriter, r *http.Request) {
	key := strings.TrimSpace(r.Header.Get("Idempotency-Key"))
	if !safeID.MatchString(key) {
		problem(w, http.StatusBadRequest, "缺少有效的任务幂等键")
		return
	}
	var request struct {
		ReleaseID        string `json:"release_id"`
		LineID           string `json:"line_id"`
		PreviewDigest    string `json:"preview_digest"`
		RequestedBy      string `json:"requested_by"`
		ConfirmInterrupt bool   `json:"confirm_interrupt"`
	}
	if !decode(w, r, &request) {
		return
	}
	if !safeID.MatchString(request.ReleaseID) || !safeID.MatchString(request.LineID) ||
		!safeID.MatchString(request.RequestedBy) || len(request.PreviewDigest) != 64 || !request.ConfirmInterrupt {
		problem(w, http.StatusBadRequest, "平台升级请求无效或未确认连接中断")
		return
	}
	preview, err := a.buildPlatformUpgradePreview(r, request.ReleaseID, request.LineID)
	if err != nil {
		problem(w, http.StatusConflict, err.Error())
		return
	}
	if preview.PreviewDigest != request.PreviewDigest {
		problem(w, http.StatusConflict, "升级影响范围已变化，请重新预览")
		return
	}
	release, _ := a.latestPlatformRelease(r)
	if _, err = a.store.UpsertLine(r.Context(), central.Line{ID: "__platform__", Name: "Platform Upgrade",
		Status: "archived", Environment: "production", Provider: "controlplane"}); err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	requestBody, _ := json.Marshal(map[string]any{"release": release, "impact": preview,
		"confirm_interrupt": true})
	id, err := operationID()
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	operation, _, err := a.store.CreateOperation(r.Context(), central.Operation{ID: id, LineID: "__platform__",
		Kind: "platform.upgrade", RequestedBy: request.RequestedBy, IdempotencyKey: key, Request: requestBody})
	if errors.Is(err, central.ErrConflict) {
		problem(w, http.StatusConflict, "幂等键已用于其他任务")
		return
	}
	if err != nil {
		problem(w, http.StatusConflict, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, operation)
}

func (a *App) rollbackPlatformUpgrade(w http.ResponseWriter, r *http.Request) {
	key := strings.TrimSpace(r.Header.Get("Idempotency-Key"))
	upgradeID := strings.TrimSpace(r.PathValue("id"))
	if !safeID.MatchString(key) || !safeID.MatchString(upgradeID) {
		problem(w, http.StatusBadRequest, "平台回滚参数无效")
		return
	}
	var request struct {
		RequestedBy  string `json:"requested_by"`
		Confirmation string `json:"confirmation"`
	}
	if !decode(w, r, &request) {
		return
	}
	if !safeID.MatchString(request.RequestedBy) || request.Confirmation != upgradeID {
		problem(w, http.StatusBadRequest, "必须输入升级任务 ID 确认回滚")
		return
	}
	upgrade, err := a.store.Operation(r.Context(), upgradeID)
	if err != nil || upgrade.LineID != "__platform__" || upgrade.Kind != "platform.upgrade" || upgrade.Status != "succeeded" {
		problem(w, http.StatusConflict, "只有成功的平台升级任务可以回滚")
		return
	}
	var original struct {
		Release map[string]any `json:"release"`
	}
	if json.Unmarshal(upgrade.Request, &original) != nil || stringValue(original.Release["release_id"]) == "" {
		problem(w, http.StatusConflict, "升级任务缺少可回滚发布信息")
		return
	}
	body, _ := json.Marshal(map[string]any{"release_id": stringValue(original.Release["release_id"]),
		"upgrade_operation_id": upgradeID})
	id, err := operationID()
	if err != nil {
		problem(w, http.StatusInternalServerError, err.Error())
		return
	}
	operation, _, err := a.store.CreateOperation(r.Context(), central.Operation{ID: id, LineID: "__platform__",
		Kind: "platform.rollback", RequestedBy: request.RequestedBy, IdempotencyKey: key, Request: body})
	if errors.Is(err, central.ErrConflict) {
		problem(w, http.StatusConflict, "幂等键已用于其他任务")
		return
	}
	if err != nil {
		problem(w, http.StatusConflict, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, operation)
}
