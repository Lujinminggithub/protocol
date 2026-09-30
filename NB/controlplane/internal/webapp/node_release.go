package webapp

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"nb-controlplane/internal/central"
)

const maxNodeSourceArchive = 1024 << 20

var fullGitCommit = regexp.MustCompile(`^[0-9a-f]{40}$`)

func (a *App) latestNodeRelease(ctx context.Context) (map[string]any, error) {
	operation, err := a.store.LatestSuccessfulOperation(ctx, "__node_release__", "node.release.build")
	if err != nil {
		return nil, err
	}
	var result struct {
		NodeRelease map[string]any `json:"node_release"`
	}
	if json.Unmarshal(operation.Result, &result) != nil || len(result.NodeRelease) == 0 {
		return nil, errors.New("Node Release 构建结果无效")
	}
	result.NodeRelease["operation_id"] = operation.ID
	result.NodeRelease["created_at"] = operation.UpdatedAt
	return result.NodeRelease, nil
}

func (a *App) nodeReleaseStatus(w http.ResponseWriter, r *http.Request) {
	release, err := a.latestNodeRelease(r.Context())
	if errors.Is(err, sql.ErrNoRows) {
		writeJSON(w, 200, map[string]any{"available": false})
		return
	}
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"available": true, "release": release})
}

func (a *App) uploadNodeSource(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxNodeSourceArchive)
	if err := r.ParseMultipartForm(16 << 20); err != nil {
		problem(w, http.StatusBadRequest, "Node 源码包无效或超过 1 GiB")
		return
	}
	commit := strings.ToLower(strings.TrimSpace(r.FormValue("git_commit")))
	if !fullGitCommit.MatchString(commit) {
		problem(w, http.StatusBadRequest, "必须填写完整的 40 位 Git commit")
		return
	}
	file, header, err := r.FormFile("archive")
	if err != nil {
		problem(w, http.StatusBadRequest, "必须上传包含 .git 的 tar.gz 源码包")
		return
	}
	defer file.Close()
	name := strings.ToLower(header.Filename)
	if !strings.HasSuffix(name, ".tar.gz") && !strings.HasSuffix(name, ".tgz") {
		problem(w, http.StatusBadRequest, "源码包必须是 tar.gz 或 tgz")
		return
	}
	uploadID, err := operationID()
	if err != nil {
		problem(w, 500, err.Error())
		return
	}
	uploadID = "source-" + strings.TrimPrefix(uploadID, "op-")
	directory := a.cfg.NodeSourceUploadDir
	if directory == "" {
		directory = filepath.Join(filepath.Dir(filepath.Dir(a.cfg.DeviceSecretsFile)), "source-uploads")
	}
	if err = os.MkdirAll(directory, 0700); err != nil {
		problem(w, 500, "无法创建源码上传目录")
		return
	}
	temporary := filepath.Join(directory, uploadID+".part")
	target := filepath.Join(directory, uploadID+".tar.gz")
	output, err := os.OpenFile(temporary, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		problem(w, 500, "无法保存源码上传")
		return
	}
	hash := sha256.New()
	written, copyErr := io.Copy(io.MultiWriter(output, hash), file)
	closeErr := output.Close()
	if copyErr != nil || closeErr != nil || written == 0 {
		_ = os.Remove(temporary)
		problem(w, 400, "源码上传不完整")
		return
	}
	if err = os.Rename(temporary, target); err != nil {
		_ = os.Remove(temporary)
		problem(w, 500, "无法发布源码上传")
		return
	}
	headerBytes := make([]byte, 2)
	headerFile, headerErr := os.Open(target)
	if headerErr == nil {
		_, headerErr = io.ReadFull(headerFile, headerBytes)
		_ = headerFile.Close()
	}
	if headerErr != nil || headerBytes[0] != 0x1f || headerBytes[1] != 0x8b {
		_ = os.Remove(target)
		problem(w, 400, "源码包不是有效的 gzip 文件")
		return
	}
	lineID := "__node_release__"
	if _, err = a.store.UpsertLine(r.Context(), central.Line{ID: lineID, Name: "Node Release", Status: "archived",
		Environment: "production", Provider: "controlplane"}); err != nil {
		_ = os.Remove(target)
		problem(w, 500, err.Error())
		return
	}
	requestedBy := strings.TrimSpace(r.FormValue("requested_by"))
	if !safeID.MatchString(requestedBy) {
		requestedBy = "operator"
	}
	request, _ := json.Marshal(map[string]string{"upload_id": uploadID, "archive": target, "archive_sha256": hex.EncodeToString(hash.Sum(nil)), "git_commit": commit})
	opID, err := operationID()
	if err != nil {
		_ = os.Remove(target)
		problem(w, 500, err.Error())
		return
	}
	operation := central.Operation{ID: opID, LineID: lineID, Kind: "node.release.build", RequestedBy: requestedBy,
		IdempotencyKey: "node-release-" + uploadID, Request: request}
	created, _, err := a.store.CreateOperation(r.Context(), operation)
	if err != nil {
		_ = os.Remove(target)
		if errors.Is(err, central.ErrConflict) {
			problem(w, http.StatusConflict, "源码上传任务冲突")
		} else {
			problem(w, http.StatusConflict, err.Error())
		}
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"operation": created, "upload_id": uploadID,
		"archive_sha256": hex.EncodeToString(hash.Sum(nil)), "received_at": time.Now().UTC().Format(time.RFC3339Nano)})
}
