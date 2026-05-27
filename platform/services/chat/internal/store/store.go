// Package store wraps the SQLite database used by the chat service.
// Schema changes happen via embedded migrations applied at Open()
// time; the rest of the package exposes typed CRUD methods that the
// API handlers and the room hub call.
//
// We use modernc.org/sqlite (pure-Go SQLite, no CGO) so the build
// stays static and the scratch container needs no shared libraries.
// Mirror of platform/services/directory/internal/store.
package store

import (
	"context"
	"database/sql"
	"embed"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	// modernc.org/sqlite registers the "sqlite" driver via init().
	_ "modernc.org/sqlite"
)

//go:embed migrations/*.sql
var migrationsFS embed.FS

// Sentinel errors. Handlers translate these into HTTP status codes.
var (
	ErrNotFound = errors.New("store: room not found")
)

// RoomKind enumerates the two flavours of room. Stream-linked rooms
// auto-spawn alongside a directory stream and are owned by the host;
// standalone rooms (v1+ UI) are user-created persistent channels.
type RoomKind string

const (
	RoomKindStreamLinked RoomKind = "stream_linked"
	RoomKindStandalone   RoomKind = "standalone"
)

// Room mirrors the chat_rooms row.
type Room struct {
	ID              string
	Kind            RoomKind
	LinkedStreamID  string // empty for standalone
	OwnerAccountID  string // empty in v0.5 (no accounts yet)
	Slug            string // empty for stream-linked rooms
	Title           string
	Description     string
	SettingsJSON    string
	CreatedAt       time.Time
	ArchivedAt      *time.Time // nil while active
}

// Message mirrors the chat_messages row.
type Message struct {
	ID            string
	RoomID        string
	ParticipantID string
	Nickname      string
	Body          string
	PostedAt      time.Time
	DeletedAt     *time.Time
	IsHost        bool
}

// Store is the public handle. Safe for concurrent use; SQLite
// serialises internally and our prepared statements don't share state.
type Store struct {
	db *sql.DB
}

// Open opens (or creates) a SQLite database at the given path, applies
// all migrations, and returns the Store. `:memory:` is accepted for
// tests.
func Open(path string) (*Store, error) {
	dsn := path
	if !strings.Contains(dsn, "?") {
		// busy_timeout: block up to 5 s waiting for a writer instead
		// of failing with SQLITE_BUSY.
		// _journal_mode=WAL: more concurrent readers; matches what
		// the directory service uses.
		dsn += "?_busy_timeout=5000&_journal_mode=WAL"
	}
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("sql.Open: %w", err)
	}
	if err := db.Ping(); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("ping: %w", err)
	}
	s := &Store{db: db}
	if err := s.applyMigrations(); err != nil {
		_ = db.Close()
		return nil, err
	}
	return s, nil
}

// Close releases the underlying SQL handle.
func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) applyMigrations() error {
	if _, err := s.db.Exec(`CREATE TABLE IF NOT EXISTS schema_migrations (
        version TEXT PRIMARY KEY,
        applied_at INTEGER NOT NULL
    )`); err != nil {
		return fmt.Errorf("create schema_migrations: %w", err)
	}

	entries, err := migrationsFS.ReadDir("migrations")
	if err != nil {
		return fmt.Errorf("read migrations dir: %w", err)
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".sql") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)

	applied := map[string]bool{}
	rows, err := s.db.Query("SELECT version FROM schema_migrations")
	if err != nil {
		return fmt.Errorf("select schema_migrations: %w", err)
	}
	for rows.Next() {
		var v string
		if err := rows.Scan(&v); err != nil {
			_ = rows.Close()
			return err
		}
		applied[v] = true
	}
	_ = rows.Close()

	for _, name := range names {
		if applied[name] {
			continue
		}
		body, err := migrationsFS.ReadFile("migrations/" + name)
		if err != nil {
			return fmt.Errorf("read migration %s: %w", name, err)
		}
		tx, err := s.db.Begin()
		if err != nil {
			return err
		}
		if _, err := tx.Exec(string(body)); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("apply %s: %w", name, err)
		}
		if _, err := tx.Exec(
			"INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)",
			name, time.Now().Unix(),
		); err != nil {
			_ = tx.Rollback()
			return fmt.Errorf("record %s: %w", name, err)
		}
		if err := tx.Commit(); err != nil {
			return fmt.Errorf("commit %s: %w", name, err)
		}
	}
	return nil
}

// GetRoom returns the room by id. ErrNotFound if missing.
func (s *Store) GetRoom(ctx context.Context, id string) (*Room, error) {
	row := s.db.QueryRowContext(ctx, `
        SELECT id, kind, COALESCE(linked_stream_id, ''),
               COALESCE(owner_account_id, ''), COALESCE(slug, ''),
               title, description, settings_json,
               created_at_ms, archived_at_ms
          FROM chat_rooms
         WHERE id = ?
    `, id)
	r := &Room{}
	var (
		createdMs  int64
		archivedMs sql.NullInt64
		kind       string
	)
	if err := row.Scan(
		&r.ID, &kind, &r.LinkedStreamID, &r.OwnerAccountID, &r.Slug,
		&r.Title, &r.Description, &r.SettingsJSON,
		&createdMs, &archivedMs,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	r.Kind = RoomKind(kind)
	r.CreatedAt = time.UnixMilli(createdMs).UTC()
	if archivedMs.Valid {
		t := time.UnixMilli(archivedMs.Int64).UTC()
		r.ArchivedAt = &t
	}
	return r, nil
}

// GetOrCreateRoomForStream looks up the stream-linked room for the
// given stream id, creating it if missing. The `created` return
// value is true iff this call inserted the row.
func (s *Store) GetOrCreateRoomForStream(
	ctx context.Context,
	streamID, generatedRoomID, title string,
) (*Room, bool, error) {
	row := s.db.QueryRowContext(ctx, `
        SELECT id FROM chat_rooms WHERE linked_stream_id = ? LIMIT 1
    `, streamID)
	var existingID string
	switch err := row.Scan(&existingID); {
	case err == nil:
		r, err := s.GetRoom(ctx, existingID)
		return r, false, err
	case errors.Is(err, sql.ErrNoRows):
		// fallthrough — create.
	default:
		return nil, false, err
	}

	now := time.Now().UTC()
	if _, err := s.db.ExecContext(ctx, `
        INSERT INTO chat_rooms(
            id, kind, linked_stream_id, owner_account_id, slug,
            title, description, settings_json,
            created_at_ms, archived_at_ms
        )
        VALUES (?, ?, ?, NULL, NULL, ?, '', '{}', ?, NULL)
    `, generatedRoomID, string(RoomKindStreamLinked), streamID, title,
		now.UnixMilli()); err != nil {
		return nil, false, fmt.Errorf("insert room: %w", err)
	}
	r, err := s.GetRoom(ctx, generatedRoomID)
	if err != nil {
		return nil, false, err
	}
	return r, true, nil
}

// InsertMessage records a chat message. Caller supplies a pre-
// generated id so the broadcast and the persistence share the same
// value (the hub fanout pre-computes the wire message with its id
// before durably storing it).
func (s *Store) InsertMessage(ctx context.Context, m *Message) error {
	if m.PostedAt.IsZero() {
		m.PostedAt = time.Now().UTC()
	}
	_, err := s.db.ExecContext(ctx, `
        INSERT INTO chat_messages(
            id, room_id, participant_id, nickname, body,
            posted_at_ms, deleted_at_ms, is_host
        ) VALUES (?, ?, ?, ?, ?, ?, NULL, ?)
    `, m.ID, m.RoomID, m.ParticipantID, m.Nickname, m.Body,
		m.PostedAt.UnixMilli(), boolToInt(m.IsHost))
	return err
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// SoftDeleteMessage marks a message as deleted. Returns ErrNotFound
// if the id doesn't exist (or is already deleted).
func (s *Store) SoftDeleteMessage(ctx context.Context, roomID, messageID string) error {
	res, err := s.db.ExecContext(ctx, `
        UPDATE chat_messages
           SET deleted_at_ms = ?
         WHERE id = ? AND room_id = ? AND deleted_at_ms IS NULL
    `, time.Now().UnixMilli(), messageID, roomID)
	if err != nil {
		return err
	}
	n, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if n == 0 {
		return ErrNotFound
	}
	return nil
}

// ListMessages returns up to `limit` messages from `roomID`, newest
// first. If `beforeCursor` is non-empty, only messages older than
// the cursored row are returned (cursor is the previous page's last
// message id). The second return value is the cursor to use for the
// next page; empty when no older rows exist.
func (s *Store) ListMessages(
	ctx context.Context,
	roomID, beforeCursor string,
	limit int,
) ([]Message, string, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	// Two-step pagination: if a cursor is given, look up its
	// posted_at_ms / id and use them as the strict-less-than gate.
	var (
		hasBefore  bool
		beforeMs   int64
		beforeIDOk string
	)
	if beforeCursor != "" {
		row := s.db.QueryRowContext(ctx, `
            SELECT posted_at_ms, id FROM chat_messages
             WHERE id = ? AND room_id = ?
        `, beforeCursor, roomID)
		if err := row.Scan(&beforeMs, &beforeIDOk); err != nil {
			if errors.Is(err, sql.ErrNoRows) {
				// Treat an unknown cursor as "from the top".
			} else {
				return nil, "", err
			}
		} else {
			hasBefore = true
		}
	}

	var (
		rows *sql.Rows
		err  error
	)
	if hasBefore {
		rows, err = s.db.QueryContext(ctx, `
            SELECT id, room_id, participant_id, nickname, body,
                   posted_at_ms, deleted_at_ms, is_host
              FROM chat_messages
             WHERE room_id = ?
               AND (posted_at_ms, id) < (?, ?)
             ORDER BY posted_at_ms DESC, id DESC
             LIMIT ?
        `, roomID, beforeMs, beforeIDOk, limit+1)
	} else {
		rows, err = s.db.QueryContext(ctx, `
            SELECT id, room_id, participant_id, nickname, body,
                   posted_at_ms, deleted_at_ms, is_host
              FROM chat_messages
             WHERE room_id = ?
             ORDER BY posted_at_ms DESC, id DESC
             LIMIT ?
        `, roomID, limit+1)
	}
	if err != nil {
		return nil, "", err
	}
	defer rows.Close()

	out := make([]Message, 0, limit)
	for rows.Next() {
		var (
			m         Message
			postedMs  int64
			deletedMs sql.NullInt64
			isHost    int
		)
		if err := rows.Scan(
			&m.ID, &m.RoomID, &m.ParticipantID, &m.Nickname, &m.Body,
			&postedMs, &deletedMs, &isHost,
		); err != nil {
			return nil, "", err
		}
		m.PostedAt = time.UnixMilli(postedMs).UTC()
		if deletedMs.Valid {
			t := time.UnixMilli(deletedMs.Int64).UTC()
			m.DeletedAt = &t
		}
		m.IsHost = isHost != 0
		out = append(out, m)
	}
	if err := rows.Err(); err != nil {
		return nil, "", err
	}

	// We over-fetched by 1 to detect whether more remain.
	nextCursor := ""
	if len(out) > limit {
		nextCursor = out[limit-1].ID
		out = out[:limit]
	}
	return out, nextCursor, nil
}

// CountActiveRooms returns the number of chat rooms not yet archived.
// Used by health / metrics.
func (s *Store) CountActiveRooms(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `
        SELECT COUNT(*) FROM chat_rooms WHERE archived_at_ms IS NULL
    `).Scan(&n); err != nil {
		return 0, err
	}
	return n, nil
}
