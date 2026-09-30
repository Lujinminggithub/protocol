package webapp

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
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
	"strconv"
	"strings"
	"time"

	"nb-controlplane/internal/central"
)

const maxNodeSourceArchive = (1024 << 20) + (1 << 20) // 1 GiB file plus multipart framing.
const maxNodeSourceChunk = 8 << 20

var fullGitCommit = regexp.MustCompile(`^[0-9a-f]{40}$`)

type nodeSourceUploadMetadata struct {
	UploadID  string `json:"upload_id"`
	UploadKey string `json:"upload_key"`
	Filename  string `json:"filename"`
	Size      int64  `json:"size"`
	CreatedAt string `json:"created_at"`
}

func (a *App) nodeSourceUploadDirectory() string {
	if a.cfg.NodeSourceUploadDir != "" {
		return a.cfg.NodeSourceUploadDir
	}
	return filepath.Join(filepath.Dir(filepath.Dir(a.cfg.DeviceSecretsFile)), "source-uploads")
}

func nodeSourceUploadPaths(directory, uploadID string) (string, string) {
	return filepath.Join(directory, uploadID+".part"), filepath.Join(directory, uploadID+".json")
}

func writeNodeSourceUploadMetadata(path string, metadata nodeSourceUploadMetadata) error {
	data, err := json.Marshal(metadata)
	if err != nil {
		return err
	}
	temporary := path + ".new"
	if err = os.WriteFile(temporary, data, 0600); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

func readNodeSourceUploadMetadata(path string) (nodeSourceUploadMetadata, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nodeSourceUploadMetadata{}, err
	}
	var metadata nodeSourceUploadMetadata
	if json.Unmarshal(data, &metadata) != nil || !safeID.MatchString(metadata.UploadID) || metadata.Size < 1 || metadata.Size > 1024<<20 {
		return nodeSourceUploadMetadata{}, errors.New("源码分块上传元数据无效")
	}
	return metadata, nil
}

func validateNodeSourceArchive(path, filename string) (string, error) {
	name := strings.ToLower(filename)
	allowed := strings.HasSuffix(name, ".zip") || strings.HasSuffix(name, ".tar") ||
		strings.HasSuffix(name, ".tar.gz") || strings.HasSuffix(name, ".tgz")
	if !allowed {
		return "", errors.New("源码包必须是 zip、tar、tar.gz 或 tgz")
	}
	if archive, err := zip.OpenReader(path); err == nil {
		defer archive.Close()
		if len(archive.File) == 0 {
			return "", errors.New("ZIP 源码包为空")
		}
		return "zip", nil
	}
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	reader := io.Reader(file)
	if strings.HasSuffix(name, ".tar.gz") || strings.HasSuffix(name, ".tgz") {
		compressed, gzipErr := gzip.NewReader(file)
		if gzipErr != nil {
			return "", errors.New("TAR.GZ 源码包无效")
		}
		defer compressed.Close()
		reader = compressed
	}
	if _, err = tar.NewReader(reader).Next(); err != nil {
		return "", errors.New("TAR 源码包无效或为空")
	}
	if strings.HasSuffix(name, ".tar") {
		return "tar", nil
	}
	return "tar.gz", nil
}

func hashFileSHA256(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hash := sha256.New()
	if _, err = io.Copy(hash, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func (a *App) createNodeReleaseOperation(r *http.Request, uploadID, archivePath, archiveSHA256, commit, requestedBy string) (central.Operation, error) {
	lineID := "__node_release__"
	if _, err := a.store.UpsertLine(r.Context(), central.Line{ID: lineID, Name: "Node Release", Status: "archived",
		Environment: "production", Provider: "controlplane"}); err != nil {
		return central.Operation{}, err
	}
	requestedBy = strings.TrimSpace(requestedBy)
	if !safeID.MatchString(requestedBy) {
		requestedBy = "operator"
	}
	request, _ := json.Marshal(map[string]string{"upload_id": uploadID, "archive": archivePath,
		"archive_sha256": archiveSHA256, "git_commit": commit})
	opID, err := operationID()
	if err != nil {
		return central.Operation{}, err
	}
	operation := central.Operation{ID: opID, LineID: lineID, Kind: "node.release.build", RequestedBy: requestedBy,
		IdempotencyKey: "node-release-" + uploadID, Request: request}
	created, _, err := a.store.CreateOperation(r.Context(), operation)
	if errors.Is(err, central.ErrConflict) {
		return central.Operation{}, errors.New("源码上传任务冲突")
	}
	return created, err
}

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

func (a *App) initNodeSourceUpload(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Filename  string `json:"filename"`
		Size      int64  `json:"size"`
		UploadKey string `json:"upload_key"`
	}
	if !decode(w, r, &request) {
		return
	}
	request.Filename = filepath.Base(strings.TrimSpace(request.Filename))
	request.UploadKey = strings.TrimSpace(request.UploadKey)
	if request.Filename == "." || request.Filename == "" || request.Size < 1 || request.Size > 1024<<20 || len(request.UploadKey) < 8 || len(request.UploadKey) > 256 {
		problem(w, 400, "源码分块上传参数无效")
		return
	}
	keyDigest := sha256.Sum256([]byte(request.UploadKey))
	uploadID := "source-" + hex.EncodeToString(keyDigest[:12])
	directory := a.nodeSourceUploadDirectory()
	if err := os.MkdirAll(directory, 0700); err != nil {
		problem(w, 500, "无法创建源码上传目录")
		return
	}
	partPath, metadataPath := nodeSourceUploadPaths(directory, uploadID)
	a.nodeUploadMu.Lock()
	defer a.nodeUploadMu.Unlock()
	metadata, err := readNodeSourceUploadMetadata(metadataPath)
	if err == nil {
		if metadata.Filename != request.Filename || metadata.Size != request.Size || metadata.UploadKey != request.UploadKey {
			problem(w, 409, "相同上传标识对应的文件信息不一致")
			return
		}
	} else if errors.Is(err, os.ErrNotExist) {
		metadata = nodeSourceUploadMetadata{UploadID: uploadID, UploadKey: request.UploadKey, Filename: request.Filename,
			Size: request.Size, CreatedAt: time.Now().UTC().Format(time.RFC3339Nano)}
		if err = writeNodeSourceUploadMetadata(metadataPath, metadata); err != nil {
			problem(w, 500, "无法保存源码上传状态")
			return
		}
	} else {
		problem(w, 500, err.Error())
		return
	}
	received := int64(0)
	if info, statErr := os.Stat(partPath); statErr == nil {
		received = info.Size()
		if received > metadata.Size {
			problem(w, 500, "源码上传临时文件超过声明大小")
			return
		}
	} else if !errors.Is(statErr, os.ErrNotExist) {
		problem(w, 500, "无法读取源码上传状态")
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{"upload_id": uploadID, "received": received, "size": metadata.Size})
}

func (a *App) uploadNodeSourceChunk(w http.ResponseWriter, r *http.Request) {
	uploadID := r.PathValue("id")
	offset, err := strconv.ParseInt(r.Header.Get("X-Upload-Offset"), 10, 64)
	if !safeID.MatchString(uploadID) || !strings.HasPrefix(uploadID, "source-") || err != nil || offset < 0 {
		problem(w, 400, "源码分块上传标识或偏移无效")
		return
	}
	directory := a.nodeSourceUploadDirectory()
	partPath, metadataPath := nodeSourceUploadPaths(directory, uploadID)
	a.nodeUploadMu.Lock()
	defer a.nodeUploadMu.Unlock()
	metadata, err := readNodeSourceUploadMetadata(metadataPath)
	if err != nil {
		problem(w, 404, "源码分块上传不存在或已过期")
		return
	}
	received := int64(0)
	if info, statErr := os.Stat(partPath); statErr == nil {
		received = info.Size()
	} else if !errors.Is(statErr, os.ErrNotExist) {
		problem(w, 500, "无法读取源码上传临时文件")
		return
	}
	if offset != received {
		writeJSON(w, http.StatusConflict, map[string]any{"error": "上传偏移不一致", "status": 409, "received": received})
		return
	}
	remaining := metadata.Size - received
	if remaining <= 0 {
		writeJSON(w, 200, map[string]any{"received": received, "size": metadata.Size})
		return
	}
	limit := int64(maxNodeSourceChunk)
	if remaining < limit {
		limit = remaining
	}
	body := http.MaxBytesReader(w, r.Body, limit+1)
	output, err := os.OpenFile(partPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		problem(w, 500, "无法写入源码上传分块")
		return
	}
	written, copyErr := io.Copy(output, body)
	closeErr := output.Close()
	if copyErr != nil || closeErr != nil || written < 1 || written > limit {
		_ = os.Truncate(partPath, received)
		problem(w, 400, "源码上传分块不完整或超过限制")
		return
	}
	received += written
	writeJSON(w, 200, map[string]any{"received": received, "size": metadata.Size})
}

func (a *App) completeNodeSourceUpload(w http.ResponseWriter, r *http.Request) {
	uploadID := r.PathValue("id")
	var request struct {
		GitCommit   string `json:"git_commit"`
		RequestedBy string `json:"requested_by"`
	}
	if !safeID.MatchString(uploadID) || !strings.HasPrefix(uploadID, "source-") {
		problem(w, 400, "源码分块上传标识无效")
		return
	}
	if !decode(w, r, &request) {
		return
	}
	request.GitCommit = strings.ToLower(strings.TrimSpace(request.GitCommit))
	if !fullGitCommit.MatchString(request.GitCommit) {
		problem(w, 400, "必须填写完整的 40 位 Git commit")
		return
	}
	directory := a.nodeSourceUploadDirectory()
	partPath, metadataPath := nodeSourceUploadPaths(directory, uploadID)
	a.nodeUploadMu.Lock()
	defer a.nodeUploadMu.Unlock()
	metadata, err := readNodeSourceUploadMetadata(metadataPath)
	if err != nil {
		problem(w, 404, "源码分块上传不存在或已过期")
		return
	}
	info, err := os.Stat(partPath)
	if err != nil || info.Size() != metadata.Size {
		received := int64(0)
		if err == nil {
			received = info.Size()
		}
		writeJSON(w, http.StatusConflict, map[string]any{"error": "源码上传尚未完成", "status": 409, "received": received, "size": metadata.Size})
		return
	}
	target := filepath.Join(directory, uploadID+".archive")
	if format, formatErr := validateNodeSourceArchive(partPath, metadata.Filename); formatErr != nil {
		problem(w, 400, formatErr.Error())
		return
	} else {
		_ = format
	}
	hash, err := hashFileSHA256(partPath)
	if err != nil {
		problem(w, 500, "无法计算源码包 SHA256")
		return
	}
	if err = os.Rename(partPath, target); err != nil {
		problem(w, 500, "无法发布源码上传")
		return
	}
	created, err := a.createNodeReleaseOperation(r, uploadID, target, hash, request.GitCommit, request.RequestedBy)
	if err != nil {
		_ = os.Rename(target, partPath)
		problem(w, http.StatusConflict, err.Error())
		return
	}
	_ = os.Remove(metadataPath)
	writeJSON(w, http.StatusAccepted, map[string]any{"operation": created, "upload_id": uploadID, "archive_sha256": hash,
		"received_at": time.Now().UTC().Format(time.RFC3339Nano)})
}

func (a *App) uploadNodeSource(w http.ResponseWriter, r *http.Request) {
	if strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		a.initNodeSourceUpload(w, r)
		return
	}
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
		problem(w, http.StatusBadRequest, "必须上传包含 .git 的 ZIP、TAR 或 TAR.GZ 源码包")
		return
	}
	defer file.Close()
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
	target := filepath.Join(directory, uploadID+".archive")
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
	archiveFormat, validationErr := validateNodeSourceArchive(target, header.Filename)
	if validationErr != nil {
		_ = os.Remove(target)
		problem(w, 400, validationErr.Error())
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
	request, _ := json.Marshal(map[string]string{"upload_id": uploadID, "archive": target, "archive_format": archiveFormat, "archive_sha256": hex.EncodeToString(hash.Sum(nil)), "git_commit": commit})
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
