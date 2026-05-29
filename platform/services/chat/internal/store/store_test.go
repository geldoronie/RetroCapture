package store

import (
	"context"
	"path/filepath"
	"testing"
	"time"
)

// openTempStore opens a fresh sqlite DB under t.TempDir so each test
// gets an isolated schema (migrations run on Open).
func openTempStore(t *testing.T) *Store {
	t.Helper()
	dir := t.TempDir()
	s, err := Open(filepath.Join(dir, "chat.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}

func TestCreateStandalone_IsStreamRoomRoundTrip(t *testing.T) {
	s := openTempStore(t)
	ctx := context.Background()

	cases := []struct {
		name        string
		isStreamRoom bool
	}{
		{"user-created", false},
		{"stream-provisioned", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rid := "r_" + tc.name
			r, err := s.CreateStandaloneRoom(ctx, rid, "slug-"+tc.name,
				"Title "+tc.name,
				StandaloneRoomOptions{
					OwnerSecretHash: "fakehash",
					Listed:          true,
					IsStreamRoom:    tc.isStreamRoom,
				})
			if err != nil {
				t.Fatalf("create: %v", err)
			}
			if r.IsStreamRoom != tc.isStreamRoom {
				t.Errorf("after create: IsStreamRoom = %v, want %v",
					r.IsStreamRoom, tc.isStreamRoom)
			}
			again, err := s.GetRoom(ctx, rid)
			if err != nil {
				t.Fatalf("get: %v", err)
			}
			if again.IsStreamRoom != tc.isStreamRoom {
				t.Errorf("after fetch: IsStreamRoom = %v, want %v",
					again.IsStreamRoom, tc.isStreamRoom)
			}
		})
	}
}

func TestSetRoomListed_TogglesAndAffectsListing(t *testing.T) {
	s := openTempStore(t)
	ctx := context.Background()

	rid := "r_listing"
	if _, err := s.CreateStandaloneRoom(ctx, rid, "list-test", "T",
		StandaloneRoomOptions{
			OwnerSecretHash: "h", Listed: false,
		}); err != nil {
		t.Fatalf("create: %v", err)
	}
	visible := func() bool {
		rooms, err := s.ListPublicRooms(ctx, 50)
		if err != nil {
			t.Fatalf("list: %v", err)
		}
		for _, r := range rooms {
			if r.ID == rid {
				return true
			}
		}
		return false
	}
	if visible() {
		t.Fatal("expected unlisted room to be hidden")
	}
	if err := s.SetRoomListed(ctx, rid, true); err != nil {
		t.Fatalf("set listed=true: %v", err)
	}
	if !visible() {
		t.Fatal("expected listed=true room to appear")
	}
	if err := s.SetRoomListed(ctx, rid, false); err != nil {
		t.Fatalf("set listed=false: %v", err)
	}
	if visible() {
		t.Fatal("expected listed=false flip to hide the room again")
	}
}

func TestDeleteRoom_CascadesMessages(t *testing.T) {
	s := openTempStore(t)
	ctx := context.Background()

	rid := "r_cascade"
	if _, err := s.CreateStandaloneRoom(ctx, rid, "cascade", "T",
		StandaloneRoomOptions{
			OwnerSecretHash: "h", Listed: true,
		}); err != nil {
		t.Fatalf("create: %v", err)
	}
	if err := s.InsertMessage(ctx, &Message{
		ID: "m_1", RoomID: rid, Nickname: "tester", Body: "hi",
		PostedAt: time.Now().UTC(),
	}); err != nil {
		t.Fatalf("insert msg: %v", err)
	}
	if err := s.DeleteRoom(ctx, rid); err != nil {
		t.Fatalf("delete: %v", err)
	}
	if _, err := s.GetRoom(ctx, rid); err == nil {
		t.Fatal("expected ErrNotFound after delete")
	}
	// Messages table should be empty for that room.
	row := s.db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM chat_messages WHERE room_id = ?`, rid)
	var n int
	if err := row.Scan(&n); err != nil {
		t.Fatalf("scan count: %v", err)
	}
	if n != 0 {
		t.Errorf("messages count after cascade = %d, want 0", n)
	}
}

func TestListInactiveRoomIDs_FiltersByCutoff(t *testing.T) {
	s := openTempStore(t)
	ctx := context.Background()

	// Fresh room — last_activity_ms = now.
	freshID := "r_fresh"
	if _, err := s.CreateStandaloneRoom(ctx, freshID, "fresh", "T",
		StandaloneRoomOptions{
			OwnerSecretHash: "h", Listed: true,
		}); err != nil {
		t.Fatalf("create fresh: %v", err)
	}
	// Stale room — backdated.
	staleID := "r_stale"
	if _, err := s.CreateStandaloneRoom(ctx, staleID, "stale", "T",
		StandaloneRoomOptions{
			OwnerSecretHash: "h", Listed: true,
		}); err != nil {
		t.Fatalf("create stale: %v", err)
	}
	oldMs := time.Now().Add(-72 * time.Hour).UnixMilli()
	if _, err := s.db.ExecContext(ctx,
		`UPDATE chat_rooms SET last_activity_ms = ? WHERE id = ?`,
		oldMs, staleID); err != nil {
		t.Fatalf("backdate: %v", err)
	}
	cutoff := time.Now().Add(-1 * time.Hour).UnixMilli()
	ids, err := s.ListInactiveRoomIDs(ctx, cutoff)
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	var sawStale, sawFresh bool
	for _, id := range ids {
		if id == staleID {
			sawStale = true
		}
		if id == freshID {
			sawFresh = true
		}
	}
	if !sawStale {
		t.Error("stale room missing from inactive listing")
	}
	if sawFresh {
		t.Error("fresh room should not appear in inactive listing")
	}
}

func TestTouchRoomActivity_BumpsTimestamp(t *testing.T) {
	s := openTempStore(t)
	ctx := context.Background()

	rid := "r_touch"
	if _, err := s.CreateStandaloneRoom(ctx, rid, "touch", "T",
		StandaloneRoomOptions{
			OwnerSecretHash: "h", Listed: true,
		}); err != nil {
		t.Fatalf("create: %v", err)
	}
	getActivity := func() int64 {
		var v int64
		_ = s.db.QueryRowContext(ctx,
			`SELECT last_activity_ms FROM chat_rooms WHERE id = ?`,
			rid).Scan(&v)
		return v
	}
	before := getActivity()
	// Sleep just enough for the millisecond clock to advance.
	time.Sleep(5 * time.Millisecond)
	if err := s.TouchRoomActivity(ctx, rid); err != nil {
		t.Fatalf("touch: %v", err)
	}
	after := getActivity()
	if after <= before {
		t.Errorf("after touch: %d <= before %d", after, before)
	}
}
