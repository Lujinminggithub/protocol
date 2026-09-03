package webapp

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"net/http"
	"strings"
	"sync"
	"time"
	"unicode"

	"golang.org/x/crypto/bcrypt"
	"nb-controlplane/internal/central"
)

const (
	initialAdminUsername = "admin"
	initialAdminPassword = "admin123!@#"
	adminSessionLifetime = 12 * time.Hour
)

var initialAdminHash struct {
	sync.Once
	value []byte
	err   error
}

func defaultAdminPasswordHash() ([]byte, error) {
	initialAdminHash.Do(func() {
		initialAdminHash.value, initialAdminHash.err = bcrypt.GenerateFromPassword([]byte(initialAdminPassword), bcrypt.DefaultCost)
	})
	return initialAdminHash.value, initialAdminHash.err
}

func validateAdminPassword(password string) error {
	if len([]rune(password)) < 8 {
		return errors.New("新密码至少需要 8 个字符")
	}
	var upper, lower, special bool
	for _, value := range password {
		upper = upper || unicode.IsUpper(value)
		lower = lower || unicode.IsLower(value)
		special = special || (!unicode.IsLetter(value) && !unicode.IsDigit(value))
	}
	if !upper || !lower || !special {
		return errors.New("新密码必须同时包含大写字母、小写字母和特殊字符")
	}
	return nil
}

func sessionToken() (string, []byte, error) {
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", nil, err
	}
	token := base64.RawURLEncoding.EncodeToString(raw)
	digest := sha256.Sum256([]byte(token))
	return token, digest[:], nil
}

func bearerHash(r *http.Request) []byte {
	token := strings.TrimSpace(bearer(r))
	if token == "" {
		return nil
	}
	digest := sha256.Sum256([]byte(token))
	return digest[:]
}

func (a *App) authenticateSession(ctx context.Context, r *http.Request) (central.UserSession, []byte, error) {
	hash := bearerHash(r)
	if len(hash) == 0 {
		return central.UserSession{}, nil, central.ErrAuthentication
	}
	user, err := a.store.UserBySession(ctx, hash, time.Now().UTC())
	return user, hash, err
}

func (a *App) login(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !decode(w, r, &request) {
		return
	}
	user, err := a.store.UserForLogin(r.Context(), strings.TrimSpace(request.Username))
	if err != nil || bcrypt.CompareHashAndPassword(user.PasswordHash, []byte(request.Password)) != nil {
		problem(w, http.StatusUnauthorized, "用户名或密码错误")
		return
	}
	token, hash, err := sessionToken()
	if err != nil || a.store.CreateUserSession(r.Context(), user.UserID, hash, time.Now().UTC().Add(adminSessionLifetime)) != nil {
		problem(w, http.StatusInternalServerError, "创建登录会话失败")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"token": token, "username": user.Username,
		"must_change_password": user.MustChangePassword, "expires_in_seconds": int(adminSessionLifetime / time.Second)})
}

func (a *App) changePassword(w http.ResponseWriter, r *http.Request) {
	user, tokenHash, err := a.authenticateSession(r.Context(), r)
	if err != nil {
		problem(w, http.StatusUnauthorized, "登录会话无效或已过期")
		return
	}
	var request struct {
		CurrentPassword string `json:"current_password"`
		NewPassword     string `json:"new_password"`
	}
	if !decode(w, r, &request) {
		return
	}
	if bcrypt.CompareHashAndPassword(user.PasswordHash, []byte(request.CurrentPassword)) != nil {
		problem(w, http.StatusUnauthorized, "当前密码错误")
		return
	}
	if err = validateAdminPassword(request.NewPassword); err != nil {
		problem(w, http.StatusBadRequest, err.Error())
		return
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(request.NewPassword), bcrypt.DefaultCost)
	if err != nil || a.store.ChangeUserPassword(r.Context(), user.UserID, tokenHash, hash) != nil {
		problem(w, http.StatusInternalServerError, "修改密码失败")
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"updated": true})
}

func (a *App) currentUser(w http.ResponseWriter, r *http.Request) {
	user, _, err := a.authenticateSession(r.Context(), r)
	if err != nil {
		problem(w, http.StatusUnauthorized, "登录会话无效或已过期")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"username": user.Username,
		"must_change_password": user.MustChangePassword})
}
