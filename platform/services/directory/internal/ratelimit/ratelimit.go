// Package ratelimit implements a per-key token-bucket rate limiter
// suitable for HTTP middleware. State is in-memory and bounded by
// periodic eviction of stale keys (see Limiter.EvictStale).
//
// A bucket refills continuously at Config.Rate tokens per second up
// to Config.Capacity. Each Allow consumes one token; if no token is
// available the call returns false along with the duration the
// caller would have to wait for one to appear.
//
// Concurrent-safe. A single Limiter handles many keys; we expect one
// Limiter per policy (e.g. one for /register, one for /heartbeat).
package ratelimit

import (
	"sync"
	"time"
)

// Config describes a rate-limit policy.
//
//   - Rate     — sustained tokens per second
//   - Capacity — burst size; the most tokens a bucket can accumulate
//
// PerHour and PerMinute are the conventional helpers; build Config
// directly only when you need an unusual ratio (e.g. small burst
// with high sustained rate for tests).
type Config struct {
	Rate     float64
	Capacity float64
}

// PerHour builds a Config that allows n requests per hour with a
// burst of n. Equivalent to bucket size n refilling at n/3600 tokens
// per second.
func PerHour(n int) Config {
	return Config{
		Rate:     float64(n) / 3600.0,
		Capacity: float64(n),
	}
}

// PerMinute is the same shape as PerHour but at minute granularity.
func PerMinute(n int) Config {
	return Config{
		Rate:     float64(n) / 60.0,
		Capacity: float64(n),
	}
}

// Limiter applies one Config across many keys. The zero value is not
// useful; construct via New.
type Limiter struct {
	cfg     Config
	mu      sync.Mutex
	buckets map[string]*bucket
}

type bucket struct {
	tokens     float64
	lastRefill time.Time
	lastTouch  time.Time
}

// New constructs a Limiter with the given policy.
func New(cfg Config) *Limiter {
	return &Limiter{
		cfg:     cfg,
		buckets: make(map[string]*bucket),
	}
}

// Allow attempts to consume one token for the given key. Returns
// (true, 0) if a token was consumed; (false, retryAfter) if the
// caller should back off for that long before retrying.
//
// retryAfter is conservative (always >= 1 ns), so handlers serialising
// it into a Retry-After header can clamp upward to a whole-second
// minimum without ever sending zero.
func (l *Limiter) Allow(key string) (bool, time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()

	now := time.Now()
	b, ok := l.buckets[key]
	if !ok {
		// First sighting of this key — start the bucket full so the
		// first burst goes through (this is what we want for a fresh
		// client: they don't owe history).
		l.buckets[key] = &bucket{
			tokens:     l.cfg.Capacity - 1, // consume one
			lastRefill: now,
			lastTouch:  now,
		}
		return true, 0
	}

	// Refill based on elapsed wall-clock time since the previous
	// refill. We don't decay across reboots — bucket state is in
	// memory, so a restart effectively gives every client a fresh
	// full bucket. Acceptable for our usage.
	elapsed := now.Sub(b.lastRefill).Seconds()
	if elapsed > 0 {
		b.tokens += elapsed * l.cfg.Rate
		if b.tokens > l.cfg.Capacity {
			b.tokens = l.cfg.Capacity
		}
		b.lastRefill = now
	}
	b.lastTouch = now

	if b.tokens >= 1.0 {
		b.tokens -= 1.0
		return true, 0
	}

	// Compute the wait until one token becomes available. Rate==0
	// would mean the policy denies everything once the bucket runs
	// out — return a very long retry-after rather than dividing by
	// zero.
	if l.cfg.Rate <= 0 {
		return false, 24 * time.Hour
	}
	deficit := 1.0 - b.tokens
	wait := time.Duration(deficit / l.cfg.Rate * float64(time.Second))
	if wait < time.Nanosecond {
		wait = time.Nanosecond
	}
	return false, wait
}

// EvictStale removes bucket state for keys not touched in the given
// idle period. Returns the count evicted. Safe to call concurrently
// with Allow.
//
// Eviction is purely memory hygiene: the only state lost is the
// per-key bucket level, and a fresh first-touch starts with a full
// bucket, so evicting a key just means that client now sees a fresh
// rate-limit window. Acceptable.
func (l *Limiter) EvictStale(idle time.Duration) int {
	l.mu.Lock()
	defer l.mu.Unlock()

	cutoff := time.Now().Add(-idle)
	n := 0
	for k, b := range l.buckets {
		if b.lastTouch.Before(cutoff) {
			delete(l.buckets, k)
			n++
		}
	}
	return n
}

// Size returns the number of keys currently tracked. Mainly useful
// for tests and metrics.
func (l *Limiter) Size() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.buckets)
}
