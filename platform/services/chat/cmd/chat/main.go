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

	// #84 — Room inactivity sweep. Permanently deletes rooms (and
	// their messages) that have had no joins and no posts within
	// the configured window. CHAT_ROOM_INACTIVITY_DAYS=0 disables.
	if cfg.RoomInactivityDays > 0 {
		go sweepInactiveRooms(ctx, logger, st, registry,
			cfg.RoomSweepInterval,
			time.Duration(cfg.RoomInactivityDays)*24*time.Hour)
	} else {
		logger.Info("inactivity_sweep disabled (CHAT_ROOM_INACTIVITY_DAYS=0)")
	}

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

// sweepInactiveRooms periodically wipes rooms (and their messages)
// whose last activity timestamp is older than `window`. We skip
// any room currently has at least one live participant in the
// in-memory Registry — even if the DB row's last_activity_ms is
// stale (e.g. a long-lived viewer who joined right at the window
// boundary), an actually-connected room must not be reaped.
//
// Idempotent + safe to run alongside DELETE /rooms; both paths
// use the same Store.DeleteRoom transaction so a race is at worst
// a 404 on one side.
func sweepInactiveRooms(
	ctx context.Context,
	logger *slog.Logger,
	st *store.Store,
	registry *room.Registry,
	interval time.Duration,
	window time.Duration,
) {
	logger.Info("inactivity_sweep starting",
		"interval", interval.String(),
		"window", window.String())
	t := time.NewTicker(interval)
	defer t.Stop()
	// Fire once on boot so a fresh container picks up any past-due
	// rooms immediately instead of waiting a full interval.
	runSweepOnce(ctx, logger, st, registry, window)
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			runSweepOnce(ctx, logger, st, registry, window)
		}
	}
}

func runSweepOnce(
	ctx context.Context,
	logger *slog.Logger,
	st *store.Store,
	registry *room.Registry,
	window time.Duration,
) {
	cutoff := time.Now().UTC().Add(-window).UnixMilli()
	ids, err := st.ListInactiveRoomIDs(ctx, cutoff)
	if err != nil {
		logger.Warn("inactivity_sweep list_failed", "err", err)
		return
	}
	deleted := 0
	skippedLive := 0
	for _, id := range ids {
		if registry.HasParticipants(id) {
			skippedLive++
			continue
		}
		if err := st.DeleteRoom(ctx, id); err != nil {
			logger.Warn("inactivity_sweep delete_failed",
				"err", err, "room_id", id)
			continue
		}
		registry.Forget(id)
		deleted++
	}
	if deleted > 0 || skippedLive > 0 {
		logger.Info("inactivity_sweep tick",
			"deleted", deleted,
			"skipped_live", skippedLive,
			"cutoff_ms", cutoff)
	}
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
