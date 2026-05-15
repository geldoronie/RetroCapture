// Command directory runs the RetroCapture public stream directory
// service. See platform/services/directory/README.md for what it does
// and the docs/DIRECTORY_PROTOCOL.md spec for the wire format.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/api"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/config"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/reaper"
	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "directory: fatal:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: cfg.LogLevel,
	}))
	logger.Info("starting",
		"port", cfg.Port,
		"db_path", cfg.DBPath,
		"ttl_seconds", int(cfg.TTL.Seconds()),
		"protocol_version", api.ProtocolVersion,
	)

	if dir := filepath.Dir(cfg.DBPath); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("mkdir db dir: %w", err)
		}
	}

	st, err := store.Open(cfg.DBPath)
	if err != nil {
		return fmt.Errorf("open store: %w", err)
	}
	defer st.Close()

	// Rate-limit policies. Defaults per docs/DIRECTORY_PROTOCOL.md;
	// each is overridable via the matching DIRECTORY_RATE_*_PER_HOUR
	// env var so the operator can tighten in production or loosen
	// during local development without recompiling.
	limRegister := ratelimit.New(ratelimit.PerHour(cfg.RateRegisterPerHour))
	limHeartbeat := ratelimit.New(ratelimit.PerHour(cfg.RateHeartbeatPerHour))
	limPatch := ratelimit.New(ratelimit.PerHour(cfg.RatePatchPerHour))
	limList := ratelimit.New(ratelimit.PerHour(cfg.RateListPerHour))
	limReport := ratelimit.New(ratelimit.PerHour(cfg.RateReportPerHour))

	server := &api.Server{
		Logger:         logger,
		Store:          st,
		TTL:            cfg.TTL,
		LimitRegister:  limRegister,
		LimitHeartbeat: limHeartbeat,
		LimitPatch:     limPatch,
		LimitList:      limList,
		LimitReport:    limReport,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Reaper: deletes expired entries.
	(&reaper.Reaper{
		Store:    st,
		Logger:   logger,
		Interval: 60 * time.Second,
	}).Start(ctx)

	// Rate-limit bucket eviction: every hour, drop buckets we haven't
	// seen in 2 h. Keeps the in-memory map bounded without ever
	// punishing a returning client.
	go evictRateLimitBuckets(ctx, logger,
		limRegister, limHeartbeat, limPatch, limList, limReport)

	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", cfg.Port),
		Handler:           server.Routes(),
		ReadHeaderTimeout: 5 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
		}
	}()

	select {
	case err := <-errCh:
		return fmt.Errorf("http server: %w", err)
	case <-ctx.Done():
		logger.Info("shutting down")
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("shutdown: %w", err)
	}
	logger.Info("stopped")
	return nil
}

func evictRateLimitBuckets(ctx context.Context, logger *slog.Logger, limiters ...*ratelimit.Limiter) {
	t := time.NewTicker(1 * time.Hour)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			total := 0
			for _, l := range limiters {
				total += l.EvictStale(2 * time.Hour)
			}
			if total > 0 {
				logger.Info("ratelimit_evicted", "count", total)
			}
		}
	}
}
