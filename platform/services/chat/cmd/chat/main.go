// Command chat runs the RetroCapture chat service. See platform/
// services/chat/README.md for what it does and docs/CHAT_PROTOCOL.md
// for the wire format.
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

	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/api"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/config"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/ratelimit"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/room"
	"github.com/geldoronie/RetroCapture/platform/services/chat/internal/store"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chat: fatal:", err)
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

	registry := room.NewRegistry()

	limPost := ratelimit.New(10*time.Second, cfg.RatePostPer10s)

	server := &api.Server{
		Logger:            logger,
		Store:             st,
		Registry:          registry,
		LimitPost:         limPost,
		HelloTimeout:      cfg.HelloTimeout,
		TrustProxyHeaders: cfg.TrustProxyHeaders,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Periodic eviction of idle rate-limit buckets so the in-memory
	// map doesn't grow unbounded under abuse. Same cadence the
	// directory uses.
	go evictRateLimitBuckets(ctx, logger, limPost)

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
