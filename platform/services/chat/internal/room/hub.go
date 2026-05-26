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
}

// NewRoom returns an empty room. Created lazily by the Registry.
func NewRoom(id string) *Room {
	return &Room{
		ID:           id,
		participants: make(map[string]*Participant),
	}
}

// Join adds the participant to the room.
func (r *Room) Join(p *Participant) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.participants[p.ID] = p
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
}

func (r *Room) Snapshot() []ParticipantInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]ParticipantInfo, 0, len(r.participants))
	for _, p := range r.participants {
		out = append(out, ParticipantInfo{ID: p.ID, Nickname: p.Nickname})
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
