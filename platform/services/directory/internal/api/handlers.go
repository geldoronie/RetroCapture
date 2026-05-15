// Package api wires HTTP handlers for the directory service. See
// docs/DIRECTORY_PROTOCOL.md for the wire format these implement.
//
// Layering:
//
//   - This package only knows about HTTP and the wire types.
//   - It calls into internal/store for persistence; it does not touch
//     SQL directly.
//   - It does not know about the reaper or rate limiter — those are
//     separate goroutines / middleware.
package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
	"github.com/google/uuid"
)

const (
	// ProtocolVersion is the wire-format version advertised at /health.
	// Bump on backwards-incompatible changes; the service must
	// continue accepting the previous version for at least one release.
	ProtocolVersion = 1

	// maxBodyBytes caps every JSON request body so a malicious client
	// can't make us OOM by streaming an endless body.
	maxBodyBytes = 64 * 1024

	// Field length limits — keeps display columns sane and prevents
	// abuse via 100-MB-name registrations.
	maxNameLen         = 120
	maxHostNicknameLen = 40
	maxShaderLen       = 200
	maxEndpointLen     = 500
	maxReasonLen       = 1000
	maxContactLen      = 200
)

// Server holds the dependencies the HTTP handlers need.
//
// The Limit* fields are optional. A nil limiter means the endpoint is
// unlimited (which is what /health and DELETE want by spec). main.go
// wires real limiters with the production rates; tests can wire
// tight ones to exercise the 429 path.
type Server struct {
	Logger *slog.Logger
	Store  *store.Store
	TTL    time.Duration

	LimitRegister  *ratelimit.Limiter // POST /register
	LimitHeartbeat *ratelimit.Limiter // POST /heartbeat
	LimitPatch     *ratelimit.Limiter // PATCH /streams/{id}
	LimitList      *ratelimit.Limiter // GET /streams and GET /streams/{id}
	LimitReport    *ratelimit.Limiter // POST /streams/{id}/report
}

// Routes returns the http.Handler the service should serve from.
// Every endpoint is registered here so the surface is auditable in
// one place. Each entry visibly states its rate-limit policy via the
// `ratelimit.Wrap` call — endpoints without a limiter (nil) pass
// through unchanged.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.handleHealth) // unlimited

	mux.HandleFunc("POST /register",
		ratelimit.Wrap(s.LimitRegister, ratelimit.ClientIPKey, s.handleRegister))

	mux.HandleFunc("POST /heartbeat",
		ratelimit.Wrap(s.LimitHeartbeat, ratelimit.ClientIPKey, s.handleHeartbeat))

	mux.HandleFunc("GET /streams",
		ratelimit.Wrap(s.LimitList, ratelimit.ClientIPKey, s.handleListStreams))

	mux.HandleFunc("GET /streams/{id}",
		ratelimit.Wrap(s.LimitList, ratelimit.ClientIPKey, s.handleGetStream))

	mux.HandleFunc("PATCH /streams/{id}",
		ratelimit.Wrap(s.LimitPatch, ratelimit.ClientIPKey, s.handlePatchStream))

	// DELETE is intentionally unlimited per the spec — the owner
	// token gates abuse, and we want disconnect to always succeed.
	mux.HandleFunc("DELETE /streams/{id}", s.handleDeleteStream)

	mux.HandleFunc("POST /streams/{id}/report",
		ratelimit.Wrap(s.LimitReport, ratelimit.ClientIPKey, s.handleReportStream))

	return s.withRequestLogging(mux)
}

// --- middleware ---

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
			"remote", clientIP(r),
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

// --- handlers ---

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, HealthResponse{
		Status:          "ok",
		ProtocolVersion: ProtocolVersion,
	})
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	var req RegisterRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}
	if err := validateRegister(&req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}

	streamID := uuid.NewString()
	ownerToken, err := newToken()
	if err != nil {
		s.Logger.Error("token_generation_failed", "err", err)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to generate owner token")
		return
	}

	now := time.Now().UTC()
	expiresAt := now.Add(s.TTL)
	st := store.Stream{
		StreamID:         streamID,
		OwnerToken:       ownerToken,
		Name:             req.Name,
		HostNickname:     req.HostNickname,
		Shader:           req.Shader,
		ResolutionW:      req.Resolution.W,
		ResolutionH:      req.Resolution.H,
		FPS:              req.FPS,
		Codec:            req.Codec,
		PasswordRequired: req.PasswordRequired,
		Endpoint:         req.Endpoint,
		EndpointMode:     req.EndpointMode,
		ClientCount:      0,
		PublicIP:         clientIP(r),
		Version:          req.Version,
		RegisteredAt:     now,
		LastHeartbeatAt:  now,
		ExpiresAt:        expiresAt,
	}
	if err := s.Store.Insert(r.Context(), st); err != nil {
		s.Logger.Error("register_insert_failed", "err", err)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to persist")
		return
	}

	writeJSON(w, http.StatusCreated, RegisterResponse{
		StreamID:   streamID,
		OwnerToken: ownerToken,
		ExpiresAt:  expiresAt.Format(time.RFC3339),
	})
}

func (s *Server) handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	var req HeartbeatRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}
	if req.StreamID == "" || req.OwnerToken == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "streamId and ownerToken are required")
		return
	}
	if req.ClientCount != nil && (*req.ClientCount < 0 || *req.ClientCount > 100_000) {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "clientCount out of range")
		return
	}

	newExpiry := time.Now().UTC().Add(s.TTL)
	err := s.Store.Heartbeat(r.Context(), req.StreamID, req.OwnerToken, req.ClientCount, newExpiry)
	switch {
	case errors.Is(err, store.ErrNotFound):
		writeError(w, http.StatusNotFound, CodeNotFound, "no entry with that streamId")
		return
	case errors.Is(err, store.ErrForbidden):
		writeError(w, http.StatusForbidden, CodeForbidden, "owner token mismatch")
		return
	case err != nil:
		s.Logger.Error("heartbeat_failed", "err", err, "stream_id", req.StreamID)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to persist heartbeat")
		return
	}

	writeJSON(w, http.StatusOK, HeartbeatResponse{
		ExpiresAt: newExpiry.Format(time.RFC3339),
	})
}

func (s *Server) handlePatchStream(w http.ResponseWriter, r *http.Request) {
	streamID := r.PathValue("id")
	if streamID == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "missing streamId in path")
		return
	}

	var req PatchRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}
	if req.OwnerToken == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "ownerToken is required")
		return
	}
	if err := validatePatch(&req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}

	patch := store.Patch{
		Name:             req.Name,
		HostNickname:     req.HostNickname,
		Shader:           req.Shader,
		FPS:              req.FPS,
		Codec:            req.Codec,
		PasswordRequired: req.PasswordRequired,
		Endpoint:         req.Endpoint,
		EndpointMode:     req.EndpointMode,
	}
	if req.Resolution != nil {
		w := req.Resolution.W
		h := req.Resolution.H
		patch.ResolutionW = &w
		patch.ResolutionH = &h
	}

	err := s.Store.ApplyPatch(r.Context(), streamID, req.OwnerToken, patch)
	switch {
	case errors.Is(err, store.ErrNotFound):
		writeError(w, http.StatusNotFound, CodeNotFound, "no entry with that streamId")
		return
	case errors.Is(err, store.ErrForbidden):
		writeError(w, http.StatusForbidden, CodeForbidden, "owner token mismatch")
		return
	case err != nil:
		s.Logger.Error("patch_failed", "err", err, "stream_id", streamID)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to persist patch")
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleDeleteStream(w http.ResponseWriter, r *http.Request) {
	streamID := r.PathValue("id")
	if streamID == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "missing streamId in path")
		return
	}
	var req DeleteRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}
	if req.OwnerToken == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "ownerToken is required")
		return
	}

	err := s.Store.Delete(r.Context(), streamID, req.OwnerToken)
	switch {
	case errors.Is(err, store.ErrForbidden):
		writeError(w, http.StatusForbidden, CodeForbidden, "owner token mismatch")
		return
	case err != nil:
		s.Logger.Error("delete_failed", "err", err, "stream_id", streamID)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to delete")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleListStreams(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()

	limit := 100
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 || n > 500 {
			writeError(w, http.StatusBadRequest, CodeInvalidRequest, "limit must be 1..500")
			return
		}
		limit = n
	}

	opts := store.ListOptions{
		Sort:  q.Get("sort"),
		Query: q.Get("q"),
		Limit: limit,
	}
	streams, total, err := s.Store.List(r.Context(), opts)
	if err != nil {
		// Validation errors from the store come through as plain errors
		// (e.g. "invalid sort"). Anything we couldn't map up front is
		// a 400 here.
		if strings.HasPrefix(err.Error(), "invalid sort") {
			writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
			return
		}
		s.Logger.Error("list_failed", "err", err)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to list streams")
		return
	}

	views := make([]StreamView, 0, len(streams))
	for _, st := range streams {
		views = append(views, toView(st))
	}
	writeJSON(w, http.StatusOK, ListResponse{Streams: views, TotalCount: total})
}

func (s *Server) handleGetStream(w http.ResponseWriter, r *http.Request) {
	streamID := r.PathValue("id")
	if streamID == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "missing streamId in path")
		return
	}
	st, err := s.Store.Get(r.Context(), streamID)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, CodeNotFound, "no entry with that streamId")
		return
	}
	if err != nil {
		s.Logger.Error("get_failed", "err", err, "stream_id", streamID)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to fetch stream")
		return
	}
	writeJSON(w, http.StatusOK, toView(st))
}

func (s *Server) handleReportStream(w http.ResponseWriter, r *http.Request) {
	streamID := r.PathValue("id")
	if streamID == "" {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "missing streamId in path")
		return
	}
	var req ReportRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, err.Error())
		return
	}
	if len(req.Reason) > maxReasonLen {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "reason too long")
		return
	}
	if len(req.ReporterContact) > maxContactLen {
		writeError(w, http.StatusBadRequest, CodeInvalidRequest, "reporterContact too long")
		return
	}

	if err := s.Store.InsertReport(r.Context(), streamID, clientIP(r), req.Reason, req.ReporterContact); err != nil {
		s.Logger.Error("report_failed", "err", err, "stream_id", streamID)
		writeError(w, http.StatusInternalServerError, CodeInternal, "failed to record report")
		return
	}
	s.Logger.Info("report_received",
		"stream_id", streamID,
		"reporter_ip", clientIP(r),
		"reason_len", len(req.Reason),
	)
	w.WriteHeader(http.StatusAccepted)
}

// --- validation ---

func validateRegister(req *RegisterRequest) error {
	if req.Name = strings.TrimSpace(req.Name); req.Name == "" {
		return fmt.Errorf("name is required")
	}
	if len(req.Name) > maxNameLen {
		return fmt.Errorf("name too long (max %d)", maxNameLen)
	}
	if len(req.HostNickname) > maxHostNicknameLen {
		return fmt.Errorf("hostNickname too long (max %d)", maxHostNicknameLen)
	}
	if len(req.Shader) > maxShaderLen {
		return fmt.Errorf("shader too long (max %d)", maxShaderLen)
	}
	if req.Resolution.W <= 0 || req.Resolution.W > 16384 {
		return fmt.Errorf("resolution.w out of range")
	}
	if req.Resolution.H <= 0 || req.Resolution.H > 16384 {
		return fmt.Errorf("resolution.h out of range")
	}
	if req.FPS <= 0 || req.FPS > 1000 {
		return fmt.Errorf("fps out of range")
	}
	if req.Codec != "h264" && req.Codec != "h265" {
		return fmt.Errorf("codec must be h264 or h265")
	}
	if req.Endpoint = strings.TrimSpace(req.Endpoint); req.Endpoint == "" {
		return fmt.Errorf("endpoint is required")
	}
	if len(req.Endpoint) > maxEndpointLen {
		return fmt.Errorf("endpoint too long (max %d)", maxEndpointLen)
	}
	if !isValidEndpointMode(req.EndpointMode) {
		return fmt.Errorf("endpointMode must be direct, tunnel-cloudflare or custom")
	}
	return nil
}

func validatePatch(req *PatchRequest) error {
	if req.Name != nil {
		v := strings.TrimSpace(*req.Name)
		if v == "" || len(v) > maxNameLen {
			return fmt.Errorf("name must be 1..%d chars", maxNameLen)
		}
		*req.Name = v
	}
	if req.HostNickname != nil && len(*req.HostNickname) > maxHostNicknameLen {
		return fmt.Errorf("hostNickname too long (max %d)", maxHostNicknameLen)
	}
	if req.Shader != nil && len(*req.Shader) > maxShaderLen {
		return fmt.Errorf("shader too long (max %d)", maxShaderLen)
	}
	if req.Resolution != nil {
		if req.Resolution.W <= 0 || req.Resolution.W > 16384 ||
			req.Resolution.H <= 0 || req.Resolution.H > 16384 {
			return fmt.Errorf("resolution out of range")
		}
	}
	if req.FPS != nil && (*req.FPS <= 0 || *req.FPS > 1000) {
		return fmt.Errorf("fps out of range")
	}
	if req.Codec != nil && *req.Codec != "h264" && *req.Codec != "h265" {
		return fmt.Errorf("codec must be h264 or h265")
	}
	if req.Endpoint != nil {
		v := strings.TrimSpace(*req.Endpoint)
		if v == "" || len(v) > maxEndpointLen {
			return fmt.Errorf("endpoint must be 1..%d chars", maxEndpointLen)
		}
		*req.Endpoint = v
	}
	if req.EndpointMode != nil && !isValidEndpointMode(*req.EndpointMode) {
		return fmt.Errorf("endpointMode must be direct, tunnel-cloudflare or custom")
	}
	return nil
}

func isValidEndpointMode(m string) bool {
	switch m {
	case "direct", "tunnel-cloudflare", "custom":
		return true
	}
	return false
}

// --- helpers ---

func decodeJSON(r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(nil, r.Body, maxBodyBytes)
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		if errors.Is(err, io.EOF) {
			return fmt.Errorf("request body is empty")
		}
		return fmt.Errorf("invalid JSON: %v", err)
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(Envelope{Data: data, Error: nil})
}

func writeError(w http.ResponseWriter, status int, code, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(Envelope{Data: nil, Error: &APIError{Code: code, Message: msg}})
}

// newToken returns a 32-byte cryptographically-random hex string. Used
// as the per-stream owner token. crypto/rand never panics on Linux
// once the kernel CSPRNG is seeded (it always is on a running system).
func newToken() (string, error) {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(b[:]), nil
}

// clientIP extracts the request's source IP without the port. Falls
// back to the raw RemoteAddr if the host:port split fails. We do NOT
// honour X-Forwarded-For at this layer — the cloudflared / reverse
// proxy in front of the directory is responsible for the source IP
// it presents to us.
func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func toView(st store.Stream) StreamView {
	return StreamView{
		StreamID:         st.StreamID,
		Name:             st.Name,
		HostNickname:     st.HostNickname,
		Shader:           st.Shader,
		Resolution:       Resolution{W: st.ResolutionW, H: st.ResolutionH},
		FPS:              st.FPS,
		Codec:            st.Codec,
		PasswordRequired: st.PasswordRequired,
		Endpoint:         st.Endpoint,
		EndpointMode:     st.EndpointMode,
		ClientCount:      st.ClientCount,
		PublicIP:         st.PublicIP,
		Version:          st.Version,
		RegisteredAt:     st.RegisteredAt.Format(time.RFC3339),
		LastHeartbeatAt:  st.LastHeartbeatAt.Format(time.RFC3339),
		ExpiresAt:        st.ExpiresAt.Format(time.RFC3339),
	}
}

// silence unused warning in case context import gets pruned during edits
var _ context.Context = nil
