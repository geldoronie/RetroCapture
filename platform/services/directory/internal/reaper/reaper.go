// Package reaper periodically deletes expired entries from the
// directory store. The directory's TTL semantics rely on this — an
// entry survives `lastHeartbeatAt + TTL` and is then garbage; the
// reaper is what makes the garbage actually disappear.
package reaper

import (
	"context"
	"log/slog"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

// Reaper sweeps the store on Interval. Start spawns a goroutine that
// runs until the provided context is cancelled.
type Reaper struct {
	Store    *store.Store
	Logger   *slog.Logger
	Interval time.Duration // default 60s if zero
}

// Start launches the background loop. Returns immediately; the loop
// stops when ctx is done.
func (r *Reaper) Start(ctx context.Context) {
	interval := r.Interval
	if interval <= 0 {
		interval = 60 * time.Second
	}

	go func() {
		// First sweep happens after Interval, not immediately, so a
		// fresh start doesn't fight with the startup INSERTs.
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				r.Logger.Info("reaper_stopping")
				return
			case <-t.C:
				r.sweep(ctx)
			}
		}
	}()
}

func (r *Reaper) sweep(ctx context.Context) {
	now := time.Now().UTC()
	n, err := r.Store.DeleteExpired(ctx, now)
	if err != nil {
		r.Logger.Error("reaper_sweep_failed", "err", err)
		return
	}
	if n > 0 {
		r.Logger.Info("reaper_swept", "deleted", n)
	}
}
