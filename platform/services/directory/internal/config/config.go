// Package config reads the service's runtime configuration from
// environment variables. Everything is read once at start-up; the
// service is intended to be restarted to pick up changes, not
// reload-on-SIGHUP.
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
	TTL      time.Duration
}

// Load reads environment variables and returns a populated Config.
// It returns an error if any variable is malformed; missing variables
// fall back to defaults.
func Load() (Config, error) {
	cfg := Config{
		Port:     8081,
		DBPath:   "./data/directory.db",
		LogLevel: slog.LevelInfo,
		TTL:      120 * time.Second,
	}

	if v := os.Getenv("DIRECTORY_PORT"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 || n > 65535 {
			return cfg, fmt.Errorf("DIRECTORY_PORT must be a port number, got %q", v)
		}
		cfg.Port = n
	}

	if v := os.Getenv("DIRECTORY_DB_PATH"); v != "" {
		cfg.DBPath = v
	}

	if v := os.Getenv("DIRECTORY_LOG_LEVEL"); v != "" {
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
			return cfg, fmt.Errorf("DIRECTORY_LOG_LEVEL must be debug/info/warn/error, got %q", v)
		}
	}

	if v := os.Getenv("DIRECTORY_TTL_SECONDS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return cfg, fmt.Errorf("DIRECTORY_TTL_SECONDS must be a positive integer, got %q", v)
		}
		cfg.TTL = time.Duration(n) * time.Second
	}

	return cfg, nil
}
