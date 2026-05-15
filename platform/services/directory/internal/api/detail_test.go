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

	// Use custom endpoint mode so the endpoint round-trips verbatim
	// (direct mode rewrites the host with the request source IP; that
	// rewrite is covered by TestRegister_DirectModeRewritesEndpointHost).
	req := validRegisterReq()
	req.EndpointMode = "custom"
	rec, env := doJSON(t, h, "POST", "/register", req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("register: %d", rec.Code)
	}
	id := mustData[RegisterResponse](t, env).StreamID

	rec, env = doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	view := mustData[StreamView](t, env)
	if view.StreamID != id {
		t.Fatalf("streamId = %q, want %q", view.StreamID, id)
	}
	if view.Name != req.Name || view.HostNickname != req.HostNickname ||
		view.Shader != req.Shader || view.Endpoint != req.Endpoint ||
		view.EndpointMode != req.EndpointMode || view.Codec != req.Codec ||
		view.FPS != req.FPS || view.Resolution != req.Resolution {
		t.Fatalf("round-trip mismatch: got=%+v want input=%+v", view, req)
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
