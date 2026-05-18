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

	// Rate limits, per source IP per hour. Tuned for the alpha-scale
	// expected traffic (a few hundred concurrent hosts), generous
	// enough that legitimate users — including a developer who
	// restarts the host process many times in an hour — don't trip
	// them. Tighten in production deploy via env vars if abuse shows
	// up.
	RateRegisterPerHour  int
	RateHeartbeatPerHour int
	RatePatchPerHour     int
	RateListPerHour      int
	RateReportPerHour    int
}

// Load reads environment variables and returns a populated Config.
// It returns an error if any variable is malformed; missing variables
// fall back to defaults.
func Load() (Config, error) {
	cfg := Config{
		Port:                 8081,
		DBPath:               "./data/directory.db",
		LogLevel:             slog.LevelInfo,
		TTL:                  120 * time.Second,
		RateRegisterPerHour:  60,
		RateHeartbeatPerHour: 600,
		RatePatchPerHour:     60,
		RateListPerHour:      600,
		RateReportPerHour:    30,
	}

	parsePositiveInt := func(name string, dst *int) error {
		if v := os.Getenv(name); v != "" {
			n, err := strconv.Atoi(v)
			if err != nil || n <= 0 {
				return fmt.Errorf("%s must be a positive integer, got %q", name, v)
			}
			*dst = n
		}
		return nil
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

	if err := parsePositiveInt("DIRECTORY_RATE_REGISTER_PER_HOUR",  &cfg.RateRegisterPerHour);  err != nil { return cfg, err }
	if err := parsePositiveInt("DIRECTORY_RATE_HEARTBEAT_PER_HOUR", &cfg.RateHeartbeatPerHour); err != nil { return cfg, err }
	if err := parsePositiveInt("DIRECTORY_RATE_PATCH_PER_HOUR",     &cfg.RatePatchPerHour);     err != nil { return cfg, err }
	if err := parsePositiveInt("DIRECTORY_RATE_LIST_PER_HOUR",      &cfg.RateListPerHour);      err != nil { return cfg, err }
	if err := parsePositiveInt("DIRECTORY_RATE_REPORT_PER_HOUR",    &cfg.RateReportPerHour);    err != nil { return cfg, err }

	return cfg, nil
}
