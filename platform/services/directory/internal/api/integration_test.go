package api

// Cross-endpoint scenarios. These tests exercise more than one
// endpoint per case — register + heartbeat + list, register +
// rate-limit, etc. Anything that's "did endpoint X do its single
// thing correctly" lives in its own per-endpoint _test.go.

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

// Full register → heartbeat (with clientCount) → list visibility flow.
// This is the canonical "happy path" used by a real RetroCapture host.
func TestE2E_RegisterHeartbeatList(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	cc := 5
	doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID:    id,
		OwnerToken:  tok,
		ClientCount: &cc,
	})

	_, env := doJSON(t, h, "GET", "/streams", nil)
	list := mustData[ListResponse](t, env)
	if len(list.Streams) != 1 || list.Streams[0].ClientCount != 5 {
		t.Fatalf("heartbeat clientCount didn't surface in list: %+v", list)
	}
	if list.Streams[0].StreamID != id {
		t.Fatalf("streamId mismatch: got %q want %q", list.Streams[0].StreamID, id)
	}
}

// Register from one IP, hit the bucket, verify a different IP isn't
// affected. Bigger integration cousin of the unit-level
// ratelimit/middleware_test.
func TestE2E_RateLimit429OnRegister(t *testing.T) {
	st, err := store.Open(filepath.Join(t.TempDir(), "rl.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })

	// Capacity 2, very slow refill — third call in a row from the
	// same IP fires the 429.
	srv := &Server{
		Logger:        slog.New(slog.NewTextHandler(io.Discard, nil)),
		Store:         st,
		TTL:           2 * time.Minute,
		LimitRegister: ratelimit.New(ratelimit.Config{Rate: 0.0001, Capacity: 2}),
	}
	h := srv.Routes()

	// Two registers from 1.1.1.1 succeed.
	for i := 0; i < 2; i++ {
		body, _ := json.Marshal(validRegisterReq())
		req := httptest.NewRequest("POST", "/register", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.RemoteAddr = "1.1.1.1:1000"
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		if rec.Code != http.StatusCreated {
			t.Fatalf("request %d: status = %d, want 201", i, rec.Code)
		}
	}

	// Third from same IP → 429 + Retry-After + rate_limited envelope.
	body, _ := json.Marshal(validRegisterReq())
	req := httptest.NewRequest("POST", "/register", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "1.1.1.1:1000"
	rec := httptest.NewRecorder()
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

	// Different IP gets its own bucket — register still works.
	body, _ = json.Marshal(validRegisterReq())
	req = httptest.NewRequest("POST", "/register", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.RemoteAddr = "2.2.2.2:1000"
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("different-IP register: status = %d, want 201", rec.Code)
	}
}

// Owner-token leakage check across the whole read surface. Register a
// stream, fetch every read endpoint that could expose it, scan for
// the literal token value in the JSON body. If any path slips, this
// test catches it.
func TestE2E_OwnerTokenNeverLeaks(t *testing.T) {
	h, _ := newTestServer(t)
	id, token := registerOne(t, h)

	endpoints := []struct {
		method, path string
	}{
		{"GET", "/streams"},
		{"GET", "/streams/" + id},
	}
	for _, e := range endpoints {
		t.Run(e.method+" "+e.path, func(t *testing.T) {
			rec, env := doJSON(t, h, e.method, e.path, nil)
			if rec.Code != http.StatusOK {
				t.Fatalf("status = %d", rec.Code)
			}
			b, _ := json.Marshal(env)
			if got := string(b); contains(got, token) {
				t.Fatalf("owner token leaked in body: %s", got)
			}
			if contains(string(b), "ownerToken") || contains(string(b), "owner_token") {
				t.Fatalf("ownerToken field key present in body: %s", string(b))
			}
		})
	}
}

// Full lifecycle: register → patch shader → heartbeat → delete →
// confirm gone. Hits five endpoints in sequence and verifies each
// one's effect lands.
func TestE2E_FullLifecycle(t *testing.T) {
	h, _ := newTestServer(t)
	id, tok := registerOne(t, h)

	// patch
	newShader := "lifecycle/patched.glslp"
	rec, _ := doJSON(t, h, "PATCH", "/streams/"+id, PatchRequest{
		OwnerToken: tok, Shader: &newShader,
	})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("patch: %d", rec.Code)
	}

	// heartbeat
	cc := 11
	rec, _ = doJSON(t, h, "POST", "/heartbeat", HeartbeatRequest{
		StreamID: id, OwnerToken: tok, ClientCount: &cc,
	})
	if rec.Code != http.StatusOK {
		t.Fatalf("heartbeat: %d", rec.Code)
	}

	// detail should reflect both updates
	_, env := doJSON(t, h, "GET", "/streams/"+id, nil)
	v := mustData[StreamView](t, env)
	if v.Shader != newShader {
		t.Fatalf("shader = %q after patch, want %q", v.Shader, newShader)
	}
	if v.ClientCount != cc {
		t.Fatalf("clientCount = %d after heartbeat, want %d", v.ClientCount, cc)
	}

	// delete
	rec, _ = doJSON(t, h, "DELETE", "/streams/"+id, DeleteRequest{OwnerToken: tok})
	if rec.Code != http.StatusNoContent {
		t.Fatalf("delete: %d", rec.Code)
	}

	// gone
	rec, _ = doJSON(t, h, "GET", "/streams/"+id, nil)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("get after delete: %d, want 404", rec.Code)
	}
	rec, env = doJSON(t, h, "GET", "/streams", nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("list: %d", rec.Code)
	}
	if list := mustData[ListResponse](t, env); list.TotalCount != 0 {
		t.Fatalf("list totalCount after delete = %d, want 0", list.TotalCount)
	}
}

// Tiny strings.Contains-equivalent that avoids the import (kept local
// so the test file's imports stay focused). For the small payloads
// these tests look at, the difference is irrelevant.
func contains(haystack, needle string) bool {
	if len(needle) == 0 {
		return true
	}
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
