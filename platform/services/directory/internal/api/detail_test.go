package api

// Tests for GET /streams/{id} — single-entry detail.

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

func TestGet_HappyPath(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)

	rec, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	view := mustData[StreamView](t, env)
	if view.StreamID != id {
		t.Fatalf("streamId = %q, want %q", view.StreamID, id)
	}
	// All the canonical fields from a valid register should round-trip.
	want := validRegisterReq()
	if view.Name != want.Name || view.HostNickname != want.HostNickname ||
		view.Shader != want.Shader || view.Endpoint != want.Endpoint ||
		view.EndpointMode != want.EndpointMode || view.Codec != want.Codec ||
		view.FPS != want.FPS || view.Resolution != want.Resolution {
		t.Fatalf("round-trip mismatch: got=%+v want input=%+v", view, want)
	}
}

func TestGet_NotFound(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "GET", "/streams/00000000-0000-0000-0000-000000000000", nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeNotFound {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestGet_OwnerTokenNeverInResponse(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	b, _ := json.Marshal(env)
	if strings.Contains(string(b), tok) {
		t.Fatal("owner token leaked into GET response")
	}
	if strings.Contains(string(b), "ownerToken") {
		t.Fatal("ownerToken field present in GET response")
	}
}

// Detail must reflect the latest persisted state, not a stale cache.
// After a PATCH the next GET should return the new values.
func TestGet_ReflectsPatch(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	newShader := "after/patch.glslp"
	doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok, Shader: &newShader,
	})

	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	view := mustData[StreamView](t, env)
	if view.Shader != newShader {
		t.Fatalf("shader = %q, want %q (patch not reflected)", view.Shader, newShader)
	}
}
