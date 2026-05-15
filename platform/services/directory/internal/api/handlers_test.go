package api

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

// newTestServer wires a real store + the public Routes() handler so
// each test exercises the full stack. The DB lives under t.TempDir
// so tests are independent.
func newTestServer(t *testing.T) (http.Handler, *store.Store) {
	t.Helper()
	st, err := store.Open(filepath.Join(t.TempDir(), "h.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })

	srv := &Server{
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		Store:  st,
		TTL:    2 * time.Minute,
	}
	return srv.Routes(), st
}

func doJSON(t *testing.T, h http.Handler, method, path string, body any) (*httptest.ResponseRecorder, Envelope) {
	t.Helper()
	var buf bytes.Buffer
	if body != nil {
		if err := json.NewEncoder(&buf).Encode(body); err != nil {
			t.Fatalf("encode body: %v", err)
		}
	}
	req := httptest.NewRequest(method, path, &buf)
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "9.9.9.9:54321"
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	var env Envelope
	if rec.Body.Len() > 0 {
		_ = json.NewDecoder(rec.Body).Decode(&env)
	}
	return rec, env
}

func mustData[T any](t *testing.T, env Envelope) T {
	t.Helper()
	b, err := json.Marshal(env.Data)
	if err != nil {
		t.Fatalf("re-marshal data: %v", err)
	}
	var out T
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatalf("decode into target: %v", err)
	}
	return out
}

func validRegisterReq() RegisterRequest {
	return RegisterRequest{
		Name:         "Test Stream",
		HostNickname: "alice",
		Shader:       "crt/test.glslp",
		Resolution:   Resolution{W: 1920, H: 1080},
		FPS:          60,
		Codec:        "h264",
		Endpoint:     "https://example.com/raw",
		EndpointMode: "direct",
		Version:      "0.7.0-alpha",
	}
}

func TestHealth(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "GET", "/health", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if env.Error != nil {
		t.Fatalf("env.Error = %v", env.Error)
	}
	resp := mustData[HealthResponse](t, env)
	if resp.Status != "ok" || resp.ProtocolVersion != ProtocolVersion {
		t.Fatalf("unexpected health body: %+v", resp)
	}
}

func TestRegisterHappyPath(t *testing.T) {
	h, st := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	if rec.Code != http.StatusCreated {
		t.Fatalf("status = %d, want 201; env=%+v", rec.Code, env)
	}
	resp := mustData[RegisterResponse](t, env)
	if resp.StreamID == "" || resp.OwnerToken == "" || resp.ExpiresAt == "" {
		t.Fatalf("missing fields in response: %+v", resp)
	}
	// Owner token must be 64 hex chars (32 bytes).
	if len(resp.OwnerToken) != 64 {
		t.Fatalf("ownerToken length = %d, want 64", len(resp.OwnerToken))
	}

	// And the entry should actually exist in the store, with the
	// request IP recorded.
	got, err := st.Get(t.Context(), resp.StreamID)
	if err != nil {
		t.Fatalf("entry not in store: %v", err)
	}
	if got.PublicIP != "9.9.9.9" {
		t.Fatalf("public_ip = %q, want 9.9.9.9", got.PublicIP)
	}
}

func TestRegisterValidation(t *testing.T) {
	h, _ := newTestServer(t)
	cases := []struct {
		name  string
		mutate func(*RegisterRequest)
	}{
		{"empty name", func(r *RegisterRequest) { r.Name = "" }},
		{"bad codec", func(r *RegisterRequest) { r.Codec = "av1" }},
		{"bad endpoint mode", func(r *RegisterRequest) { r.EndpointMode = "bogus" }},
		{"empty endpoint", func(r *RegisterRequest) { r.Endpoint = "" }},
		{"bad resolution", func(r *RegisterRequest) { r.Resolution.W = -1 }},
		{"bad fps", func(r *RegisterRequest) { r.FPS = 0 }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			req := validRegisterReq()
			c.mutate(&req)
			rec, env := doJSON(t, h, "POST", "/register", req)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400; env=%+v", rec.Code, env)
			}
			if env.Error == nil || env.Error.Code != CodeInvalidRequest {
				t.Fatalf("env.Error = %+v", env.Error)
			}
		})
	}
}

func TestRegisterRejectsUnknownFields(t *testing.T) {
	h, _ := newTestServer(t)
	body := []byte(`{"name":"x","codec":"h264","resolution":{"w":1,"h":1},"fps":60,"endpoint":"y","endpointMode":"direct","sneaky":true}`)
	req := httptest.NewRequest("POST", "/register", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for unknown field", rec.Code)
	}
}

func TestHeartbeatHappyPath(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	if rec.Code != http.StatusCreated {
		t.Fatalf("register failed: %d %+v", rec.Code, env)
	}
	regResp := mustData[RegisterResponse](t, env)

	cc := 3
	hbReq := HeartbeatRequest{
		StreamID:    regResp.StreamID,
		OwnerToken:  regResp.OwnerToken,
		ClientCount: &cc,
	}
	rec, env = doJSON(t, h, "POST", "/heartbeat", hbReq)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200; env=%+v", rec.Code, env)
	}
	hbResp := mustData[HeartbeatResponse](t, env)
	if hbResp.ExpiresAt == "" {
		t.Fatal("missing expiresAt in heartbeat response")
	}
}

func TestHeartbeatForbidden(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	if rec.Code != http.StatusCreated {
		t.Fatalf("register: %d", rec.Code)
	}
	regResp := mustData[RegisterResponse](t, env)

	rec, env = doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:   regResp.StreamID,
		OwnerToken: "wrong-token",
	})
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
	if env.Error == nil || env.Error.Code != CodeForbidden {
		t.Fatalf("error code = %+v", env.Error)
	}
}

func TestHeartbeatNotFound(t *testing.T) {
	h, _ := newTestServer(t)
	rec, _ := doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:   "00000000-0000-0000-0000-000000000000",
		OwnerToken: "x",
	})
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

func TestPatch(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	reg := mustData[RegisterResponse](t, env)

	newShader := "crt/easymode.glslp"
	pwReq := true
	rec, _ = doJSON(t, h, "PATCH", "/streams/"+reg.StreamID, PatchRequest{
		OwnerToken:       reg.OwnerToken,
		Shader:           &newShader,
		PasswordRequired: &pwReq,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", rec.Code)
	}

	// Read it back.
	rec, env = doJSON(t, h, "GET", "/streams/"+reg.StreamID, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("get after patch: %d", rec.Code)
	}
	view := mustData[StreamView](t, env)
	if view.Shader != newShader || !view.PasswordRequired {
		t.Fatalf("patch didn't apply: %+v", view)
	}
}

func TestPatchForbidden(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	reg := mustData[RegisterResponse](t, env)

	shader := "x"
	rec, _ = doJSON(t, h, "PATCH", "/streams/"+reg.StreamID, PatchRequest{
		OwnerToken: "bad",
		Shader:     &shader,
	})
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status = %d, want 403", rec.Code)
	}
}

func TestDelete(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	reg := mustData[RegisterResponse](t, env)

	rec, _ = doJSON(t, h, "DELETE", "/streams/"+reg.StreamID, DeleteRequest{OwnerToken: reg.OwnerToken})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("delete status = %d", rec.Code)
	}
	// Re-delete is idempotent.
	rec, _ = doJSON(t, h, "DELETE", "/streams/"+reg.StreamID, DeleteRequest{OwnerToken: reg.OwnerToken})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("idempotent delete = %d", rec.Code)
	}
}

func TestListAndDetail(t *testing.T) {
	h, _ := newTestServer(t)

	// Register three with different names so list sort + search both
	// have something to bite on.
	for _, name := range []string{"Alpha CRT", "Beta NTSC", "Gamma Plain"} {
		req := validRegisterReq()
		req.Name = name
		rec, _ := doJSON(t, h, "POST", "/register", req)
		if rec.Code != http.StatusCreated {
			t.Fatalf("register %q: %d", name, rec.Code)
		}
	}

	rec, env := doJSON(t, h, "GET", "/streams", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("list status = %d", rec.Code)
	}
	list := mustData[ListResponse](t, env)
	if list.TotalCount != 3 {
		t.Fatalf("totalCount = %d, want 3", list.TotalCount)
	}
	if len(list.Streams) != 3 {
		t.Fatalf("len(streams) = %d, want 3", len(list.Streams))
	}
	// owner_token must not appear in serialised StreamView.
	b, _ := json.Marshal(list)
	if strings.Contains(string(b), "ownerToken") || strings.Contains(string(b), "owner_token") {
		t.Fatal("list response leaked ownerToken field")
	}

	rec, env = doJSON(t, h, "GET", "/streams?q=crt", nil)
	list = mustData[ListResponse](t, env)
	if list.TotalCount != 1 {
		t.Fatalf("filtered totalCount = %d, want 1", list.TotalCount)
	}

	rec, _ = doJSON(t, h, "GET", "/streams?sort=bogus", nil)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("bogus sort status = %d, want 400", rec.Code)
	}
}

func TestGetNotFound(t *testing.T) {
	h, _ := newTestServer(t)
	rec, _ := doJSON(t, h, "GET", "/streams/00000000-0000-0000-0000-000000000000", nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

func TestReport(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	reg := mustData[RegisterResponse](t, env)

	rec, _ = doJSON(t, h, "POST", "/streams/"+reg.StreamID+"/report", ReportRequest{
		Reason: "spam",
	})
	if rec.Code != http.StatusAccepted {
		t.Fatalf("report status = %d, want 202", rec.Code)
	}
}

func TestRateLimit429OnRegister(t *testing.T) {
	// Build a server with a tight register limiter so the 6th attempt
	// fires the 429 path without polluting the other tests.
	st, err := store.Open(filepath.Join(t.TempDir(), "r.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })

	srv := &Server{
		Logger:        slog.New(slog.NewTextHandler(io.Discard, nil)),
		Store:         st,
		TTL:           2 * time.Minute,
		LimitRegister: ratelimit.New(ratelimit.Config{Rate: 0.0001, Capacity: 2}),
	}
	h := srv.Routes()

	// Two register attempts pass.
	for i := 0; i < 2; i++ {
		rec := httptest.NewRecorder()
		body, _ := json.Marshal(validRegisterReq())
		req := httptest.NewRequest("POST", "/register", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.RemoteAddr = "1.1.1.1:1000"
		h.ServeHTTP(rec, req)
		if rec.Code != http.StatusCreated {
			t.Fatalf("request %d: status = %d, want 201", i, rec.Code)
		}
	}

	// Third gets rate-limited.
	rec := httptest.NewRecorder()
	body, _ := json.Marshal(validRegisterReq())
	req := httptest.NewRequest("POST", "/register", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "1.1.1.1:1000"
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusTooManyRequests {
		t.Fatalf("third request: status = %d, want 429", rec.Code)
	}
	ra := rec.Header().Get("Retry-After")
	if n, err := strconv.Atoi(ra); err != nil || n < 1 {
		t.Fatalf("Retry-After = %q, want positive int", ra)
	}
	var env Envelope
	if err := json.NewDecoder(rec.Body).Decode(&env); err != nil {
		t.Fatalf("decode 429 body: %v", err)
	}
	if env.Error == nil || env.Error.Code != "rate_limited" {
		t.Fatalf("error envelope = %+v, want rate_limited", env.Error)
	}

	// Different IP gets its own bucket → still works.
	rec = httptest.NewRecorder()
	body, _ = json.Marshal(validRegisterReq())
	req = httptest.NewRequest("POST", "/register", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "2.2.2.2:1000"
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("different-IP register: status = %d, want 201", rec.Code)
	}
}

func TestEndToEndRegisterHeartbeatList(t *testing.T) {
	// Simulates a real host: register, heartbeat with client count,
	// browse list shows the updated count.
	h, _ := newTestServer(t)

	_, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	reg := mustData[RegisterResponse](t, env)

	cc := 5
	doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:    reg.StreamID,
		OwnerToken:  reg.OwnerToken,
		ClientCount: &cc,
	})

	_, env = doJSON(t, h, "GET", "/streams", nil)
	list := mustData[ListResponse](t, env)
	if len(list.Streams) != 1 || list.Streams[0].ClientCount != 5 {
		t.Fatalf("heartbeat clientCount didn't surface in list: %+v", list)
	}
}
