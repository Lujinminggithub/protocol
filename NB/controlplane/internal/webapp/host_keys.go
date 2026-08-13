package webapp

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

const hostKeyConfirmationLifetime = 10 * time.Minute

type hostKeyScanRequest struct {
	Host    string `json:"host"`
	SSHPort int    `json:"ssh_port"`
}

type hostKeyConfirmation struct {
	Host        string `json:"host"`
	SSHPort     int    `json:"ssh_port"`
	Key         string `json:"ssh_host_key"`
	KeyType     string `json:"ssh_host_key_type"`
	Fingerprint string `json:"ssh_host_key_sha256"`
	ExpiresAt   int64  `json:"expires_at"`
}

func scanSSHHostKey(ctx context.Context, host string, port int) (ssh.PublicKey, error) {
	address := net.JoinHostPort(host, strconv.Itoa(port))
	dialer := net.Dialer{Timeout: 8 * time.Second}
	connection, err := dialer.DialContext(ctx, "tcp", address)
	if err != nil {
		return nil, errors.New("无法连接 SSH 管理端口")
	}
	defer connection.Close()
	deadline := time.Now().Add(8 * time.Second)
	if contextDeadline, ok := ctx.Deadline(); ok && contextDeadline.Before(deadline) {
		deadline = contextDeadline
	}
	_ = connection.SetDeadline(deadline)
	var captured ssh.PublicKey
	configuration := &ssh.ClientConfig{
		User: "nb-host-key-scan",
		HostKeyCallback: func(_ string, _ net.Addr, key ssh.PublicKey) error {
			captured = key
			return nil
		},
	}
	client, _, _, handshakeErr := ssh.NewClientConn(connection, address, configuration)
	if client != nil {
		_ = client.Close()
	}
	if captured == nil {
		if handshakeErr != nil {
			return nil, errors.New("目标端未返回有效的 SSH 主机密钥")
		}
		return nil, errors.New("SSH 主机密钥采集失败")
	}
	return captured, nil
}

var errSSHHostKeyChanged = errors.New("SSH 主机密钥与已确认记录不一致，请重新扫描并确认")

func verifySSHPassword(ctx context.Context, host string, port int, user, password string, expected ssh.PublicKey) error {
	address := net.JoinHostPort(host, strconv.Itoa(port))
	dialer := net.Dialer{Timeout: 8 * time.Second}
	connection, err := dialer.DialContext(ctx, "tcp", address)
	if err != nil {
		return errors.New("无法连接 SSH 管理端口，密码未保存")
	}
	defer connection.Close()
	deadline := time.Now().Add(10 * time.Second)
	if contextDeadline, ok := ctx.Deadline(); ok && contextDeadline.Before(deadline) {
		deadline = contextDeadline
	}
	_ = connection.SetDeadline(deadline)
	configuration := &ssh.ClientConfig{
		User:              user,
		Auth:              []ssh.AuthMethod{ssh.Password(password)},
		HostKeyAlgorithms: []string{expected.Type()},
		HostKeyCallback: func(_ string, _ net.Addr, actual ssh.PublicKey) error {
			if actual.Type() != expected.Type() || !bytes.Equal(actual.Marshal(), expected.Marshal()) {
				return errSSHHostKeyChanged
			}
			return nil
		},
		Timeout: 10 * time.Second,
	}
	client, _, _, handshakeErr := ssh.NewClientConn(connection, address, configuration)
	if client != nil {
		_ = client.Close()
	}
	if handshakeErr == nil {
		return nil
	}
	if errors.Is(handshakeErr, errSSHHostKeyChanged) {
		return errSSHHostKeyChanged
	}
	if strings.Contains(handshakeErr.Error(), "unable to authenticate") {
		return errors.New("SSH 密码认证失败，请重新输入正确密码")
	}
	return errors.New("SSH 登录验证失败，密码未保存")
}

func publicKeyFields(key ssh.PublicKey) (string, string, string) {
	return base64.StdEncoding.EncodeToString(key.Marshal()), key.Type(), ssh.FingerprintSHA256(key)
}

func validatePublicKey(encoded, keyType, fingerprint string) (ssh.PublicKey, error) {
	raw, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, errors.New("SSH 主机公钥格式无效")
	}
	key, err := ssh.ParsePublicKey(raw)
	if err != nil || key.Type() != keyType || ssh.FingerprintSHA256(key) != fingerprint {
		return nil, errors.New("SSH 主机密钥确认信息不匹配")
	}
	return key, nil
}

func (a *App) signHostKeyConfirmation(value hostKeyConfirmation) (string, error) {
	payload, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	encoded := base64.RawURLEncoding.EncodeToString(payload)
	mac := hmac.New(sha256.New, a.hostKeyTokenKey[:])
	_, _ = mac.Write([]byte(encoded))
	signature := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	return encoded + "." + signature, nil
}

func (a *App) verifyHostKeyConfirmation(token string, expected hostKeyConfirmation) error {
	parts := strings.Split(token, ".")
	if len(parts) != 2 {
		return errors.New("请先扫描并确认 SSH 主机密钥")
	}
	mac := hmac.New(sha256.New, a.hostKeyTokenKey[:])
	_, _ = mac.Write([]byte(parts[0]))
	signature, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil || !hmac.Equal(signature, mac.Sum(nil)) {
		return errors.New("SSH 主机密钥确认令牌无效")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return errors.New("SSH 主机密钥确认令牌无效")
	}
	var actual hostKeyConfirmation
	if json.Unmarshal(payload, &actual) != nil || actual.ExpiresAt < time.Now().Unix() {
		return errors.New("SSH 主机密钥确认已过期，请重新扫描")
	}
	if actual.Host != expected.Host || actual.SSHPort != expected.SSHPort || actual.Key != expected.Key ||
		actual.KeyType != expected.KeyType || actual.Fingerprint != expected.Fingerprint {
		return errors.New("SSH 主机密钥确认信息不匹配")
	}
	return nil
}

func (a *App) scanDeviceHostKey(w http.ResponseWriter, r *http.Request) {
	var request hostKeyScanRequest
	if !decode(w, r, &request) {
		return
	}
	request.Host = strings.TrimSpace(request.Host)
	if !validHost(request.Host) || request.SSHPort < 1 || request.SSHPort > 65535 {
		problem(w, http.StatusBadRequest, "SSH 地址或端口无效")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	key, err := scanSSHHostKey(ctx, request.Host, request.SSHPort)
	if err != nil {
		problem(w, http.StatusBadGateway, err.Error())
		return
	}
	encoded, keyType, fingerprint := publicKeyFields(key)
	confirmation := hostKeyConfirmation{Host: request.Host, SSHPort: request.SSHPort, Key: encoded,
		KeyType: keyType, Fingerprint: fingerprint, ExpiresAt: time.Now().Add(hostKeyConfirmationLifetime).Unix()}
	token, err := a.signHostKeyConfirmation(confirmation)
	if err != nil {
		problem(w, http.StatusInternalServerError, "生成 SSH 主机密钥确认信息失败")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"host": request.Host, "ssh_port": request.SSHPort, "ssh_host_key": encoded,
		"ssh_host_key_type": keyType, "ssh_host_key_sha256": fingerprint,
		"confirmation_token": token, "expires_at": time.Unix(confirmation.ExpiresAt, 0).UTC().Format(time.RFC3339),
	})
}

func confirmedHostKey(host string, port int, encoded, keyType, fingerprint, token string, app *App) (hostKeyConfirmation, error) {
	if encoded == "" || keyType == "" || fingerprint == "" || token == "" {
		return hostKeyConfirmation{}, errors.New("请先扫描并确认 SSH 主机密钥")
	}
	if _, err := validatePublicKey(encoded, keyType, fingerprint); err != nil {
		return hostKeyConfirmation{}, err
	}
	confirmation := hostKeyConfirmation{Host: host, SSHPort: port, Key: encoded, KeyType: keyType, Fingerprint: fingerprint}
	if err := app.verifyHostKeyConfirmation(token, confirmation); err != nil {
		return hostKeyConfirmation{}, err
	}
	return confirmation, nil
}

func hostKeyLine(host string, port int, keyType, encoded string) string {
	marker := host
	if port != 22 {
		marker = fmt.Sprintf("[%s]:%d", host, port)
	}
	return marker + " " + keyType + " " + encoded
}
