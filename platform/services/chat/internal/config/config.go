// Package config reads the chat service's runtime configuration from
// environment variables. Mirrors platform/services/directory/internal/
// config — everything is read once at start-up; the service is intended
// to be restarted to pick up changes, not reload-on-SIGHUP.
package config

import (
	"fmt"
	"log/slog"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config holds the fully-resolved runtime configuration.
type Config struct {
	Port     int
	DBPath   string
	LogLevel slog.Level

	// HelloTimeout is how long the server waits for the first `hello`
	// frame after a WS upgrade before closing the connection. Keeps
	// idle / probing connections from holding hub slots.
	HelloTimeout time.Duration

	// RatePostPer10s is the per-WebSocket-connection post-rate cap.
	// Slow mode (per-room, configurable by the room owner) layers on
	// top of this in v1; v0.5 has no slow mode.
	RatePostPer10s int

	// TrustProxyHeaders mirrors the directory's option: when the
	// service runs behind a reverse proxy (Cloudflare / nginx / FRP),
	// read the source IP from Cf-Connecting-Ip / True-Client-Ip /
	// X-Real-Ip / X-Forwarded-For before falling back to the TCP
	// RemoteAddr. Off by default so a fresh direct deployment is
	// spoofing-resistant.
	TrustProxyHeaders bool

	// RoomInactivityDays (#84) — rooms with no participant joins
	// and no posts for this many days are permanently deleted by
	// the sweep worker. 0 disables the sweep entirely. Activity is
	// stamped on the chat_rooms row on every WS hello + every
	// persisted message.
	RoomInactivityDays int
	// RoomSweepInterval is how often the sweep worker wakes up to
	// scan for expired rooms. Defaults to 1 hour — the sweep is
	// idempotent and cheap (indexed query + targeted DELETEs).
	RoomSweepInterval time.Duration
}

// Load reads environment variables and returns a populated Config.
// Returns an error if any variable is malformed; missing variables
// fall back to defaults.
func Load() (Config, error) {
	cfg := Config{
		Port:               8082,
		DBPath:             "./data/chat.db",
		LogLevel:           slog.LevelInfo,
		HelloTimeout:       5 * time.Second,
		RatePostPer10s:     5,
		TrustProxyHeaders:  false,
		RoomInactivityDays: 3,
		RoomSweepInterval:  1 * time.Hour,
	}

	if v := os.Getenv("CHAT_PORT"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 || n > 65535 {
			return cfg, fmt.Errorf("CHAT_PORT must be a port number, got %q", v)
		}
		cfg.Port = n
	}

	if v := os.Getenv("CHAT_DB_PATH"); v != "" {
		cfg.DBPath = v
	}

	if v := os.Getenv("CHAT_LOG_LEVEL"); v != "" {
		switch strings.ToLower(v) {
		case "debug":
			cfg.LogLevel = slog.LevelDebug
		case "info":
			cfg.LogLevel = slog.LevelInfo
		case "warn", "warning":
			cfg.LogLevel = slog.LevelWarn
		case "error":
			cfg.LogLevel = slog.LevelError
		default:
			return cfg, fmt.Errorf("CHAT_LOG_LEVEL must be debug/info/warn/error, got %q", v)
		}
	}

	if v := os.Getenv("CHAT_HELLO_TIMEOUT_SECONDS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return cfg, fmt.Errorf("CHAT_HELLO_TIMEOUT_SECONDS must be a positive integer, got %q", v)
		}
		cfg.HelloTimeout = time.Duration(n) * time.Second
	}

	if v := os.Getenv("CHAT_RATE_POST_PER_10S"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return cfg, fmt.Errorf("CHAT_RATE_POST_PER_10S must be a positive integer, got %q", v)
		}
		cfg.RatePostPer10s = n
	}

	if v := os.Getenv("CHAT_ROOM_INACTIVITY_DAYS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			return cfg, fmt.Errorf("CHAT_ROOM_INACTIVITY_DAYS must be >= 0, got %q", v)
		}
		cfg.RoomInactivityDays = n
	}

	if v := os.Getenv("CHAT_ROOM_SWEEP_INTERVAL_MINUTES"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return cfg, fmt.Errorf("CHAT_ROOM_SWEEP_INTERVAL_MINUTES must be > 0, got %q", v)
		}
		cfg.RoomSweepInterval = time.Duration(n) * time.Minute
	}

	if v := os.Getenv("CHAT_TRUST_PROXY_HEADERS"); v != "" {
		switch strings.ToLower(v) {
		case "1", "true", "yes", "on":
			cfg.TrustProxyHeaders = true
		case "0", "false", "no", "off":
			cfg.TrustProxyHeaders = false
		default:
			return cfg, fmt.Errorf("CHAT_TRUST_PROXY_HEADERS must be true/false, got %q", v)
		}
	}

	return cfg, nil
}
