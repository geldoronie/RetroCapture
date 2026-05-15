package api

// Tests for POST /register — creates a directory entry, returns a
// uuid streamId + owner token.

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

func TestRegister_HappyPath(t *testing.T) {
	h, st := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())

	if rec.Code != http.StatusCreated {
		t.Fatalf("status = %d, want 201; env=%+v", rec.Code, env)
	}
	r := mustData[RegisterResponse](t, env)

	if r.StreamID == "" {
		t.Fatal("streamId missing in response")
	}
	if r.ExpiresAt == "" {
		t.Fatal("expiresAt missing in response")
	}
	if len(r.OwnerToken) != 64 {
		t.Fatalf("ownerToken length = %d, want 64 (32 bytes hex)", len(r.OwnerToken))
	}

	got, err := st.Get(t.Context(), r.StreamID)
	if err != nil {
		t.Fatalf("entry not persisted: %v", err)
	}
	if got.PublicIP != "9.9.9.9" {
		t.Fatalf("public_ip = %q, want 9.9.9.9 (taken from RemoteAddr)", got.PublicIP)
	}
	if got.Name != "Test Stream" || got.HostNickname != "alice" {
		t.Fatalf("unexpected entry: %+v", got)
	}
}

// Both supported codecs must be accepted.
func TestRegister_BothCodecs(t *testing.T) {
	h, _ := newTestServer(t)
	for _, c := range []string{"h264", "h265"} {
		t.Run(c, func(t *testing.T) {
			req := validRegisterReq()
			req.Codec = c
			rec, env := doJSON(t, h, "POST", "/register", req)
			if rec.Code != http.StatusCreated {
				t.Fatalf("codec=%q status=%d env=%+v", c, rec.Code, env)
			}
		})
	}
}

// Every documented endpoint mode must be accepted.
func TestRegister_AllEndpointModes(t *testing.T) {
	h, _ := newTestServer(t)
	for _, m := range []string{"direct", "tunnel-cloudflare", "custom"} {
		t.Run(m, func(t *testing.T) {
			req := validRegisterReq()
			req.EndpointMode = m
			rec, env := doJSON(t, h, "POST", "/register", req)
			if rec.Code != http.StatusCreated {
				t.Fatalf("endpointMode=%q status=%d env=%+v", m, rec.Code, env)
			}
		})
	}
}

// Validation matrix: every field constraint gets a dedicated subtest
// so a regression in one constraint surfaces clearly.
func TestRegister_Validation(t *testing.T) {
	h, _ := newTestServer(t)
	cases := []struct {
		name   string
		mutate func(*RegisterRequest)
	}{
		{"empty name", func(r *RegisterRequest) { r.Name = "" }},
		{"name too long", func(r *RegisterRequest) { r.Name = strings.Repeat("x", 121) }},
		{"nickname too long", func(r *RegisterRequest) { r.HostNickname = strings.Repeat("x", 41) }},
		{"shader too long", func(r *RegisterRequest) { r.Shader = strings.Repeat("x", 201) }},
		{"endpoint too long", func(r *RegisterRequest) { r.Endpoint = strings.Repeat("x", 501) }},
		{"empty endpoint", func(r *RegisterRequest) { r.Endpoint = "" }},
		{"endpoint whitespace only", func(r *RegisterRequest) { r.Endpoint = "   " }},
		{"resolution zero width", func(r *RegisterRequest) { r.Resolution.W = 0 }},
		{"resolution zero height", func(r *RegisterRequest) { r.Resolution.H = 0 }},
		{"resolution negative", func(r *RegisterRequest) { r.Resolution.W = -1 }},
		{"resolution too wide", func(r *RegisterRequest) { r.Resolution.W = 99_999 }},
		{"fps zero", func(r *RegisterRequest) { r.FPS = 0 }},
		{"fps negative", func(r *RegisterRequest) { r.FPS = -1 }},
		{"fps absurd", func(r *RegisterRequest) { r.FPS = 100_000 }},
		{"bad codec", func(r *RegisterRequest) { r.Codec = "av1" }},
		{"bad endpoint mode", func(r *RegisterRequest) { r.EndpointMode = "bogus" }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			req := validRegisterReq()
			c.mutate(&req)
			rec, env := doJSON(t, h, "POST", "/register", req)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (env=%+v)", rec.Code, env)
			}
			if env.Error == nil || env.Error.Code != CodeInvalidRequest {
				t.Fatalf("error = %+v, want code=invalid_request", env.Error)
			}
		})
	}
}

// JSON decoder is configured to reject unknown fields so silent typos
// in the client don't become silent dropped data on the server.
func TestRegister_RejectsUnknownFields(t *testing.T) {
	h, _ := newTestServer(t)
	body := []byte(`{"name":"x","shader":"x","resolution":{"w":1,"h":1},"fps":60,"codec":"h264","endpoint":"y","endpointMode":"direct","sneaky":true}`)
	rec := doRaw(t, h, "POST", "/register", body)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for unknown field", rec.Code)
	}
}

func TestRegister_RejectsEmptyBody(t *testing.T) {
	h, _ := newTestServer(t)
	rec := doRaw(t, h, "POST", "/register", nil)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for empty body", rec.Code)
	}
}

func TestRegister_RejectsMalformedJSON(t *testing.T) {
	h, _ := newTestServer(t)
	rec := doRaw(t, h, "POST", "/register", []byte(`{not json`))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for malformed JSON", rec.Code)
	}
}

// Owner token must never leak through any subsequent read path. The
// register response is the only place a client should ever see it.
func TestRegister_OwnerTokenNeverEchoedInGet(t *testing.T) {
	h, _ := newTestServer(t)
	streamID, ownerToken := registerOne(t, h)

	// Detail endpoint
	rec, env := doJSON(t, h, "GET", "/streams/"+streamID, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("get status = %d", rec.Code)
	}
	b, _ := json.Marshal(env)
	if strings.Contains(string(b), ownerToken) {
		t.Fatal("owner token leaked in GET /streams/{id}")
	}
	if strings.Contains(string(b), "ownerToken") {
		t.Fatal("ownerToken field present in GET /streams/{id} response")
	}

	// List endpoint
	rec, env = doJSON(t, h, "GET", "/streams", nil)
	b, _ = json.Marshal(env)
	if strings.Contains(string(b), ownerToken) {
		t.Fatal("owner token leaked in GET /streams")
	}
	if strings.Contains(string(b), "ownerToken") {
		t.Fatal("ownerToken field present in GET /streams response")
	}
}

// Public IP comes from the request, not the body — host cannot spoof
// it by claiming to be elsewhere.
func TestRegister_PublicIPRecordedFromRequest(t *testing.T) {
	h, st := newTestServer(t)
	rec, env := doJSONFrom(t, h, "POST", "/register", validRegisterReq(), "203.0.113.5:9999")
	if rec.Code != http.StatusCreated {
		t.Fatalf("register: %d", rec.Code)
	}
	r := mustData[RegisterResponse](t, env)
	got, err := st.Get(t.Context(), r.StreamID)
	if err != nil {
		t.Fatalf("entry missing: %v", err)
	}
	if got.PublicIP != "203.0.113.5" {
		t.Fatalf("public_ip = %q, want 203.0.113.5", got.PublicIP)
	}
}
