// Package api wires HTTP handlers for the directory service.
// At Phase 1 bootstrap this is just the health endpoint; the
// /register, /heartbeat, /streams, etc. handlers land incrementally
// as #49 Phase 1 progresses.
package api

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"time"
)

// Server holds the dependencies the HTTP handlers need.
type Server struct {
	Logger *slog.Logger
}

// Routes returns the http.Handler the service should serve from.
// Every endpoint is registered here so the surface is auditable in
// one place.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.handleHealth)
	return s.withRequestLogging(mux)
}

// withRequestLogging wraps the mux so every request emits a single
// structured log line. Goes here rather than in middleware.go to keep
// the Phase 1 skeleton minimal; can move once we have more than one
// piece of middleware.
func (s *Server) withRequestLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rw := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rw, r)
		s.Logger.Info("http_request",
			"method", r.Method,
			"path", r.URL.Path,
			"status", rw.status,
			"duration_ms", time.Since(start).Milliseconds(),
			"remote", r.RemoteAddr,
		)
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

// handleHealth is the liveness probe. Returns 200 with a tiny JSON
// payload so anything probing the service can also check the
// content-type round-trip works.
func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// writeJSON is the canonical response writer. All handlers will use
// this so the response envelope stays consistent.
func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	// Errors here mean the client disconnected mid-write — nothing
	// useful to do with the result, so we drop it.
	_ = json.NewEncoder(w).Encode(body)
}
