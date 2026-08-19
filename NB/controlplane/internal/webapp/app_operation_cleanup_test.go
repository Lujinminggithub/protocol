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

func TestOperationCleanupAPIHandlesSingleAndBatchDeletion(t *testing.T) {
	database, err := central.Open(filepath.Join(t.TempDir(), "central.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err = database.UpsertLine(t.Context(), central.Line{ID: "line-clean", Name: "clean", Status: "draft",
		EntryRegion: "entry", ExitRegion: "exit", Provider: "test", CapacityMbps: 10}); err != nil {
		t.Fatal(err)
	}
	createCancelled := func(id, key string) {
		t.Helper()
		_, _, createErr := database.CreateOperation(t.Context(), central.Operation{ID: id, LineID: "line-clean",
			Kind: "line.validate", RequestedBy: "operator", IdempotencyKey: key, Request: json.RawMessage(`{}`)})
		if createErr != nil {
			t.Fatal(createErr)
		}
		if cancelErr := database.CancelOperation(t.Context(), id, "test cleanup"); cancelErr != nil {
			t.Fatal(cancelErr)
		}
	}
	createCancelled("op-single", "single")
	createCancelled("op-batch-a", "batch-a")
	createCancelled("op-batch-b", "batch-b")
	active, _, err := database.CreateOperation(t.Context(), central.Operation{ID: "op-active", LineID: "line-clean",
		Kind: "line.validate", RequestedBy: "operator", IdempotencyKey: "active", Request: json.RawMessage(`{}`)})
	if err != nil {
		t.Fatal(err)
	}

	server := httptest.NewServer(New(database, Config{AdminToken: "admin", AgentToken: "agent"}).Handler())
	defer server.Close()
	response, body := call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/operations/op-single", "admin", "", nil)
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"deleted":1`)) {
		t.Fatalf("single cleanup status=%d body=%s", response.StatusCode, body)
	}
	if _, err = database.Operation(t.Context(), "op-single"); err != nil {
		t.Fatalf("single cleanup removed durable evidence: %v", err)
	}
	visible, err := database.Operations(t.Context(), "line-clean", 10)
	if err != nil {
		t.Fatal(err)
	}
	for _, item := range visible {
		if item.ID == "op-single" {
			t.Fatalf("single cleaned operation remains visible: %+v", item)
		}
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/operations", "admin", "",
		map[string]any{"ids": []string{"op-batch-a", "op-batch-b"}})
	if response.StatusCode != http.StatusOK || !bytes.Contains(body, []byte(`"deleted":2`)) {
		t.Fatalf("batch cleanup status=%d body=%s", response.StatusCode, body)
	}
	response, body = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/operations/"+active.ID, "admin", "", nil)
	if response.StatusCode != http.StatusConflict || !bytes.Contains(body, []byte("执行中的任务不能清理")) {
		t.Fatalf("active cleanup status=%d body=%s", response.StatusCode, body)
	}
	response, _ = call(t, server.Client(), http.MethodDelete, server.URL+"/api/v1/operations/op-active", "agent", "", nil)
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("agent deleted operation status=%d", response.StatusCode)
	}
}
