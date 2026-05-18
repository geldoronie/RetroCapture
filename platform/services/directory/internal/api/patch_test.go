package api

// Tests for PATCH /streams/{id} — updates mutable fields of an
// existing entry. Owner-token gated.

import (
	"net/http"
	"strings"
	"testing"
)

func TestPatch_HappyPath_SingleField(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	newShader := "crt/easymode.glslp"
	rec, _ := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok,
		Shader:     &newShader,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", rec.Code)
	}

	rec, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("get after patch: %d", rec.Code)
	}
	view := mustData[StreamView](t, env)
	if view.Shader != newShader {
		t.Fatalf("shader = %q, want %q", view.Shader, newShader)
	}
}

// Multi-field patch: every mutable field in one request. Verifies the
// store update statement covers them all.
func TestPatch_MultipleFields(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	newName := "Renamed"
	newNick := "bob"
	newShader := "crt/other.glslp"
	newRes := Resolution{W: 1280, H: 720}
	newFPS := 30
	newCodec := "h265"
	pwReq := true
	newEndpoint := "https://renamed.example.com"
	newMode := "tunnel-cloudflare"
	rec, _ := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken:       tok,
		Name:             &newName,
		HostNickname:     &newNick,
		Shader:           &newShader,
		Resolution:       &newRes,
		FPS:              &newFPS,
		Codec:            &newCodec,
		PasswordRequired: &pwReq,
		Endpoint:         &newEndpoint,
		EndpointMode:     &newMode,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d", rec.Code)
	}
	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	v := mustData[StreamView](t, env)
	if v.Name != newName || v.HostNickname != newNick || v.Shader != newShader ||
		v.Resolution != newRes || v.FPS != newFPS || v.Codec != newCodec ||
		v.PasswordRequired != pwReq || v.Endpoint != newEndpoint || v.EndpointMode != newMode {
		t.Fatalf("not all fields applied: %+v", v)
	}
}

// Resolution is the only nested object; the patch path has a
// dedicated branch for it. Make sure it survives a round-trip.
func TestPatch_OnlyResolution(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	res := Resolution{W: 800, H: 600}
	rec, _ := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok,
		Resolution: &res,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d", rec.Code)
	}
	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	v := mustData[StreamView](t, env)
	if v.Resolution != res {
		t.Fatalf("resolution = %+v, want %+v", v.Resolution, res)
	}
}

// Sending only the ownerToken (i.e. no actual fields to update) is a
// valid PATCH per HTTP semantics — it shouldn't error.
func TestPatch_EmptyIsNoop(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	rec, _ := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204 (no-op should still succeed)", rec.Code)
	}
}

func TestPatch_Forbidden(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	shader := "x"
	rec, env := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: "wrong",
		Shader:     &shader,
	})
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeForbidden {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestPatch_NotFound(t *testing.T) {
	h, _ := newTestServer(t)
	shader := "x"
	rec, env := doJSON(t, h, "PATCH", "/streams/00000000-0000-0000-0000-000000000000",
		PatchRequest{OwnerToken: "x", Shader: &shader})
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeNotFound {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

func TestPatch_RequiresOwnerToken(t *testing.T) {
	h, _ := newTestServer(t)
	id, _ := registerOne(t, h)
	shader := "x"
	rec, env := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		Shader: &shader,
	})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeInvalidRequest {
		t.Fatalf("error envelope = %+v", env.Error)
	}
}

// Same validation rules as /register: codec, endpointMode, lengths,
// numeric ranges. Sample a few that map directly to validatePatch.
func TestPatch_Validation(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	tooLongName := strings.Repeat("x", 200)
	badCodec := "av1"
	badMode := "bogus"
	badRes := Resolution{W: 0, H: 0}
	badFPS := -1
	emptyEndpoint := ""

	cases := []struct {
		name string
		body PatchRequest
	}{
		{"name too long", PatchRequest{OwnerToken: tok, Name: &tooLongName}},
		{"bad codec", PatchRequest{OwnerToken: tok, Codec: &badCodec}},
		{"bad endpoint mode", PatchRequest{OwnerToken: tok, EndpointMode: &badMode}},
		{"bad resolution", PatchRequest{OwnerToken: tok, Resolution: &badRes}},
		{"bad fps", PatchRequest{OwnerToken: tok, FPS: &badFPS}},
		{"empty endpoint", PatchRequest{OwnerToken: tok, Endpoint: &emptyEndpoint}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			rec, env := doJSON(t, h, "PATCH", "/streams/"+id, c.body)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (env=%+v)", rec.Code, env)
			}
			if env.Error == nil || env.Error.Code != CodeInvalidRequest {
				t.Fatalf("error envelope = %+v", env.Error)
			}
		})
	}
}

// Untouched fields must survive a patch — the SET clause is built
// dynamically and a regression could nuke everything.
func TestPatch_DoesNotResetOtherFields(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)
	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	before := mustData[StreamView](t, env)

	newShader := "crt/zzz.glslp"
	doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok, Shader: &newShader,
	})
	_, env = doJSON(t, h, "GET", "/streams/"+id, nil)
	after := mustData[StreamView](t, env)

	if after.Shader != newShader {
		t.Fatalf("shader didn't change: %q", after.Shader)
	}
	if after.Name != before.Name || after.HostNickname != before.HostNickname ||
		after.Endpoint != before.Endpoint || after.EndpointMode != before.EndpointMode ||
		after.Resolution != before.Resolution || after.FPS != before.FPS ||
		after.Codec != before.Codec {
		t.Fatalf("untouched fields changed: before=%+v after=%+v", before, after)
	}
}
