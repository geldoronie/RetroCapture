// Package ratelimit provides a tiny in-memory rate limiter scoped to
// a single key (e.g. a participant id or a source IP). It's NOT a
// faithful sliding-window counter — it's a fixed-window counter that
// resets every `Window` duration, which is good enough for "5 posts
// per 10 seconds" semantics and costs O(1) memory per key.
package ratelimit

import (
	"sync"
	"time"
)

// Limiter is a per-key fixed-window counter. Safe for concurrent use.
type Limiter struct {
	Window time.Duration
	Max    int

	mu      sync.Mutex
	buckets map[string]*bucket
}

type bucket struct {
	resetAt time.Time
	count   int
}

// New returns a Limiter with the given window and per-key cap.
func New(window time.Duration, max int) *Limiter {
	return &Limiter{
		Window:  window,
		Max:     max,
		buckets: make(map[string]*bucket),
	}
}

// Allow records one event under key and returns true iff the event
// is below the per-window cap.
func (l *Limiter) Allow(key string) bool {
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	b, ok := l.buckets[key]
	if !ok || now.After(b.resetAt) {
		l.buckets[key] = &bucket{
			resetAt: now.Add(l.Window),
			count:   1,
		}
		return true
	}
	b.count++
	return b.count <= l.Max
}

// Forget drops a key's bucket (call on disconnect to free memory).
func (l *Limiter) Forget(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.buckets, key)
}

// EvictStale removes buckets whose reset time elapsed more than
// `idle` ago. Called periodically by a background goroutine so the
// map doesn't grow unbounded under attack.
func (l *Limiter) EvictStale(idle time.Duration) int {
	cutoff := time.Now().Add(-idle)
	l.mu.Lock()
	defer l.mu.Unlock()
	n := 0
	for k, b := range l.buckets {
		if b.resetAt.Before(cutoff) {
			delete(l.buckets, k)
			n++
		}
	}
	return n
}
