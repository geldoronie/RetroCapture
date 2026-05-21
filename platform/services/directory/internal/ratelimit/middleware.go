package ratelimit

import (
	"encoding/json"
	"net"
	"net/http"
	"strconv"
	"time"
)

// Wrap returns an http.HandlerFunc that gates `next` behind `l`. The
// rate-limit key for each request is computed by `keyFn`. Denied
// requests get a 429 response with the canonical error envelope and
// a Retry-After header rounded up to the nearest second.
//
// If l is nil, Wrap returns `next` unchanged. This is intentional so
// callers can register an unlimited endpoint with the same pattern
// as the limited ones (`Wrap(nil, ...)` is a no-op).
func Wrap(l *Limiter, keyFn func(*http.Request) string, next http.HandlerFunc) http.HandlerFunc {
	if l == nil {
		return next
	}
	return func(w http.ResponseWriter, r *http.Request) {
		ok, retryAfter := l.Allow(keyFn(r))
		if !ok {
			writeTooManyRequests(w, retryAfter)
			return
		}
		next(w, r)
	}
}

// ClientIPKey extracts the request's source IP (without port). Falls
// back to RemoteAddr verbatim when the split fails. Matches the same
// IP-extraction convention the API package uses for logging.
//
// X-Forwarded-For is intentionally NOT consulted here. The reverse
// proxy or tunnel in front of the directory is responsible for
// presenting an honest RemoteAddr; honouring XFF without verifying
// the proxy chain would let any client forge their source.
func ClientIPKey(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func writeTooManyRequests(w http.ResponseWriter, retryAfter time.Duration) {
	seconds := int(retryAfter.Seconds())
	if seconds < 1 {
		seconds = 1
	}
	w.Header().Set("Retry-After", strconv.Itoa(seconds))
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusTooManyRequests)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"data": nil,
		"error": map[string]string{
			"code":    "rate_limited",
			"message": "rate limit exceeded; see Retry-After",
		},
	})
}
