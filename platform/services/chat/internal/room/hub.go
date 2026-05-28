// Package room maintains the in-memory presence + broadcast layer for
// chat. Each active chat room has one Room object in the registry,
// holding the set of connected participants and a fanout buffer.
//
// Storage of the actual messages still happens via internal/store —
// the hub only handles the live-now part. Restarting the service
// resets the registry to empty (participants reconnect, history
// reloads from the store).
package room

import (
	"sync"
	"time"
)

// Participant is one open WebSocket connection inside a Room.
type Participant struct {
	ID       string // "p_<random>" — per-connection, NOT stable across reconnects in v0.5
	Nickname string
	JoinedAt time.Time
	// IsHost is true when this participant claimed the host role
	// (and won) for the room. Set once at hello-time; never flips
	// during the session.
	IsHost   bool
	// IsOwner mirrors IsHost for standalone rooms — set when the
	// participant's client_id matches the room's owner_client_id.
	IsOwner  bool

	// Send is the per-connection outbound channel. The WebSocket
	// writer goroutine reads from this; the hub writes to it during
	// fanout. Closed by the hub when it removes the participant.
	Send chan []byte
}

// Room is a live chat room. The registry hands out *Room handles;
// each is safe for concurrent use.
type Room struct {
	ID string

	mu           sync.RWMutex
	participants map[string]*Participant
	// hostID is the participant id of whoever currently holds the
	// host role for this room. First-claim-wins in v0.5; cleared
	// when that participant leaves so a reconnect can re-claim.
	hostID       string
}

// NewRoom returns an empty room. Created lazily by the Registry.
func NewRoom(id string) *Room {
	return &Room{
		ID:           id,
		participants: make(map[string]*Participant),
	}
}

// TryClaimHost atomically assigns the host role to participantID
// when no one currently holds it. Returns true on success. The
// caller is expected to set the Participant.IsHost flag on a
// successful claim.
func (r *Room) TryClaimHost(participantID string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.hostID != "" {
		return false
	}
	r.hostID = participantID
	return true
}

// HostID returns the current host's participant id, or "" if no one
// holds the role.
func (r *Room) HostID() string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.hostID
}

// Join adds the participant to the room. Caller must ensure p.ID is
// unique; use TryJoin if collisions are possible (client-supplied
// persistent ids).
func (r *Room) Join(p *Participant) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.participants[p.ID] = p
}

// TryJoin atomically rejects the join when p.ID is already in use
// in the room — returns false and DOES NOT add the participant.
// Used for client-supplied persistent ids (rc_<...>) where two
// connections from the same identity must not collide.
func (r *Room) TryJoin(p *Participant) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, taken := r.participants[p.ID]; taken {
		return false
	}
	r.participants[p.ID] = p
	return true
}

// Leave removes the participant and closes its Send channel.
// Idempotent.
func (r *Room) Leave(participantID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if p, ok := r.participants[participantID]; ok {
		delete(r.participants, participantID)
		// Close on the next tick so any in-flight write into Send
		// drains. The writer goroutine exits when it sees Send
		// closed.
		close(p.Send)
	}
	// If the host left, clear the slot so a reconnect can re-claim.
	// First-claim-wins isn't great UX if the host crashes and reopens
	// against a still-populated room; with this clear it just works.
	if r.hostID == participantID {
		r.hostID = ""
	}
}

// Broadcast sends the encoded JSON frame to every participant
// currently in the room. Slow consumers (Send channel full) are
// dropped — the writer is responsible for keeping up; we'd rather
// lose a message than block the whole room.
func (r *Room) Broadcast(frame []byte) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, p := range r.participants {
		select {
		case p.Send <- frame:
		default:
			// drop on full
		}
	}
}

// BroadcastExcept is like Broadcast but skips a single participant
// (e.g. the sender for actions that shouldn't echo). v0.5 doesn't
// use this — posts echo back to the sender for consistency — but
// it's here for the moderation paths in v1.
func (r *Room) BroadcastExcept(frame []byte, skipParticipantID string) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, p := range r.participants {
		if p.ID == skipParticipantID {
			continue
		}
		select {
		case p.Send <- frame:
		default:
		}
	}
}

// Snapshot returns a copy of the current participant set, intended
// for the `room_state` welcome payload. Cheap — runs in O(N) and
// allocs N ParticipantInfo structs.
type ParticipantInfo struct {
	ID       string
	Nickname string
	IsHost   bool
	IsOwner  bool
}

func (r *Room) Snapshot() []ParticipantInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]ParticipantInfo, 0, len(r.participants))
	for _, p := range r.participants {
		out = append(out, ParticipantInfo{
			ID:       p.ID,
			Nickname: p.Nickname,
			IsHost:   p.IsHost,
			IsOwner:  p.IsOwner,
		})
	}
	return out
}

// Count returns the number of currently-connected participants.
func (r *Room) Count() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.participants)
}

// Registry is the process-wide map of rooms-currently-being-served.
// A room is created lazily on the first Join and stays alive until
// either it goes empty AND the registry is asked to GC it (the
// gateway never explicitly evicts; we just let empty rooms stay
// around since they're tiny — a follow-up can add eviction if
// memory becomes a concern).
type Registry struct {
	mu    sync.Mutex
	rooms map[string]*Room
}

// NewRegistry returns an empty registry.
func NewRegistry() *Registry {
	return &Registry{
		rooms: make(map[string]*Room),
	}
}

// Get returns the in-memory Room for id, creating it on first use.
func (g *Registry) Get(id string) *Room {
	g.mu.Lock()
	defer g.mu.Unlock()
	if r, ok := g.rooms[id]; ok {
		return r
	}
	r := NewRoom(id)
	g.rooms[id] = r
	return r
}

// Stats returns coarse counters for /health diagnostics.
func (g *Registry) Stats() (rooms int, totalParticipants int) {
	g.mu.Lock()
	defer g.mu.Unlock()
	rooms = len(g.rooms)
	for _, r := range g.rooms {
		totalParticipants += r.Count()
	}
	return
}
