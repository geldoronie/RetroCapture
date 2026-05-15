package api

// Tests for GET /health — liveness / readiness probe.

import (
	"net/http"
	"testing"
)

func TestHealth_OK(t *testing.T) {
	h, _ := newTestServer(t)
	rec, env := doJSON(t, h, "GET", "/health", nil)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if env.Error != nil {
		t.Fatalf("env.Error = %+v, want nil", env.Error)
	}
	got := mustData[HealthResponse](t, env)
	if got.Status != "ok" {
		t.Fatalf("status field = %q, want \"ok\"", got.Status)
	}
	if got.ProtocolVersion != ProtocolVersion {
		t.Fatalf("protocol_version = %d, want %d", got.ProtocolVersion, ProtocolVersion)
	}
}

// Health is registered as "GET /health" specifically. Other methods
// against the same path should be rejected by net/http's mux
// matching (404 or 405 depending on Go version's mux behaviour).
func TestHealth_WrongMethod(t *testing.T) {
	h, _ := newTestServer(t)
	rec, _ := doJSON(t, h, "POST", "/health", nil)
	if rec.Code == http.StatusOK {
		t.Fatalf("POST /health returned 200; expected 4xx (got %d)", rec.Code)
	}
	if rec.Code < 400 || rec.Code >= 500 {
		t.Fatalf("POST /health status = %d, want 4xx", rec.Code)
	}
}

// /health is documented as unlimited (no rate limit). Smoke that 100
// rapid calls all succeed.
func TestHealth_Unlimited(t *testing.T) {
	h, _ := newTestServer(t)
	for i := 0; i < 100; i++ {
		rec, _ := doJSON(t, h, "GET", "/health", nil)
		if rec.Code != http.StatusOK {
			t.Fatalf("call %d status = %d, want 200", i, rec.Code)
		}
	}
}
