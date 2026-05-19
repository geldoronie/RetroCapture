package store

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"
)

// newTestStore opens a fresh on-disk SQLite under t.TempDir so each
// test gets isolated state. In-memory ":memory:" works for single
// connections but our store uses the default pool, so on-disk is
// safer.
func newTestStore(t *testing.T) *Store {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.db")
	s, err := Open(path)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}

func sampleStream(id string) Stream {
	now := time.Now().UTC().Truncate(time.Second)
	return Stream{
		StreamID:         id,
		OwnerToken:       "token-" + id,
		Name:             "Test stream " + id,
		HostNickname:     "alice",
		Shader:           "crt/test.glslp",
		ResolutionW:      1920,
		ResolutionH:      1080,
		FPS:              60,
		Codec:            "h264",
		PasswordRequired: false,
		Endpoint:         "https://example.com/" + id,
		EndpointMode:     "direct",
		ClientCount:      0,
		PublicIP:         "1.2.3.4",
		Version:          "0.7.0-alpha",
		RegisteredAt:     now,
		LastHeartbeatAt:  now,
		ExpiresAt:        now.Add(2 * time.Minute),
	}
}

func TestInsertAndGet(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()

	in := sampleStream("a")
	if err := s.Insert(ctx, in); err != nil {
		t.Fatalf("insert: %v", err)
	}

	out, err := s.Get(ctx, "a")
	if err != nil {
		t.Fatalf("get: %v", err)
	}
	if out.Name != in.Name || out.OwnerToken != in.OwnerToken || out.PasswordRequired != in.PasswordRequired {
		t.Fatalf("roundtrip mismatch: in=%+v out=%+v", in, out)
	}
	if !out.RegisteredAt.Equal(in.RegisteredAt) {
		t.Fatalf("registered_at mismatch: in=%v out=%v", in.RegisteredAt, out.RegisteredAt)
	}
}

func TestGetNotFound(t *testing.T) {
	s := newTestStore(t)
	_, err := s.Get(context.Background(), "nope")
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("want ErrNotFound, got %v", err)
	}
}

func TestHeartbeatHappyPath(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	in := sampleStream("hb")
	in.ExpiresAt = time.Now().Add(1 * time.Minute).UTC()
	if err := s.Insert(ctx, in); err != nil {
		t.Fatalf("insert: %v", err)
	}

	newExp := time.Now().Add(5 * time.Minute).UTC().Truncate(time.Second)
	cc := 7
	if err := s.Heartbeat(ctx, "hb", "token-hb", &cc, newExp); err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	out, _ := s.Get(ctx, "hb")
	if !out.ExpiresAt.Equal(newExp) {
		t.Fatalf("expires_at not refreshed: got %v want %v", out.ExpiresAt, newExp)
	}
	if out.ClientCount != 7 {
		t.Fatalf("client_count not updated: got %d", out.ClientCount)
	}
}

func TestHeartbeatTokenMismatch(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	_ = s.Insert(ctx, sampleStream("h2"))
	err := s.Heartbeat(ctx, "h2", "wrong", nil, time.Now().Add(time.Minute))
	if !errors.Is(err, ErrForbidden) {
		t.Fatalf("want ErrForbidden, got %v", err)
	}
}

func TestHeartbeatNotFound(t *testing.T) {
	s := newTestStore(t)
	err := s.Heartbeat(context.Background(), "missing", "x", nil, time.Now())
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("want ErrNotFound, got %v", err)
	}
}

func TestApplyPatchSubset(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	_ = s.Insert(ctx, sampleStream("p"))

	newShader := "crt/other.glslp"
	pwReq := true
	if err := s.ApplyPatch(ctx, "p", "token-p", Patch{Shader: &newShader, PasswordRequired: &pwReq}); err != nil {
		t.Fatalf("patch: %v", err)
	}
	out, _ := s.Get(ctx, "p")
	if out.Shader != newShader || !out.PasswordRequired {
		t.Fatalf("patch didn't apply: %+v", out)
	}
	// Untouched fields must remain.
	if out.Name == "" || out.Endpoint == "" {
		t.Fatalf("patch nuked unrelated fields: %+v", out)
	}
}

func TestApplyPatchEmptyIsNoop(t *testing.T) {
	s := newTestStore(t)
	_ = s.Insert(context.Background(), sampleStream("e"))
	err := s.ApplyPatch(context.Background(), "e", "token-e", Patch{})
	if err != nil {
		t.Fatalf("empty patch should be no-op, got %v", err)
	}
}

func TestApplyPatchForbidden(t *testing.T) {
	s := newTestStore(t)
	_ = s.Insert(context.Background(), sampleStream("f"))
	newShader := "x"
	err := s.ApplyPatch(context.Background(), "f", "wrong", Patch{Shader: &newShader})
	if !errors.Is(err, ErrForbidden) {
		t.Fatalf("want ErrForbidden, got %v", err)
	}
}

func TestDeleteIdempotent(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	_ = s.Insert(ctx, sampleStream("d"))
	if err := s.Delete(ctx, "d", "token-d"); err != nil {
		t.Fatalf("delete: %v", err)
	}
	// Second delete is a no-op.
	if err := s.Delete(ctx, "d", "token-d"); err != nil {
		t.Fatalf("idempotent delete: %v", err)
	}
}

func TestDeleteForbidden(t *testing.T) {
	s := newTestStore(t)
	_ = s.Insert(context.Background(), sampleStream("df"))
	err := s.Delete(context.Background(), "df", "wrong")
	if !errors.Is(err, ErrForbidden) {
		t.Fatalf("want ErrForbidden, got %v", err)
	}
}

func TestListSortDefaultClients(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	a := sampleStream("a")
	a.ClientCount = 5
	b := sampleStream("b")
	b.ClientCount = 10
	c := sampleStream("c")
	c.ClientCount = 1
	_ = s.Insert(ctx, a)
	_ = s.Insert(ctx, b)
	_ = s.Insert(ctx, c)

	got, total, err := s.List(ctx, ListOptions{})
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if total != 3 {
		t.Fatalf("total = %d, want 3", total)
	}
	want := []string{"b", "a", "c"}
	for i, st := range got {
		if st.StreamID != want[i] {
			t.Fatalf("position %d: got %q want %q", i, st.StreamID, want[i])
		}
	}
}

func TestListSortRecent(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	base := time.Now().UTC().Truncate(time.Second)
	for i, id := range []string{"old", "new", "mid"} {
		st := sampleStream(id)
		st.LastHeartbeatAt = base.Add(time.Duration(i) * time.Second)
		_ = s.Insert(ctx, st)
	}
	// indices: old=0, new=1, mid=2. Recent sort wants 2,1,0 → mid, new, old.
	got, _, err := s.List(ctx, ListOptions{Sort: "recent"})
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	want := []string{"mid", "new", "old"}
	for i, st := range got {
		if st.StreamID != want[i] {
			t.Fatalf("position %d: got %q want %q", i, st.StreamID, want[i])
		}
	}
}

func TestListQueryFilter(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	a := sampleStream("a")
	a.Name = "CRT Mattias"
	b := sampleStream("b")
	b.Name = "NTSC Easy"
	b.HostNickname = "crt-fan"
	c := sampleStream("c")
	c.Name = "Plain"
	_ = s.Insert(ctx, a)
	_ = s.Insert(ctx, b)
	_ = s.Insert(ctx, c)

	got, total, err := s.List(ctx, ListOptions{Query: "crt"})
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if total != 2 {
		t.Fatalf("total filtered = %d, want 2 (a by name, b by nickname)", total)
	}
	if len(got) != 2 {
		t.Fatalf("len = %d, want 2", len(got))
	}
}

func TestListInvalidSort(t *testing.T) {
	s := newTestStore(t)
	_, _, err := s.List(context.Background(), ListOptions{Sort: "bogus"})
	if err == nil {
		t.Fatal("want error for invalid sort")
	}
}

func TestDeleteExpired(t *testing.T) {
	s := newTestStore(t)
	ctx := context.Background()
	now := time.Now().UTC().Truncate(time.Second)
	old := sampleStream("old")
	old.ExpiresAt = now.Add(-1 * time.Minute)
	fresh := sampleStream("fresh")
	fresh.ExpiresAt = now.Add(5 * time.Minute)
	_ = s.Insert(ctx, old)
	_ = s.Insert(ctx, fresh)

	n, err := s.DeleteExpired(ctx, now)
	if err != nil {
		t.Fatalf("delete expired: %v", err)
	}
	if n != 1 {
		t.Fatalf("deleted = %d, want 1", n)
	}
	if _, err := s.Get(ctx, "old"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("old should be gone, got %v", err)
	}
	if _, err := s.Get(ctx, "fresh"); err != nil {
		t.Fatalf("fresh should remain, got %v", err)
	}
}

func TestInsertReport(t *testing.T) {
	s := newTestStore(t)
	if err := s.InsertReport(context.Background(), "anyid", "R-AB12CD34", "9.9.9.9", "spam", "alice@example"); err != nil {
		t.Fatalf("insert report: %v", err)
	}
}

func TestMigrationsAreIdempotent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "idem.db")
	s1, err := Open(path)
	if err != nil {
		t.Fatalf("open 1: %v", err)
	}
	_ = s1.Insert(context.Background(), sampleStream("x"))
	_ = s1.Close()

	s2, err := Open(path)
	if err != nil {
		t.Fatalf("open 2: %v", err)
	}
	defer s2.Close()
	if _, err := s2.Get(context.Background(), "x"); err != nil {
		t.Fatalf("data lost on reopen: %v", err)
	}
}
