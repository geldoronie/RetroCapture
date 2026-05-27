// WebSocket upgrade + envelope routing. The transport itself uses
// github.com/coder/websocket (modern, simple, MIT-licensed fork of
// nhooyr.io/websocket).
//
// One goroutine per connection (read loop); we use channel-based
// fanout for writes (the writer goroutine drains a per-participant
// Send channel that the room hub feeds during Broadcast). This keeps
// the hub Broadcast call non-blocking — slow consumers get dropped
// rather than back-pressuring the rest of the room.
package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/coder/websocket"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/room"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/store"
)

const (
	maxMessageBytes = 4 * 1024 // 4 KB per WS frame is plenty for chat lines
	maxBodyChars    = 800      // visible-chars cap on a single chat line
	sendBufferSize  = 64       // per-participant outbound buffer; >this → drop
)

// handleWebSocket upgrades the HTTP request to a WebSocket and runs
// the per-connection loop. Query string must specify EITHER ?room=<id>
// (existing room) OR ?stream=<id> (auto-resolve / auto-create).
func (s *Server) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	roomID := q.Get("room")
	streamID := q.Get("stream")
	if roomID == "" && streamID == "" {
		s.writeError(w, http.StatusBadRequest, "bad_request",
			"need ?room=<id> or ?stream=<id>")
		return
	}

	// Resolve room id if the client only gave us a stream id.
	if roomID == "" {
		generatedID := "r_" + shortID()
		row, _, err := s.Store.GetOrCreateRoomForStream(
			r.Context(), streamID, generatedID, "",
		)
		if err != nil {
			s.Logger.Error("ws_resolve_room", "err", err, "stream_id", streamID)
			s.writeError(w, http.StatusInternalServerError, "internal_error",
				"could not resolve room for stream")
			return
		}
		roomID = row.ID
	} else {
		if _, err := s.Store.GetRoom(r.Context(), roomID); err != nil {
			if errors.Is(err, store.ErrNotFound) {
				s.writeError(w, http.StatusNotFound, "room_not_found", "no such room")
				return
			}
			s.Logger.Error("ws_get_room", "err", err, "room_id", roomID)
			s.writeError(w, http.StatusInternalServerError, "internal_error",
				"store failure")
			return
		}
	}

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// CORS is wide-open in v0.5 to match the rest of the service;
		// v1's directory-account auth will tighten origin checks.
		InsecureSkipVerify: true,
	})
	if err != nil {
		s.Logger.Warn("ws_accept", "err", err)
		return
	}
	conn.SetReadLimit(maxMessageBytes)

	s.serveSession(conn, roomID)
}

// serveSession drives one WebSocket connection from hello → close.
func (s *Server) serveSession(conn *websocket.Conn, roomID string) {
	// Detached context so the session continues even after the HTTP
	// request's context cancels (which it does as soon as the
	// handler returns).
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	defer func() {
		_ = conn.Close(websocket.StatusNormalClosure, "")
	}()

	// Step 1 — wait for the hello frame.
	helloCtx, helloCancel := context.WithTimeout(ctx, s.HelloTimeout)
	defer helloCancel()
	first, err := readFrame(helloCtx, conn)
	if err != nil {
		s.Logger.Debug("ws_hello_timeout", "room_id", roomID, "err", err)
		return
	}
	if first.Kind != "hello" || first.Hello == nil {
		writeFrame(ctx, conn, wsFrame{
			Kind:  "error",
			Error: &wsError{Code: "bad_request", Message: "first frame must be hello"},
		})
		return
	}
	nick := sanitizeNickname(first.Hello.Nickname)
	if nick == "" {
		writeFrame(ctx, conn, wsFrame{
			Kind:  "error",
			Error: &wsError{Code: "bad_request", Message: "nickname required"},
		})
		return
	}

	// Step 2 — register participant.
	roomRow, err := s.Store.GetRoom(ctx, roomID)
	if err != nil {
		s.Logger.Error("ws_get_room", "err", err, "room_id", roomID)
		writeFrame(ctx, conn, wsFrame{
			Kind:  "error",
			Error: &wsError{Code: "internal_error", Message: "store failure"},
		})
		return
	}
	roomLive := s.Registry.Get(roomID)
	p := &room.Participant{
		ID:       "p_" + shortID(),
		Nickname: nick,
		JoinedAt: time.Now().UTC(),
		Send:     make(chan []byte, sendBufferSize),
	}
	// #84 — Host role claim. Only meaningful on stream_linked rooms;
	// standalone rooms can re-use the same field later for room
	// owners but v0.5 doesn't expose that path.
	if first.Hello.Role == "host" && roomRow.Kind == store.RoomKindStreamLinked {
		if roomLive.TryClaimHost(p.ID) {
			p.IsHost = true
		}
	}
	roomLive.Join(p)
	defer func() {
		roomLive.Leave(p.ID)
		if s.LimitPost != nil {
			s.LimitPost.Forget(p.ID)
		}
	}()

	// Step 3 — welcome + room_state.
	if err := writeFrame(ctx, conn, wsFrame{
		Kind: "welcome",
		Welcome: &wsWelcome{
			ParticipantID:     p.ID,
			RoomID:            roomRow.ID,
			RoomKind:          string(roomRow.Kind),
			LinkedStreamID:    roomRow.LinkedStreamID,
			ServerTimeMs:      time.Now().UnixMilli(),
			ProtocolVersion:   ProtocolVersion,
			IsHost:            p.IsHost,
			HostParticipantID: roomLive.HostID(),
		},
	}); err != nil {
		return
	}
	snap := roomLive.Snapshot()
	parts := make([]wsParticipant, 0, len(snap))
	for _, sp := range snap {
		parts = append(parts, wsParticipant{
			ID:       sp.ID,
			Nickname: sp.Nickname,
			IsHost:   sp.IsHost,
		})
	}
	if err := writeFrame(ctx, conn, wsFrame{
		Kind: "room_state",
		RoomState: &wsRoomState{
			Participants:      parts,
			Settings:          wsRoomSettings{SlowModeSecs: 0, WordFilter: []string{}},
			HostParticipantID: roomLive.HostID(),
		},
	}); err != nil {
		return
	}

	// Step 4 — announce join to other participants.
	if presence, err := encodeFrame(wsFrame{
		Kind: "presence",
		Presence: &wsPresence{
			ParticipantID: p.ID,
			Nickname:      p.Nickname,
			Event:         "join",
			IsHost:        p.IsHost,
		},
	}); err == nil {
		roomLive.BroadcastExcept(presence, p.ID)
	}

	// Step 5 — writer + reader.
	writerDone := make(chan struct{})
	go func() {
		defer close(writerDone)
		for {
			select {
			case <-ctx.Done():
				return
			case frame, ok := <-p.Send:
				if !ok {
					return
				}
				wctx, wcancel := context.WithTimeout(ctx, 5*time.Second)
				err := conn.Write(wctx, websocket.MessageText, frame)
				wcancel()
				if err != nil {
					return
				}
			}
		}
	}()

	// Reader loop. Returns when the connection closes or a fatal
	// protocol error happens.
	for {
		frame, err := readFrame(ctx, conn)
		if err != nil {
			break
		}
		switch frame.Kind {
		case "post":
			if frame.Post == nil {
				writeFrame(ctx, conn, wsFrame{
					Kind:  "error",
					Error: &wsError{Code: "bad_request", Message: "post needs body"},
				})
				continue
			}
			s.handlePost(ctx, roomRow, roomLive, p, frame.Post.Body)
		case "delete":
			// Moderation: v0.5 lets a participant delete their own
			// messages only (no per-room moderator concept yet).
			if frame.Delete == nil || frame.Delete.MessageID == "" {
				writeFrame(ctx, conn, wsFrame{
					Kind:  "error",
					Error: &wsError{Code: "bad_request", Message: "delete needs message_id"},
				})
				continue
			}
			s.handleDelete(ctx, roomRow.ID, roomLive, p, frame.Delete.MessageID)
		case "ping":
			writeFrame(ctx, conn, wsFrame{Kind: "pong", Pong: &wsPong{}})
		default:
			writeFrame(ctx, conn, wsFrame{
				Kind:  "error",
				Error: &wsError{Code: "bad_request", Message: "unknown kind: " + frame.Kind},
			})
		}
	}

	// Step 6 — announce leave.
	if presence, err := encodeFrame(wsFrame{
		Kind: "presence",
		Presence: &wsPresence{
			ParticipantID: p.ID,
			Nickname:      p.Nickname,
			Event:         "leave",
		},
	}); err == nil {
		roomLive.BroadcastExcept(presence, p.ID)
	}
	cancel()
	<-writerDone
}

func (s *Server) handlePost(
	ctx context.Context,
	roomRow *store.Room,
	roomLive *room.Room,
	p *room.Participant,
	bodyIn string,
) {
	body := sanitizeBody(bodyIn)
	if body == "" {
		// Empty after trim — silently drop instead of errorring
		// (clients sometimes send accidental empties on whitespace).
		return
	}
	if s.LimitPost != nil && !s.LimitPost.Allow(p.ID) {
		// Send the rate-limit error on the participant's Send channel
		// so it goes through the same writer goroutine path.
		if frame, err := encodeFrame(wsFrame{
			Kind:  "error",
			Error: &wsError{Code: "rate_limited", Message: "too many messages"},
		}); err == nil {
			select {
			case p.Send <- frame:
			default:
			}
		}
		return
	}

	msg := &store.Message{
		ID:            "m_" + shortID(),
		RoomID:        roomRow.ID,
		ParticipantID: p.ID,
		Nickname:      p.Nickname,
		Body:          body,
		PostedAt:      time.Now().UTC(),
		IsHost:        p.IsHost,
	}
	if err := s.Store.InsertMessage(ctx, msg); err != nil {
		s.Logger.Error("insert_message", "err", err, "room_id", roomRow.ID)
		if frame, e := encodeFrame(wsFrame{
			Kind:  "error",
			Error: &wsError{Code: "internal_error", Message: "could not persist"},
		}); e == nil {
			select {
			case p.Send <- frame:
			default:
			}
		}
		return
	}

	frame, err := encodeFrame(wsFrame{
		Kind: "message",
		Message: &wsMessage{
			ID:            msg.ID,
			RoomID:        msg.RoomID,
			ParticipantID: msg.ParticipantID,
			Nickname:      msg.Nickname,
			Body:          msg.Body,
			PostedAtMs:    msg.PostedAt.UnixMilli(),
			IsHost:        msg.IsHost,
		},
	})
	if err != nil {
		s.Logger.Error("encode_message", "err", err)
		return
	}
	roomLive.Broadcast(frame)
}

func (s *Server) handleDelete(
	ctx context.Context,
	roomID string,
	roomLive *room.Room,
	p *room.Participant,
	messageID string,
) {
	// v0.5: only the original poster can delete. v1 adds room-owner
	// moderation against the directory-account JWT.
	// We don't have the message author here; the store check is
	// enough as a placeholder — caller side. For now we just delete
	// regardless and log; tighten as soon as moderation lands.
	if err := s.Store.SoftDeleteMessage(ctx, roomID, messageID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			return
		}
		s.Logger.Error("soft_delete", "err", err, "room_id", roomID, "msg_id", messageID)
		return
	}
	if frame, err := encodeFrame(wsFrame{
		Kind: "deleted",
		Deleted: &wsDeleted{
			MessageID: messageID,
			DeletedBy: p.ID,
		},
	}); err == nil {
		roomLive.Broadcast(frame)
	}
}

// readFrame reads one WS frame and JSON-decodes the envelope.
func readFrame(ctx context.Context, conn *websocket.Conn) (*wsFrame, error) {
	_, data, err := conn.Read(ctx)
	if err != nil {
		return nil, err
	}
	var f wsFrame
	if err := json.Unmarshal(data, &f); err != nil {
		return nil, err
	}
	if f.Kind == "" {
		return nil, errors.New("missing kind")
	}
	return &f, nil
}

// writeFrame is used directly on synchronous paths (hello/welcome
// before the writer goroutine starts). After welcome, prefer pushing
// to participant.Send so the dedicated writer goroutine handles the
// actual conn.Write.
func writeFrame(ctx context.Context, conn *websocket.Conn, f wsFrame) error {
	if conn == nil {
		return nil
	}
	body, err := encodeFrame(f)
	if err != nil {
		return err
	}
	wctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	return conn.Write(wctx, websocket.MessageText, body)
}

// sanitizeNickname trims whitespace and caps the visible length. v0.5
// is lenient — any non-empty trimmed string passes. Banned-substring
// filtering comes with v1's word filter.
func sanitizeNickname(in string) string {
	s := strings.TrimSpace(in)
	if len([]rune(s)) > 32 {
		s = string([]rune(s)[:32])
	}
	return s
}

// sanitizeBody trims whitespace and caps to maxBodyChars.
func sanitizeBody(in string) string {
	s := strings.TrimSpace(in)
	if r := []rune(s); len(r) > maxBodyChars {
		s = string(r[:maxBodyChars])
	}
	return s
}
