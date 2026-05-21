package reaper

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"path/filepath"
	"testing"
	"time"

	"github.com/geldoronie/RetroCapture/platform/services/directory/internal/store"
)

func newStore(t *testing.T) *store.Store {
	t.Helper()
	s, err := store.Open(filepath.Join(t.TempDir(), "reaper.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}

func sample(id string, exp time.Time) store.Stream {
	now := time.Now().UTC().Truncate(time.Second)
	return store.Stream{
		StreamID:        id,
		OwnerToken:      "tok-" + id,
		Name:            id,
		ResolutionW:     640,
		ResolutionH:     480,
		FPS:             60,
		Codec:           "h264",
		Endpoint:        "https://x/" + id,
		EndpointMode:    "direct",
		RegisteredAt:    now,
		LastHeartbeatAt: now,
		ExpiresAt:       exp,
	}
}

// TestReaperDeletesExpired wires a Reaper with a 50 ms sweep so the
// first tick fires inside the test budget. Verifies an old entry is
// gone and a fresh one survives.
func TestReaperDeletesExpired(t *testing.T) {
	st := newStore(t)
	ctx := context.Background()

	now := time.Now().UTC()
	_ = st.Insert(ctx, sample("old", now.Add(-1*time.Minute)))
	_ = st.Insert(ctx, sample("fresh", now.Add(5*time.Minute)))

	rp := &Reaper{
		Store:    st,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		Interval: 50 * time.Millisecond,
	}
	loopCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	rp.Start(loopCtx)

	// Wait long enough for at least one sweep tick.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		_, err := st.Get(ctx, "old")
		if errors.Is(err, store.ErrNotFound) {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}

	if _, err := st.Get(ctx, "old"); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("expected 'old' to be reaped, got err=%v", err)
	}
	if _, err := st.Get(ctx, "fresh"); err != nil {
		t.Fatalf("expected 'fresh' to remain, got err=%v", err)
	}
}

// TestReaperStopsOnContextCancel makes sure cancelling the context
// actually stops the goroutine — otherwise a misbehaving service
// would leak goroutines on shutdown.
func TestReaperStopsOnContextCancel(t *testing.T) {
	st := newStore(t)
	rp := &Reaper{
		Store:    st,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		Interval: 10 * time.Millisecond,
	}
	ctx, cancel := context.WithCancel(context.Background())
	rp.Start(ctx)

	// Let it tick a couple of times.
	time.Sleep(50 * time.Millisecond)
	cancel()

	// Insert an expired entry AFTER cancelling. If the goroutine is
	// still running it would reap this and we'd never see it.
	// Tolerance: another sweep would have happened by the wait below
	// if cancel didn't take effect.
	_ = st.Insert(context.Background(), sample("post", time.Now().Add(-1*time.Minute)))
	time.Sleep(100 * time.Millisecond)
	if _, err := st.Get(context.Background(), "post"); errors.Is(err, store.ErrNotFound) {
		t.Fatal("goroutine kept running after ctx cancel — reaped 'post' which we inserted after cancel")
	}
}
