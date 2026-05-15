package api

// Tests for DELETE /streams/{id} — explicit removal. Owner-token
// gated. Idempotent.

import (
	"net/http"
	"testing"
)

func TestDelete_HappyPath(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	rec, _ := doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{OwnerToken: tok})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", rec.Code)
	}

	// Confirm gone.
	rec, _ = doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("get after delete: status = %d, want 404", rec.Code)
	}
}

func TestDelete_Idempotent(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	// First delete succeeds.
	rec, _ := doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{OwnerToken: tok})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("first delete: %d", rec.Code)
	}
	// Second delete on the same id must still succeed — spec.
	rec, _ = doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{OwnerToken: tok})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("second delete: %d (want 204 idempotent)", rec.Code)
	}
}

func TestDelete_Forbidden(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)

	rec, env := doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{OwnerToken: "wrong"})
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeForbidden {
		t.Fatalf("error envelope = %+v", env.Error)
	}

	// Entry must still exist (forbidden ≠ delete).
	rec, _ = doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("entry was deleted despite 403: get returned %d", rec.Code)
	}
}

func TestDelete_RequiresOwnerToken(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	rec, env := doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

// Deleting a never-existed id behaves the same as deleting an
// already-deleted one (idempotent, returns 204) as long as the
// caller supplies an ownerToken at all. This matches the spec: the
// directory can't distinguish "expired and reaped" from "never
// existed".
func TestDelete_NonexistentIDIdempotent(t *testing.T) {
	h, _ := newTestServer(t)
	rec, _ := doJSON(t, h, "DELETE", "/streams/00000000-0000-0000-0000-000000000000",
		DeleteRequest{OwnerToken: "anything"})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204 (idempotent on missing)", rec.Code)
	}
}
