// Package api wires HTTP + WebSocket handlers for the chat service.
// See docs/CHAT_PROTOCOL.md for the wire format these implement.
package api

import (
	"encoding/json"
)

// ProtocolVersion is advertised at /health. Bumped on backwards-
// incompatible changes; the service must continue accepting the
// previous version for at least one release.
const ProtocolVersion = 1

// envelope is the common shape of every JSON response on the HTTP
// side: `data` carries the payload on success, `error` carries
// `{code, message}` on failure. Mirrors the directory service's
// shape so clients can share envelope-parsing code.
type envelope struct {
	Data  any        `json:"data"`
	Error *errorBody `json:"error"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// --- HTTP payloads --------------------------------------------------

type healthPayload struct {
	Status          string `json:"status"`
	ProtocolVersion int    `json:"protocol_version"`
	Rooms           int    `json:"rooms"`
	Participants    int    `json:"participants"`
}

type roomDetailPayload struct {
	RoomID           string `json:"room_id"`
	Kind             string `json:"kind"`
	LinkedStreamID   string `json:"linked_stream_id,omitempty"`
	OwnerAccountID   string `json:"owner_account_id,omitempty"`
	Title            string `json:"title"`
	CreatedAtMs      int64  `json:"created_at_ms"`
	ArchivedAtMs     *int64 `json:"archived_at_ms"`
	ParticipantCount int    `json:"participant_count"`
}

type messagePayload struct {
	ID            string `json:"id"`
	RoomID        string `json:"room_id,omitempty"`
	ParticipantID string `json:"participant_id"`
	Nickname      string `json:"nickname"`
	Body          string `json:"body"`
	PostedAtMs    int64  `json:"posted_at_ms"`
	Deleted       bool   `json:"deleted,omitempty"`
	IsHost        bool   `json:"is_host,omitempty"`
}

type historyPayload struct {
	Messages   []messagePayload `json:"messages"`
	NextCursor string           `json:"next_cursor,omitempty"`
}

type roomByStreamPayload struct {
	RoomID  string `json:"room_id"`
	Created bool   `json:"created"`
}

type roomBySlugPayload struct {
	RoomID string `json:"room_id"`
	Slug   string `json:"slug"`
	Title  string `json:"title"`
}

// Request body for POST /rooms — both fields are optional. An empty
// slug triggers server-side autogeneration; an empty title falls
// back to the slug.
type createRoomRequest struct {
	Title string `json:"title"`
	Slug  string `json:"slug"`
}

type createRoomPayload struct {
	RoomID string `json:"room_id"`
	Slug   string `json:"slug"`
	Title  string `json:"title"`
}

// --- WebSocket envelope ---------------------------------------------

// wsFrame is a single JSON frame on the WebSocket. Exactly one of the
// sub-payloads is populated, identified by `Kind`.
type wsFrame struct {
	Kind string `json:"kind"`

	Hello     *wsHello     `json:"hello,omitempty"`
	Welcome   *wsWelcome   `json:"welcome,omitempty"`
	Post      *wsPost      `json:"post,omitempty"`
	Delete    *wsDelete    `json:"delete,omitempty"`
	Ping      *wsPing      `json:"ping,omitempty"`
	Pong      *wsPong      `json:"pong,omitempty"`
	Message   *wsMessage   `json:"message,omitempty"`
	Presence  *wsPresence  `json:"presence,omitempty"`
	Deleted   *wsDeleted   `json:"deleted,omitempty"`
	Error     *wsError     `json:"error,omitempty"`
	RoomState *wsRoomState `json:"room_state,omitempty"`
}

// Client → server.
type wsHello struct {
	Nickname string `json:"nickname"`
	// Role is an opt-in self-declaration. The only meaningful value
	// in v0.5 is "host" — the RetroCapture instance that's publishing
	// this stream announces itself so its messages can render with a
	// host badge on every viewer's panel. First claimant per room
	// wins (v0.5 trust model: same level as streamId secrecy; v1
	// validates against the directory ownerToken).
	Role string `json:"role,omitempty"`
}

type wsPost struct {
	Body string `json:"body"`
}

type wsDelete struct {
	MessageID string `json:"message_id"`
}

type wsPing struct{}

// Server → client.
type wsWelcome struct {
	ParticipantID     string `json:"participant_id"`
	RoomID            string `json:"room_id"`
	RoomKind          string `json:"room_kind"`
	LinkedStreamID    string `json:"linked_stream_id,omitempty"`
	ServerTimeMs      int64  `json:"server_time_ms"`
	ProtocolVersion   int    `json:"protocol_version"`
	// #84 — Host role plumbing. IsHost echoes whether this welcome's
	// participant_id is the room's host (relevant when the connecting
	// client passed role:"host" and won the claim). HostParticipantID
	// is the room's current host id, empty if no one has claimed.
	IsHost            bool   `json:"is_host,omitempty"`
	HostParticipantID string `json:"host_participant_id,omitempty"`
}

type wsMessage struct {
	ID            string `json:"id"`
	RoomID        string `json:"room_id"`
	ParticipantID string `json:"participant_id"`
	Nickname      string `json:"nickname"`
	Body          string `json:"body"`
	PostedAtMs    int64  `json:"posted_at_ms"`
	// #84 — Sticky host flag. Set at insert-time based on the
	// poster's room-host status; stays true across server restarts
	// even if the host's session id has since changed.
	IsHost        bool   `json:"is_host,omitempty"`
}

type wsPresence struct {
	ParticipantID string `json:"participant_id"`
	Nickname      string `json:"nickname"`
	Event         string `json:"event"` // "join" | "leave"
	IsHost        bool   `json:"is_host,omitempty"`
}

type wsDeleted struct {
	MessageID string `json:"message_id"`
	DeletedBy string `json:"deleted_by"`
}

type wsPong struct{}

type wsError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

type wsRoomState struct {
	Participants     []wsParticipant `json:"participants"`
	Settings         wsRoomSettings  `json:"settings"`
	// #84 — Current host's participant id (empty if no host has
	// claimed the room yet). Lets the client mark seeded backlog
	// messages whose participant_id matches.
	HostParticipantID string         `json:"host_participant_id,omitempty"`
}

type wsParticipant struct {
	ID       string `json:"id"`
	Nickname string `json:"nickname"`
	IsHost   bool   `json:"is_host,omitempty"`
}

type wsRoomSettings struct {
	SlowModeSecs int      `json:"slow_mode_secs"`
	WordFilter   []string `json:"word_filter"`
}

// encodeFrame serialises a wsFrame for sending. Top-level marshal —
// the embedded payloads are pointers so omitempty actually fires.
func encodeFrame(f wsFrame) ([]byte, error) {
	return json.Marshal(f)
}
