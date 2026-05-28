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
	OwnerAccountID  string // v0.5 reused to store the creator's rc_<...> client_id
	Slug            string // empty for stream-linked rooms
	Title           string
	Description     string
	SettingsJSON    string
	PasswordHash    string // sha256-hex of plaintext, empty == no password
	OwnerSecretHash string // sha256-hex of plaintext owner secret, empty == none
	Listed          bool   // public-listing opt-in (defaults true at create)
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
	IsOwner       bool
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
               COALESCE(password_hash, ''),
               COALESCE(owner_secret_hash, ''), listed,
               created_at_ms, archived_at_ms
          FROM chat_rooms
         WHERE id = ?
    `, id)
	r := &Room{}
	var (
		createdMs  int64
		archivedMs sql.NullInt64
		kind       string
		listedInt  int
	)
	if err := row.Scan(
		&r.ID, &kind, &r.LinkedStreamID, &r.OwnerAccountID, &r.Slug,
		&r.Title, &r.Description, &r.SettingsJSON,
		&r.PasswordHash, &r.OwnerSecretHash, &listedInt,
		&createdMs, &archivedMs,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	r.Kind = RoomKind(kind)
	r.Listed = listedInt != 0
	r.CreatedAt = time.UnixMilli(createdMs).UTC()
	if archivedMs.Valid {
		t := time.UnixMilli(archivedMs.Int64).UTC()
		r.ArchivedAt = &t
	}
	return r, nil
}

// ListPublicRooms returns rooms eligible for the in-app browser,
// most-recently-created first. Two kinds qualify:
//
//   - Standalone rooms with `listed = 1` — explicit opt-in.
//   - Stream-linked rooms (always) — they exist iff someone has
//     touched the chat for a live stream, and the stream itself
//     is by definition public. The caller (handlers.go) is
//     expected to filter these by live participant count so dead
//     streams' chat rooms don't pollute the listing.
//
// limit is clamped to [1, 200], default 50.
func (s *Store) ListPublicRooms(ctx context.Context, limit int) ([]Room, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	rows, err := s.db.QueryContext(ctx, `
        SELECT id, kind, COALESCE(linked_stream_id, ''),
               COALESCE(owner_account_id, ''), COALESCE(slug, ''),
               title, description, settings_json,
               COALESCE(password_hash, ''),
               COALESCE(owner_secret_hash, ''), listed,
               created_at_ms, archived_at_ms
          FROM chat_rooms
         WHERE archived_at_ms IS NULL
           AND ((kind = ? AND listed = 1)
                OR kind = ?)
         ORDER BY created_at_ms DESC
         LIMIT ?
    `, string(RoomKindStandalone), string(RoomKindStreamLinked), limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make([]Room, 0, limit)
	for rows.Next() {
		var (
			r          Room
			createdMs  int64
			archivedMs sql.NullInt64
			kind       string
			listedInt  int
		)
		if err := rows.Scan(
			&r.ID, &kind, &r.LinkedStreamID, &r.OwnerAccountID, &r.Slug,
			&r.Title, &r.Description, &r.SettingsJSON,
			&r.PasswordHash, &r.OwnerSecretHash, &listedInt,
			&createdMs, &archivedMs,
		); err != nil {
			return nil, err
		}
		r.Kind = RoomKind(kind)
		r.Listed = listedInt != 0
		r.CreatedAt = time.UnixMilli(createdMs).UTC()
		if archivedMs.Valid {
			t := time.UnixMilli(archivedMs.Int64).UTC()
			r.ArchivedAt = &t
		}
		out = append(out, r)
	}
	return out, rows.Err()
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

// GetRoomBySlug resolves a standalone room by its human-readable
// slug. ErrNotFound if the slug isn't taken.
func (s *Store) GetRoomBySlug(ctx context.Context, slug string) (*Room, error) {
	row := s.db.QueryRowContext(ctx, `
        SELECT id FROM chat_rooms WHERE slug = ? LIMIT 1
    `, slug)
	var id string
	if err := row.Scan(&id); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	return s.GetRoom(ctx, id)
}

// ErrSlugTaken is returned by CreateStandaloneRoom when the
// requested slug already exists. Callers map it to HTTP 409.
var ErrSlugTaken = errors.New("slug already in use")

// StandaloneRoomOptions wraps the optional knobs the creator can
// set: ownerClientID (their rc_<...>), passwordHash (sha256 hex of
// the plaintext; empty == no password), and listed (whether the
// room appears in GET /rooms). The Title/Slug live as positional
// args because they're always required.
type StandaloneRoomOptions struct {
	OwnerClientID   string
	PasswordHash    string
	OwnerSecretHash string
	Listed          bool
}

// CreateStandaloneRoom inserts a kind=standalone row with the given
// slug + title + optional settings. Returns ErrSlugTaken when slug
// is already taken. The caller is expected to have validated slug
// shape and hashed the password ahead of time.
func (s *Store) CreateStandaloneRoom(
	ctx context.Context,
	roomID, slug, title string,
	opts StandaloneRoomOptions,
) (*Room, error) {
	// Pre-check so we can return a typed error instead of relying on
	// the SQLite UNIQUE-constraint error text.
	if existing, err := s.GetRoomBySlug(ctx, slug); err == nil && existing != nil {
		return nil, ErrSlugTaken
	} else if err != nil && !errors.Is(err, ErrNotFound) {
		return nil, err
	}

	now := time.Now().UTC()

	// owner_account_id v0.5 carries the creator's rc_<id>. password_hash
	// stays NULL when no password was set. listed defaults true at the
	// schema level but we pass through explicitly so the caller can
	// flip it off via the create form.
	var (
		ownerArg  interface{} = nil
		passArg   interface{} = nil
		secretArg interface{} = nil
	)
	if opts.OwnerClientID != "" {
		ownerArg = opts.OwnerClientID
	}
	if opts.PasswordHash != "" {
		passArg = opts.PasswordHash
	}
	if opts.OwnerSecretHash != "" {
		secretArg = opts.OwnerSecretHash
	}
	listedInt := 1
	if !opts.Listed {
		listedInt = 0
	}

	if _, err := s.db.ExecContext(ctx, `
        INSERT INTO chat_rooms(
            id, kind, linked_stream_id, owner_account_id, slug,
            title, description, settings_json,
            password_hash, owner_secret_hash, listed,
            created_at_ms, archived_at_ms
        )
        VALUES (?, ?, NULL, ?, ?, ?, '', '{}', ?, ?, ?, ?, NULL)
    `, roomID, string(RoomKindStandalone), ownerArg, slug, title,
		passArg, secretArg, listedInt, now.UnixMilli()); err != nil {
		return nil, fmt.Errorf("insert standalone room: %w", err)
	}
	return s.GetRoom(ctx, roomID)
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
            posted_at_ms, deleted_at_ms, is_host, is_owner
        ) VALUES (?, ?, ?, ?, ?, ?, NULL, ?, ?)
    `, m.ID, m.RoomID, m.ParticipantID, m.Nickname, m.Body,
		m.PostedAt.UnixMilli(), boolToInt(m.IsHost), boolToInt(m.IsOwner))
	return err
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// DeleteRoom removes the room AND all of its messages from the
// database — true cascade delete; no archived_at marker, no soft
// remnants. Used by the owner-initiated delete flow (#84). The
// caller is responsible for evicting the live room from the in-
// memory registry separately.
func (s *Store) DeleteRoom(ctx context.Context, roomID string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()
	if _, err := tx.ExecContext(ctx,
		`DELETE FROM chat_messages WHERE room_id = ?`, roomID); err != nil {
		return err
	}
	res, err := tx.ExecContext(ctx,
		`DELETE FROM chat_rooms WHERE id = ?`, roomID)
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
	return tx.Commit()
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
                   posted_at_ms, deleted_at_ms, is_host, is_owner
              FROM chat_messages
             WHERE room_id = ?
               AND (posted_at_ms, id) < (?, ?)
             ORDER BY posted_at_ms DESC, id DESC
             LIMIT ?
        `, roomID, beforeMs, beforeIDOk, limit+1)
	} else {
		rows, err = s.db.QueryContext(ctx, `
            SELECT id, room_id, participant_id, nickname, body,
                   posted_at_ms, deleted_at_ms, is_host, is_owner
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
			isOwner   int
		)
		if err := rows.Scan(
			&m.ID, &m.RoomID, &m.ParticipantID, &m.Nickname, &m.Body,
			&postedMs, &deletedMs, &isHost, &isOwner,
		); err != nil {
			return nil, "", err
		}
		m.PostedAt = time.UnixMilli(postedMs).UTC()
		if deletedMs.Valid {
			t := time.UnixMilli(deletedMs.Int64).UTC()
			m.DeletedAt = &t
		}
		m.IsHost  = isHost  != 0
		m.IsOwner = isOwner != 0
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
