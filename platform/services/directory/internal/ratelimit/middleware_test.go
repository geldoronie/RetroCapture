package ratelimit

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
)

func TestWrapNilPassesThrough(t *testing.T) {
	called := false
	h := Wrap(nil, ClientIPKey, func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.WriteHeader(http.StatusOK)
	})
	rec := httptest.NewRecorder()
	h(rec, httptest.NewRequest("GET", "/", nil))
	if !called {
		t.Fatal("nil limiter should be a no-op wrap")
	}
	if rec.Code != 200 {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
}

func TestWrapAllows(t *testing.T) {
	l := New(Config{Rate: 1, Capacity: 5})
	called := 0
	h := Wrap(l, ClientIPKey, func(w http.ResponseWriter, r *http.Request) {
		called++
		w.WriteHeader(http.StatusNoContent)
	})
	for i := 0; i < 5; i++ {
		rec := httptest.NewRecorder()
		req := httptest.NewRequest("GET", "/", nil)
		req.RemoteAddr = "1.2.3.4:1000"
		h(rec, req)
		if rec.Code != http.StatusNoContent {
			t.Fatalf("req %d status = %d, want 204", i, rec.Code)
		}
	}
	if called != 5 {
		t.Fatalf("inner handler called %d times, want 5", called)
	}
}

func TestWrap429EnvelopeAndHeader(t *testing.T) {
	// Tight bucket: cap 1, refill so slow the 2nd request denies.
	l := New(Config{Rate: 0.01, Capacity: 1})
	called := 0
	h := Wrap(l, ClientIPKey, func(w http.ResponseWriter, r *http.Request) {
		called++
		w.WriteHeader(http.StatusOK)
	})
	// First passes.
	rec := httptest.NewRecorder()
	req := httptest.NewRequest("GET", "/", nil)
	req.RemoteAddr = "9.9.9.9:1000"
	h(rec, req)
	if rec.Code != 200 {
		t.Fatalf("first req status = %d, want 200", rec.Code)
	}
	// Second denied.
	rec = httptest.NewRecorder()
	req = httptest.NewRequest("GET", "/", nil)
	req.RemoteAddr = "9.9.9.9:1000"
	h(rec, req)
	if rec.Code != http.StatusTooManyRequests {
		t.Fatalf("second req status = %d, want 429", rec.Code)
	}
	if called != 1 {
		t.Fatalf("inner handler should have been called exactly once, got %d", called)
	}

	ra := rec.Header().Get("Retry-After")
	if ra == "" {
		t.Fatal("missing Retry-After on 429")
	}
	if n, err := strconv.Atoi(ra); err != nil || n < 1 {
		t.Fatalf("Retry-After = %q, want positive integer seconds", ra)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Fatalf("Content-Type = %q, want application/json", ct)
	}

	var env struct {
		Data  any `json:"data"`
		Error *struct {
			Code    string `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.NewDecoder(rec.Body).Decode(&env); err != nil {
		t.Fatalf("body decode: %v", err)
	}
	if env.Error == nil || env.Error.Code != "rate_limited" {
		t.Fatalf("error envelope = %+v, want rate_limited", env.Error)
	}
}

func TestClientIPKey(t *testing.T) {
	req := httptest.NewRequest("GET", "/", nil)
	req.RemoteAddr = "1.2.3.4:54321"
	if got := ClientIPKey(req); got != "1.2.3.4" {
		t.Fatalf("ClientIPKey = %q, want 1.2.3.4", got)
	}
	// Fallback when host:port split fails.
	req.RemoteAddr = "bogus"
	if got := ClientIPKey(req); got != "bogus" {
		t.Fatalf("fallback ClientIPKey = %q, want bogus", got)
	}
}
