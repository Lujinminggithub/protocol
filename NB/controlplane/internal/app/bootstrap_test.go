package app

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"nb-controlplane/internal/store"
)

func TestBootstrapImportsLegacyUsersOnce(t *testing.T) {
	directory := t.TempDir()
	usersPath := filepath.Join(directory, "users.conf")
	tenantsPath := filepath.Join(directory, "tenants.conf")
	salt := "00112233445566778899aabbccddeeff"
	hash := "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
	if err := os.WriteFile(usersPath, []byte("alice:210000:"+salt+":"+hash+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(tenantsPath, []byte("tenant alice 16 8 10000 1024\n"), 0600); err != nil {
		t.Fatal(err)
	}
	database, err := store.Open(filepath.Join(directory, "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	service := New(database, Config{UsersFile: usersPath, TenantsFile: tenantsPath})
	if err = service.Bootstrap(context.Background()); err != nil {
		t.Fatal(err)
	}
	user, err := database.User(context.Background(), "alice")
	if err != nil || user.MaxTCP != 16 || user.QuotaMB != 1024 || user.Credential != "imported-alice" {
		t.Fatalf("imported user=%+v err=%v", user, err)
	}
	records, err := database.AuthRecords(context.Background())
	if err != nil || len(records) != 1 || records[0].Hash != hash {
		t.Fatalf("auth records=%+v err=%v", records, err)
	}
	if err = service.Bootstrap(context.Background()); err != nil {
		t.Fatal(err)
	}
	all, err := database.Users(context.Background())
	if err != nil || len(all) != 1 {
		t.Fatalf("users=%+v err=%v", all, err)
	}
}

func TestBootstrapRejectsTenantWithoutCredential(t *testing.T) {
	directory := t.TempDir()
	tenantsPath := filepath.Join(directory, "tenants.conf")
	if err := os.WriteFile(tenantsPath, []byte("tenant alice 16 8 10000 1024\n"), 0600); err != nil {
		t.Fatal(err)
	}
	database, err := store.Open(filepath.Join(directory, "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	service := New(database, Config{TenantsFile: tenantsPath, UsersFile: filepath.Join(directory, "missing.users")})
	if err = service.Bootstrap(context.Background()); err == nil {
		t.Fatal("unpaired tenant accepted")
	}
}

func TestBootstrapDisablesCredentialWithoutTenant(t *testing.T) {
	directory := t.TempDir()
	usersPath := filepath.Join(directory, "users.conf")
	salt := "00112233445566778899aabbccddeeff"
	hash := "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
	if err := os.WriteFile(usersPath, []byte("old-test:210000:"+salt+":"+hash+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	database, err := store.Open(filepath.Join(directory, "control.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	service := New(database, Config{UsersFile: usersPath, TenantsFile: filepath.Join(directory, "missing.tenants")})
	if err = service.Bootstrap(context.Background()); err != nil {
		t.Fatal(err)
	}
	user, err := database.User(context.Background(), "old-test")
	if err != nil || user.Status != "disabled" {
		t.Fatalf("user=%+v err=%v", user, err)
	}
	records, err := database.AuthRecords(context.Background())
	if err != nil || len(records) != 0 {
		t.Fatalf("active auth=%+v err=%v", records, err)
	}
}
