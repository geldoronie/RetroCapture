package api

// Shared utilities used by every endpoint test file. Lives in its
// own _test.go so the per-endpoint files stay focused on their
// endpoint.

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

// newTestServer wires a real store + the public Routes() handler so
// each test exercises the full stack. The DB lives under t.TempDir
// so tests are independent and parallelisable in spirit even though
// we don't t.Parallel() (SQLite per-test is already isolated).
//
// No rate limiters are installed by default — every test that isn't
// specifically about rate limiting gets unbounded calls. Tests that
// need limiters install them explicitly.
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

// newTestServerWithLimiter is the same but with a single named
// limiter wired. Used by ratelimit-specific tests so they don't have
// to construct a full Server themselves.
func newTestServerWithLimiter(t *testing.T, field string, l *ratelimit.Limiter) (http.Handler, *store.Store) {
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
	switch field {
	case "register":
		srv.LimitRegister = l
	case "heartbeat":
		srv.LimitHeartbeat = l
	case "patch":
		srv.LimitPatch = l
	case "list":
		srv.LimitList = l
	case "report":
		srv.LimitReport = l
	default:
		t.Fatalf("unknown limiter field %q", field)
	}
	return srv.Routes(), st
}

// doJSON issues a request and decodes the response envelope. body can
// be nil for GET / DELETE-with-no-body cases. RemoteAddr is forced
// so handlers that look at it (publicIp recording, rate-limit
// keying) have a stable input.
func doJSON(t *testing.T, h http.Handler, method, path string, body any) (*httptest.ResponseRecorder, Envelope) {
	t.Helper()
	return doJSONFrom(t, h, method, path, body, "9.9.9.9:54321")
}

func doJSONFrom(t *testing.T, h http.Handler, method, path string, body any, remoteAddr string) (*httptest.ResponseRecorder, Envelope) {
	t.Helper()
	var buf bytes.Buffer
	if body != nil {
		if err := json.NewEncoder(&buf).Encode(body); err != nil {
			t.Fatalf("encode body: %v", err)
		}
	}
	req := httptest.NewRequest(method, path, &buf)
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = remoteAddr
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	var env Envelope
	if rec.Body.Len() > 0 {
		_ = json.NewDecoder(rec.Body).Decode(&env)
	}
	return rec, env
}

// doRaw lets tests send custom byte payloads (e.g. malformed JSON).
func doRaw(t *testing.T, h http.Handler, method, path string, raw []byte) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, bytes.NewReader(raw))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "9.9.9.9:54321"
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

// mustData re-marshals env.Data into the requested concrete type. It's
// the most ergonomic way to walk back through json.RawMessage-style
// `any` payloads without reaching for reflect.
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

// validRegisterReq returns a RegisterRequest that passes validation.
// Tests mutate one field at a time to exercise rejection paths.
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

// registerOne creates a stream and returns its ID + owner token so
// tests for /heartbeat, /patch, etc. don't have to repeat the
// register dance.
func registerOne(t *testing.T, h http.Handler) (streamID, ownerToken string) {
	t.Helper()
	rec, env := doJSON(t, h, "POST", "/register", validRegisterReq())
	if rec.Code != http.StatusCreated {
		t.Fatalf("register failed: status=%d env=%+v", rec.Code, env)
	}
	r := mustData[RegisterResponse](t, env)
	return r.StreamID, r.OwnerToken
}
