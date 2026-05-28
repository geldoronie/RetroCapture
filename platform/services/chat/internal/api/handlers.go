// HTTP handlers for the chat service. The WebSocket upgrade lives in
// ws.go; this file covers the REST endpoints documented in
// docs/CHAT_PROTOCOL.md (`/health`, `/rooms/:id`, `/rooms/:id/history`,
// `/rooms/by-stream/:id`).
package api

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/room"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/store"
	"github.com/google/uuid"
)

// Server holds the dependencies the HTTP + WS handlers need. main.go
// wires the concrete instances; tests can wire fakes.
type Server struct {
	Logger   *slog.Logger
	Store    *store.Store
	Registry *room.Registry

	// Per-WebSocket-connection post-rate cap (window = 10 s, max = N).
	// Constructed in main.go with config-supplied N.
	LimitPost *ratelimit.Limiter

	// HelloTimeout is how long the WS upgrade waits for the first
	// `hello` frame before closing the connection.
	HelloTimeout time.Duration

	// TrustProxyHeaders controls source-IP extraction (Cf-Connecting-Ip
	// / X-Forwarded-For etc) — see config.Config.
	TrustProxyHeaders bool
}

// Routes returns the http.Handler the service binds to.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("/health", s.handleHealth)

	// /rooms/* — pattern matching is intentionally manual so we keep
	// the dep on http.ServeMux without pulling chi/gorilla/etc.
	mux.HandleFunc("/rooms/by-stream/", s.handleRoomByStream)
	mux.HandleFunc("/rooms/by-slug/",   s.handleRoomBySlug)
	mux.HandleFunc("/rooms",            s.handleRoomsCollection)
	mux.HandleFunc("/rooms/",           s.handleRoom)

	// WebSocket upgrade — implemented in ws.go on the same Server.
	mux.HandleFunc("/ws", s.handleWebSocket)

	// CORS preflight is wide-open in v0.5; we'll tighten with the
	// directory-account auth pass in v1.
	return withCORS(mux)
}

func withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// --- handlers ------------------------------------------------------

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", "GET only")
		return
	}
	rooms, parts := s.Registry.Stats()
	s.writeData(w, http.StatusOK, healthPayload{
		Status:          "ok",
		ProtocolVersion: ProtocolVersion,
		Rooms:           rooms,
		Participants:    parts,
	})
}

// handleRoomByStream serves GET /rooms/by-stream/<streamId>. Creates
// the linked room if it doesn't exist.
func (s *Server) handleRoomByStream(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", "GET only")
		return
	}
	streamID := strings.TrimPrefix(r.URL.Path, "/rooms/by-stream/")
	if streamID == "" || strings.Contains(streamID, "/") {
		s.writeError(w, http.StatusBadRequest, "bad_request", "missing streamId in path")
		return
	}

	roomID := "r_" + shortID()
	room, created, err := s.Store.GetOrCreateRoomForStream(
		r.Context(), streamID, roomID, "",
	)
	if err != nil {
		s.Logger.Error("get_or_create_room", "err", err, "stream_id", streamID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	s.writeData(w, http.StatusOK, roomByStreamPayload{
		RoomID:  room.ID,
		Created: created,
		Title:   room.Title,
	})
}

// handleRoomBySlug serves GET /rooms/by-slug/<slug> — the standalone-
// room analog of by-stream. 404 if no room owns the slug.
func (s *Server) handleRoomBySlug(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", "GET only")
		return
	}
	slug := strings.TrimPrefix(r.URL.Path, "/rooms/by-slug/")
	if !isValidSlug(slug) {
		s.writeError(w, http.StatusBadRequest, "bad_request",
			"slug must match ^[a-z0-9][a-z0-9-]{1,40}$")
		return
	}
	row, err := s.Store.GetRoomBySlug(r.Context(), slug)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			s.writeError(w, http.StatusNotFound, "room_not_found", "no such slug")
			return
		}
		s.Logger.Error("get_room_by_slug", "err", err, "slug", slug)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	s.writeData(w, http.StatusOK, roomBySlugPayload{
		RoomID: row.ID,
		Slug:   row.Slug,
		Title:  row.Title,
	})
}

// handleRoomsCollection serves POST /rooms (create) and GET /rooms
// (public listing). v0.5 has no authentication; anyone can create
// and the owner field carries the creator's rc_<id> verbatim — v1
// ties ownership to a directory-account signed token.
func (s *Server) handleRoomsCollection(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		s.handleRoomsList(w, r)
	case http.MethodPost:
		s.handleRoomsCreate(w, r)
	default:
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed",
			"GET or POST only")
	}
}

func (s *Server) handleRoomsList(w http.ResponseWriter, r *http.Request) {
	limit := 50
	if v := r.URL.Query().Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err == nil && n > 0 && n <= 200 {
			limit = n
		}
	}
	rooms, err := s.Store.ListPublicRooms(r.Context(), limit)
	if err != nil {
		s.Logger.Error("list_public_rooms", "err", err)
		s.writeError(w, http.StatusInternalServerError, "internal_error",
			"store failure")
		return
	}
	out := make([]listedRoomPayload, 0, len(rooms))
	for _, rm := range rooms {
		count := 0
		if live := s.Registry.Get(rm.ID); live != nil {
			count = live.Count()
		}
		// Stream-linked rooms are gated by live participants — the
		// chat service can't tell whether the stream itself is live
		// (that's the directory service's call), so an empty room
		// means "nobody is currently chatting", which is the closest
		// proxy for "this stream isn't worth surfacing". Standalone
		// rooms are always shown so creators can drop a fresh empty
		// room and tell people about it.
		if rm.Kind == store.RoomKindStreamLinked && count == 0 {
			continue
		}
		out = append(out, listedRoomPayload{
			RoomID:           rm.ID,
			Kind:             string(rm.Kind),
			Slug:             rm.Slug,
			LinkedStreamID:   rm.LinkedStreamID,
			Title:            rm.Title,
			HasPassword:      rm.PasswordHash != "",
			IsStreamRoom:     rm.IsStreamRoom,
			ParticipantCount: count,
			CreatedAtMs:      rm.CreatedAt.UnixMilli(),
		})
	}
	s.writeData(w, http.StatusOK, roomsListPayload{Rooms: out})
}

func (s *Server) handleRoomsCreate(w http.ResponseWriter, r *http.Request) {
	var req createRoomRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.writeError(w, http.StatusBadRequest, "bad_request", "invalid JSON body")
		return
	}

	slug := strings.TrimSpace(strings.ToLower(req.Slug))
	if slug == "" {
		slug = generateSlug()
	} else if !isValidSlug(slug) {
		s.writeError(w, http.StatusBadRequest, "bad_request",
			"slug must match ^[a-z0-9][a-z0-9-]{1,40}$")
		return
	}

	title := strings.TrimSpace(req.Title)
	if len(title) > 120 {
		title = title[:120]
	}
	if title == "" {
		title = slug
	}

	// Optional owner client_id — same shape validator the WS hello
	// uses so creator identity matches the chat protocol's
	// participant_id format. Empty owner is allowed (anonymous room).
	owner := strings.TrimSpace(req.OwnerClientID)
	if owner != "" && !isValidClientID(owner) {
		s.writeError(w, http.StatusBadRequest, "bad_request",
			"owner_client_id must match rc_<lowercase hex>")
		return
	}

	// Password + owner secret: both hashed here so plaintexts never
	// land in storage. Empty plaintext == no gate.
	passHash := ""
	if pw := strings.TrimSpace(req.Password); pw != "" {
		passHash = sha256Hex(pw)
	}
	secretHash := ""
	if sec := strings.TrimSpace(req.OwnerSecret); sec != "" {
		secretHash = sha256Hex(sec)
	}

	listed := true
	if req.Listed != nil {
		listed = *req.Listed
	}

	roomID := "r_" + shortID()
	room, err := s.Store.CreateStandaloneRoom(r.Context(), roomID, slug, title,
		store.StandaloneRoomOptions{
			OwnerClientID:   owner,
			PasswordHash:    passHash,
			OwnerSecretHash: secretHash,
			Listed:          listed,
			IsStreamRoom:    req.IsStreamRoom,
		})
	if err != nil {
		if errors.Is(err, store.ErrSlugTaken) {
			s.writeError(w, http.StatusConflict, "slug_taken", "slug already in use")
			return
		}
		s.Logger.Error("create_standalone_room", "err", err, "slug", slug)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}

	s.writeData(w, http.StatusCreated, createRoomPayload{
		RoomID:        room.ID,
		Slug:          room.Slug,
		Title:         room.Title,
		HasPassword:   room.PasswordHash != "",
		Listed:        room.Listed,
		OwnerClientID: room.OwnerAccountID,
	})
}

// isValidSlug enforces the same shape we documented in
// docs/CHAT_PROTOCOL.md: lowercase alphanumeric + dashes, 2..41 chars,
// must start with a letter or digit (not a dash).
func isValidSlug(s string) bool {
	n := len(s)
	if n < 2 || n > 41 {
		return false
	}
	if s[0] == '-' {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'a' && c <= 'z':
		case c >= '0' && c <= '9':
		case c == '-':
		default:
			return false
		}
	}
	return true
}

// generateSlug returns an 8-char hex slug for server-generated rooms.
// Cheaper to type / share than a UUID; 8 hex chars = 32 bits of
// entropy which is enough for the v0.5 namespace.
func generateSlug() string {
	return shortID()[:8]
}

// sha256Hex returns the hex-encoded SHA-256 of s. Used for hashing
// room passwords before they hit storage — the plaintext never
// leaves this function.
func sha256Hex(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:])
}

// handleRoomPatch serves PATCH /rooms/<roomId>. Same secret-only
// auth as DELETE. v1 only exposes the `listed` flag — title/slug
// stay frozen because the wire history would have stale denorm'd
// copies. The host's RetroCapture client uses this to promote a
// stream chat that was provisioned with listed=false (before the
// streamer-rooms-are-public default landed in #84) without
// having to delete + recreate.
func (s *Server) handleRoomPatch(w http.ResponseWriter, r *http.Request, roomID string) {
	type patchReq struct {
		OwnerSecret string `json:"owner_secret"`
		Listed      *bool  `json:"listed"`
	}
	var req patchReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		s.writeError(w, http.StatusBadRequest, "bad_request", "invalid JSON body")
		return
	}
	row, err := s.Store.GetRoom(r.Context(), roomID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			s.writeError(w, http.StatusNotFound, "room_not_found", "no such room")
			return
		}
		s.Logger.Error("patch_room get", "err", err, "room_id", roomID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	if row.OwnerSecretHash == "" {
		s.writeError(w, http.StatusForbidden, "not_owner",
			"room has no owner_secret on file; cannot be patched")
		return
	}
	if req.OwnerSecret == "" || sha256Hex(req.OwnerSecret) != row.OwnerSecretHash {
		s.writeError(w, http.StatusForbidden, "not_owner",
			"owner_secret required and must match the room")
		return
	}
	if req.Listed != nil {
		if err := s.Store.SetRoomListed(r.Context(), roomID, *req.Listed); err != nil {
			s.Logger.Error("patch_room listed", "err", err, "room_id", roomID)
			s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
			return
		}
	}
	s.writeData(w, http.StatusOK, struct {
		Updated bool `json:"updated"`
	}{Updated: true})
}

// handleRoomDelete serves DELETE /rooms/<roomId>. Authorized strictly
// via the room's owner_secret — the plaintext must hash to the stored
// owner_secret_hash. Unlike the WS hello's is_owner grant, we
// intentionally do NOT fall back to owner_client_id matching: a
// client_id (rc_<id>) is sender-claimed and visible to every other
// participant in chat frames, so honouring it for an authoritative
// op like delete would let anyone who's seen a creator's id wipe
// their rooms. Cascades through chat_messages so the row is gone
// for good. Active participants are evicted from the in-memory
// registry; their writer goroutines drop on the next write.
func (s *Server) handleRoomDelete(w http.ResponseWriter, r *http.Request, roomID string) {
	type deleteReq struct {
		OwnerSecret string `json:"owner_secret"`
	}
	var req deleteReq
	// Body is optional for HTTP DELETE but the spec doesn't forbid it.
	// We tolerate empty / malformed bodies; the owner check below
	// will reject with the same 403.
	_ = json.NewDecoder(r.Body).Decode(&req)

	row, err := s.Store.GetRoom(r.Context(), roomID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			s.writeError(w, http.StatusNotFound, "room_not_found", "no such room")
			return
		}
		s.Logger.Error("delete_room get", "err", err, "room_id", roomID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}

	// Owner check — secret-only. A missing OwnerSecretHash on the
	// room means it was created before owner_secret support landed
	// (v0.5 stream-linked rooms, pre-#84 standalones). Those can't
	// be deleted via this endpoint; let them expire / archive
	// through a separate v1 admin path.
	if row.OwnerSecretHash == "" {
		s.writeError(w, http.StatusForbidden, "not_owner",
			"room has no owner_secret on file; cannot be deleted via this endpoint")
		return
	}
	if req.OwnerSecret == "" ||
		sha256Hex(req.OwnerSecret) != row.OwnerSecretHash {
		s.writeError(w, http.StatusForbidden, "not_owner",
			"owner_secret required and must match the room")
		return
	}

	if err := s.Store.DeleteRoom(r.Context(), roomID); err != nil {
		s.Logger.Error("delete_room", "err", err, "room_id", roomID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	// Evict the live room — any participants still connected lose
	// the broadcast channel and disconnect on their next write.
	s.Registry.Forget(roomID)

	s.writeData(w, http.StatusOK, struct {
		Deleted bool `json:"deleted"`
	}{Deleted: true})
}

// handleRoom serves GET /rooms/<roomId> and GET /rooms/<roomId>/history,
// plus DELETE /rooms/<roomId>. Dispatches by method + suffix because
// we're hand-rolling routing.
func (s *Server) handleRoom(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodDelete {
		rest := strings.TrimPrefix(r.URL.Path, "/rooms/")
		if rest == "" || strings.Contains(rest, "/") {
			s.writeError(w, http.StatusBadRequest, "bad_request",
				"DELETE /rooms/<roomId>")
			return
		}
		s.handleRoomDelete(w, r, rest)
		return
	}
	if r.Method == http.MethodPatch {
		rest := strings.TrimPrefix(r.URL.Path, "/rooms/")
		if rest == "" || strings.Contains(rest, "/") {
			s.writeError(w, http.StatusBadRequest, "bad_request",
				"PATCH /rooms/<roomId>")
			return
		}
		s.handleRoomPatch(w, r, rest)
		return
	}
	if r.Method != http.MethodGet {
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed",
			"GET, PATCH or DELETE only")
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/rooms/")
	if rest == "" {
		s.writeError(w, http.StatusBadRequest, "bad_request", "missing roomId")
		return
	}

	if idx := strings.Index(rest, "/"); idx >= 0 {
		roomID := rest[:idx]
		tail := rest[idx+1:]
		switch tail {
		case "history":
			s.handleHistory(w, r, roomID)
			return
		default:
			s.writeError(w, http.StatusNotFound, "not_found", "unknown room sub-path")
			return
		}
	}

	roomID := rest
	roomRow, err := s.Store.GetRoom(r.Context(), roomID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			s.writeError(w, http.StatusNotFound, "room_not_found", "no such room")
			return
		}
		s.Logger.Error("get_room", "err", err, "room_id", roomID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	var archivedMs *int64
	if roomRow.ArchivedAt != nil {
		v := roomRow.ArchivedAt.UnixMilli()
		archivedMs = &v
	}
	s.writeData(w, http.StatusOK, roomDetailPayload{
		RoomID:           roomRow.ID,
		Kind:             string(roomRow.Kind),
		LinkedStreamID:   roomRow.LinkedStreamID,
		OwnerAccountID:   roomRow.OwnerAccountID,
		Title:            roomRow.Title,
		CreatedAtMs:      roomRow.CreatedAt.UnixMilli(),
		ArchivedAtMs:     archivedMs,
		ParticipantCount: s.Registry.Get(roomRow.ID).Count(),
	})
}

func (s *Server) handleHistory(w http.ResponseWriter, r *http.Request, roomID string) {
	q := r.URL.Query()
	limit := 50
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 || n > 200 {
			s.writeError(w, http.StatusBadRequest, "bad_request",
				"limit must be 1..200")
			return
		}
		limit = n
	}
	cursor := q.Get("before")

	// Confirm room exists before returning an empty history (so 404
	// can be distinguished from "real room, no messages").
	if _, err := s.Store.GetRoom(r.Context(), roomID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			s.writeError(w, http.StatusNotFound, "room_not_found", "no such room")
			return
		}
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}

	msgs, next, err := s.Store.ListMessages(r.Context(), roomID, cursor, limit)
	if err != nil {
		s.Logger.Error("list_messages", "err", err, "room_id", roomID)
		s.writeError(w, http.StatusInternalServerError, "internal_error", "store failure")
		return
	}
	out := make([]messagePayload, 0, len(msgs))
	for _, m := range msgs {
		mp := messagePayload{
			ID:            m.ID,
			ParticipantID: m.ParticipantID,
			Nickname:      m.Nickname,
			Body:          m.Body,
			PostedAtMs:    m.PostedAt.UnixMilli(),
			IsHost:        m.IsHost,
			IsOwner:       m.IsOwner,
		}
		if m.DeletedAt != nil {
			mp.Deleted = true
			mp.Body = "[message removed]"
		}
		out = append(out, mp)
	}
	s.writeData(w, http.StatusOK, historyPayload{
		Messages:   out,
		NextCursor: next,
	})
}

// --- helpers --------------------------------------------------------

// writeData JSON-encodes payload inside the standard envelope.
func (s *Server) writeData(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(envelope{Data: payload, Error: nil})
}

// writeError JSON-encodes an error envelope.
func (s *Server) writeError(w http.ResponseWriter, status int, code, msg string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(envelope{
		Data:  nil,
		Error: &errorBody{Code: code, Message: msg},
	})
}

// shortID returns the first 12 hex chars of a v4 UUID, which is more
// than enough entropy for room / message / participant ids in our
// scale and is friendlier in URLs / logs than the full 32-char form.
func shortID() string {
	id := uuid.New()
	return strings.ReplaceAll(id.String(), "-", "")[:12]
}

// clientIP extracts the source IP, honouring proxy headers if the
// operator opted in. Used for per-IP global rate-limiting and for
// audit logs.
func (s *Server) clientIP(r *http.Request) string {
	if s.TrustProxyHeaders {
		for _, h := range []string{"Cf-Connecting-Ip", "True-Client-Ip", "X-Real-Ip"} {
			if v := r.Header.Get(h); v != "" {
				return v
			}
		}
		if v := r.Header.Get("X-Forwarded-For"); v != "" {
			if idx := strings.Index(v, ","); idx > 0 {
				return strings.TrimSpace(v[:idx])
			}
			return strings.TrimSpace(v)
		}
	}
	// RemoteAddr is "ip:port" — strip the port.
	addr := r.RemoteAddr
	if idx := strings.LastIndex(addr, ":"); idx > 0 {
		return addr[:idx]
	}
	return addr
}

