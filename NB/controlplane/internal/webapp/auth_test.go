package webapp

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"nb-controlplane/internal/central"
)

func TestAdminMustChangeInitialPasswordBeforeUsingAPI(t *testing.T) {
	store, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	server := httptest.NewServer(New(store, Config{AgentToken: "agent"}).Handler())
	defer server.Close()

	login := postAuthJSON(t, server.URL+"/api/v1/auth/login", "", map[string]string{
		"username": "admin", "password": "admin123!@#"})
	if login.StatusCode != http.StatusOK {
		t.Fatalf("login status=%d", login.StatusCode)
	}
	var session struct {
		Token      string `json:"token"`
		MustChange bool   `json:"must_change_password"`
	}
	if json.NewDecoder(login.Body).Decode(&session) != nil || session.Token == "" || !session.MustChange {
		t.Fatalf("invalid initial session: %+v", session)
	}
	login.Body.Close()

	dashboard, _ := http.NewRequest(http.MethodGet, server.URL+"/api/v1/dashboard", nil)
	dashboard.Header.Set("Authorization", "Bearer "+session.Token)
	response, err := server.Client().Do(dashboard)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusForbidden {
		t.Fatalf("dashboard before password change status=%d", response.StatusCode)
	}
	response.Body.Close()

	tweak := postAuthJSON(t, server.URL+"/api/v1/auth/password", session.Token, map[string]string{
		"current_password": "admin123!@#", "new_password": "NewAdmin!Pass"})
	if tweak.StatusCode != http.StatusOK {
		t.Fatalf("password change status=%d", tweak.StatusCode)
	}
	tweak.Body.Close()

	dashboard, _ = http.NewRequest(http.MethodGet, server.URL+"/api/v1/dashboard", nil)
	dashboard.Header.Set("Authorization", "Bearer "+session.Token)
	response, err = server.Client().Do(dashboard)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusOK {
		t.Fatalf("dashboard after password change status=%d", response.StatusCode)
	}
	response.Body.Close()
}

func TestPasswordPolicyRequiresUpperLowerAndSpecial(t *testing.T) {
	for _, password := range []string{"short!A", "alllower!", "ALLUPPER!", "NoSpecial1"} {
		if validateAdminPassword(password) == nil {
			t.Fatalf("password %q should be rejected", password)
		}
	}
	if err := validateAdminPassword("Valid!Pass"); err != nil {
		t.Fatalf("valid password rejected: %v", err)
	}
}

func postAuthJSON(t *testing.T, endpoint, token string, payload any) *http.Response {
	t.Helper()
	encoded, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	request, _ := http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(encoded))
	request.Header.Set("Content-Type", "application/json")
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	return response
}
