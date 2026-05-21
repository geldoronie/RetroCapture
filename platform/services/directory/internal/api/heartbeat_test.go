package api

// Tests for POST /heartbeat — keeps an entry alive, optionally
// updates clientCount.

import (
	"net/http"
	"testing"
	"time"
)

func TestHeartbeat_HappyPath_RefreshesExpiry(t *testing.T) {
	h, st := newTestServer(t)
	id, tok := registerOne(t, h)

	before, err := st.Get(t.Context(), id)
	if err != nil {
		t.Fatalf("get before: %v", err)
	}

	// Sleep a tick so the new expires_at is strictly later than the
	// register-time expires_at (we store seconds, so a sub-second
	// resolution sleep is enough to roll over).
	time.Sleep(1100 * time.Millisecond)

	rec, env := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:   id,
		OwnerToken: tok,
	})
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (env=%+v)", rec.Code, env)
	}
	r := mustData[HeartbeatResponse](t, env)
	if r.ExpiresAt == "" {
		t.Fatal("expiresAt missing in heartbeat response")
	}

	after, err := st.Get(t.Context(), id)
	if err != nil {
		t.Fatalf("get after: %v", err)
	}
	if !after.ExpiresAt.After(before.ExpiresAt) {
		t.Fatalf("expires_at did not advance: before=%v after=%v", before.ExpiresAt, after.ExpiresAt)
	}
}

func TestHeartbeat_UpdatesClientCount(t *testing.T) {
	h, st := newTestServer(t)
	id, tok := registerOne(t, h)

	cc := 7
	rec, _ := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:    id,
		OwnerToken:  tok,
		ClientCount: &cc,
	})
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	got, _ := st.Get(t.Context(), id)
	if got.ClientCount != 7 {
		t.Fatalf("client_count = %d, want 7", got.ClientCount)
	}
}

// Heartbeat without ClientCount must not zero it out — the field is
// optional and "unspecified" means "leave alone".
func TestHeartbeat_OmittedClientCountLeavesItUnchanged(t *testing.T) {
	h, st := newTestServer(t)
	id, tok := registerOne(t, h)

	// Seed a count.
	five := 5
	doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: id, OwnerToken: tok, ClientCount: &five,
	})

	// Heartbeat again with no clientCount field.
	rec, _ := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: id, OwnerToken: tok,
	})
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	got, _ := st.Get(t.Context(), id)
	if got.ClientCount != 5 {
		t.Fatalf("client_count = %d, want 5 (should not have been reset)", got.ClientCount)
	}
}

func TestHeartbeat_Forbidden_WrongToken(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)

	rec, env := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:   id,
		OwnerToken: "wrong-token",
	})
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeForbidden {
		t.Fatalf("error envelope = %+v, want forbidden", env.Error)
	}
}

func TestHeartbeat_NotFound(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:   "00000000-0000-0000-0000-000000000000",
		OwnerToken: "anything",
	})
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeNotFound {
		t.Fatalf("error envelope = %+v, want not_found", env.Error)
	}
}

func TestHeartbeat_RequiresStreamID(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		OwnerToken: "tok",
	})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestHeartbeat_RequiresOwnerToken(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: "some-id",
	})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestHeartbeat_RejectsNegativeClientCount(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	bad := -1
	rec, _ := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: id, OwnerToken: tok, ClientCount: &bad,
	})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for negative clientCount", rec.Code)
	}
}

func TestHeartbeat_RejectsAbsurdClientCount(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	tooMany := 1_000_000
	rec, _ := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: id, OwnerToken: tok, ClientCount: &tooMany,
	})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for clientCount out of range", rec.Code)
	}
}
