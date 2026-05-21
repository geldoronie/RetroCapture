// Package store wraps the SQLite database used by the directory
// service. All schema changes happen via embedded migrations applied
// at Open() time; the rest of the package exposes typed CRUD methods
// that the API handlers and the reaper goroutine call.
//
// We use modernc.org/sqlite (a pure-Go SQLite, no CGO) so the build
// stays static and the scratch container needs no shared libraries.
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
	ErrNotFound  = errors.New("store: stream not found")
	ErrForbidden = errors.New("store: owner token mismatch")
)

// Stream is the typed row. Fields use the wire-format names (camelCase)
// so JSON marshalling is direct; the DB column mapping is hand-coded
// in scan/insert helpers below.
type Stream struct {
	StreamID         string
	OwnerToken       string // never include in client-facing responses
	Name             string
	HostNickname     string
	Shader           string
	ResolutionW      int
	ResolutionH      int
	FPS              int
	Codec            string
	PasswordRequired bool
	Endpoint         string
	EndpointMode     string
	ClientCount      int
	PublicIP         string
	Version          string
	RegisteredAt     time.Time
	LastHeartbeatAt  time.Time
	ExpiresAt        time.Time
}

// Patch holds the optional mutable fields for PATCH /streams/<id>.
// Any pointer field that is non-nil is applied; nils are skipped so
// the caller can update one field without re-stating the rest.
type Patch struct {
	Name             *string
	HostNickname     *string
	Shader           *string
	ResolutionW      *int
	ResolutionH      *int
	FPS              *int
	Codec            *string
	PasswordRequired *bool
	Endpoint         *string
	EndpointMode     *string
}

// ListOptions controls GET /streams behaviour. Zero values are safe:
// Limit=0 falls back to 100, Sort=="" falls back to "clients".
type ListOptions struct {
	Sort  string // "clients" | "recent" | "name"
	Query string // case-insensitive substring on name / host_nickname
	Limit int
}

// Store is the public handle. Pass it around by pointer; it is safe
// for concurrent use (SQLite handles serialisation internally and our
// statements don't share state).
type Store struct {
	db *sql.DB
}

// Open opens (or creates) a SQLite database at the given path,
// applies all migrations, and returns the Store. `:memory:` is
// accepted for tests.
func Open(path string) (*Store, error) {
	// WAL gives concurrent reads alongside one writer, which is
	// exactly the heartbeat-heavy pattern we expect. busy_timeout
	// makes contended writes wait instead of erroring.
	dsn := path + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(on)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}
	if err := db.Ping(); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("ping: %w", err)
	}

	if err := applyMigrations(db); err != nil {
		_ = db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

// Close releases the underlying database handle.
func (s *Store) Close() error { return s.db.Close() }

func applyMigrations(db *sql.DB) error {
	// Track which migrations have run in a tiny meta table so a
	// re-open is a no-op. Created on demand.
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS _migrations (
        name TEXT PRIMARY KEY,
        applied_at INTEGER NOT NULL
    )`); err != nil {
		return fmt.Errorf("create _migrations: %w", err)
	}

	entries, err := migrationsFS.ReadDir("migrations")
	if err != nil {
		return fmt.Errorf("read migrations: %w", err)
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".sql") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names) // lexicographic == numeric for NNN_ prefix

	for _, name := range names {
		var applied int
		if err := db.QueryRow(`SELECT COUNT(*) FROM _migrations WHERE name = ?`, name).Scan(&applied); err != nil {
			return fmt.Errorf("check migration %s: %w", name, err)
		}
		if applied > 0 {
			continue
		}
		body, err := migrationsFS.ReadFile("migrations/" + name)
		if err != nil {
			return fmt.Errorf("read %s: %w", name, err)
		}
		if _, err := db.Exec(string(body)); err != nil {
			return fmt.Errorf("apply %s: %w", name, err)
		}
		if _, err := db.Exec(`INSERT INTO _migrations(name, applied_at) VALUES(?, ?)`,
			name, time.Now().Unix()); err != nil {
			return fmt.Errorf("record migration %s: %w", name, err)
		}
	}
	return nil
}

// Insert creates a new stream row. The caller is responsible for
// generating StreamID, OwnerToken, PublicIP, and the *At timestamps;
// the store performs the write atomically with no further
// modification.
func (s *Store) Insert(ctx context.Context, st Stream) error {
	_, err := s.db.ExecContext(ctx, `
        INSERT INTO streams(
            stream_id, owner_token, name, host_nickname, shader,
            resolution_w, resolution_h, fps, codec, password_required,
            endpoint, endpoint_mode, client_count, public_ip, version,
            registered_at, last_heartbeat_at, expires_at
        ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    `,
		st.StreamID, st.OwnerToken, st.Name, st.HostNickname, st.Shader,
		st.ResolutionW, st.ResolutionH, st.FPS, st.Codec, boolToInt(st.PasswordRequired),
		st.Endpoint, st.EndpointMode, st.ClientCount, st.PublicIP, st.Version,
		st.RegisteredAt.Unix(), st.LastHeartbeatAt.Unix(), st.ExpiresAt.Unix(),
	)
	return err
}

// Heartbeat refreshes the entry's last_heartbeat_at + expires_at.
// If clientCount is non-nil, it's also updated. The ownerToken must
// match the stored token; ErrForbidden is returned otherwise.
// ErrNotFound when the row is gone.
func (s *Store) Heartbeat(ctx context.Context, streamID, ownerToken string, clientCount *int, newExpiresAt time.Time) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()

	var storedToken string
	err = tx.QueryRowContext(ctx, `SELECT owner_token FROM streams WHERE stream_id = ?`, streamID).Scan(&storedToken)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	if storedToken != ownerToken {
		return ErrForbidden
	}

	now := time.Now().Unix()
	if clientCount != nil {
		_, err = tx.ExecContext(ctx, `
            UPDATE streams SET last_heartbeat_at = ?, expires_at = ?, client_count = ?
            WHERE stream_id = ?
        `, now, newExpiresAt.Unix(), *clientCount, streamID)
	} else {
		_, err = tx.ExecContext(ctx, `
            UPDATE streams SET last_heartbeat_at = ?, expires_at = ?
            WHERE stream_id = ?
        `, now, newExpiresAt.Unix(), streamID)
	}
	if err != nil {
		return err
	}
	return tx.Commit()
}

// ApplyPatch updates the mutable fields. Token-authenticated. Only
// non-nil fields are applied so the caller can update one column at a
// time without re-stating the rest.
func (s *Store) ApplyPatch(ctx context.Context, streamID, ownerToken string, p Patch) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { _ = tx.Rollback() }()

	var storedToken string
	err = tx.QueryRowContext(ctx, `SELECT owner_token FROM streams WHERE stream_id = ?`, streamID).Scan(&storedToken)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	if storedToken != ownerToken {
		return ErrForbidden
	}

	// Build the SET clause dynamically. SQLite supports parameterised
	// SQL but not parameterised column names, so the column list is
	// hand-built from constant strings (no injection risk).
	var sets []string
	var args []any
	if p.Name != nil {
		sets, args = append(sets, "name = ?"), append(args, *p.Name)
	}
	if p.HostNickname != nil {
		sets, args = append(sets, "host_nickname = ?"), append(args, *p.HostNickname)
	}
	if p.Shader != nil {
		sets, args = append(sets, "shader = ?"), append(args, *p.Shader)
	}
	if p.ResolutionW != nil {
		sets, args = append(sets, "resolution_w = ?"), append(args, *p.ResolutionW)
	}
	if p.ResolutionH != nil {
		sets, args = append(sets, "resolution_h = ?"), append(args, *p.ResolutionH)
	}
	if p.FPS != nil {
		sets, args = append(sets, "fps = ?"), append(args, *p.FPS)
	}
	if p.Codec != nil {
		sets, args = append(sets, "codec = ?"), append(args, *p.Codec)
	}
	if p.PasswordRequired != nil {
		sets, args = append(sets, "password_required = ?"), append(args, boolToInt(*p.PasswordRequired))
	}
	if p.Endpoint != nil {
		sets, args = append(sets, "endpoint = ?"), append(args, *p.Endpoint)
	}
	if p.EndpointMode != nil {
		sets, args = append(sets, "endpoint_mode = ?"), append(args, *p.EndpointMode)
	}
	if len(sets) == 0 {
		// No-op patch — still return success rather than an error,
		// matches HTTP PATCH semantics where the empty body is valid.
		return tx.Commit()
	}
	args = append(args, streamID)
	q := "UPDATE streams SET " + strings.Join(sets, ", ") + " WHERE stream_id = ?"
	if _, err := tx.ExecContext(ctx, q, args...); err != nil {
		return err
	}
	return tx.Commit()
}

// Delete removes an entry. Token-authenticated. Idempotent: deleting a
// non-existent stream returns nil so /DELETE remains safe to retry.
func (s *Store) Delete(ctx context.Context, streamID, ownerToken string) error {
	var storedToken string
	err := s.db.QueryRowContext(ctx, `SELECT owner_token FROM streams WHERE stream_id = ?`, streamID).Scan(&storedToken)
	if errors.Is(err, sql.ErrNoRows) {
		return nil // idempotent
	}
	if err != nil {
		return err
	}
	if storedToken != ownerToken {
		return ErrForbidden
	}
	_, err = s.db.ExecContext(ctx, `DELETE FROM streams WHERE stream_id = ?`, streamID)
	return err
}

// Get returns a single entry without the owner token (callers that
// need the token use the internal Heartbeat / ApplyPatch / Delete
// paths above). ErrNotFound if the row is gone.
func (s *Store) Get(ctx context.Context, streamID string) (Stream, error) {
	row := s.db.QueryRowContext(ctx, `
        SELECT stream_id, owner_token, name, host_nickname, shader,
               resolution_w, resolution_h, fps, codec, password_required,
               endpoint, endpoint_mode, client_count, public_ip, version,
               registered_at, last_heartbeat_at, expires_at
        FROM streams WHERE stream_id = ?
    `, streamID)
	st, err := scanStream(row.Scan)
	if errors.Is(err, sql.ErrNoRows) {
		return Stream{}, ErrNotFound
	}
	return st, err
}

// List returns up to opts.Limit entries matching the filter. Order
// depends on opts.Sort. Total is the count of matching entries BEFORE
// the limit is applied.
func (s *Store) List(ctx context.Context, opts ListOptions) (streams []Stream, total int, err error) {
	if opts.Limit <= 0 || opts.Limit > 500 {
		opts.Limit = 100
	}
	order := "client_count DESC, last_heartbeat_at DESC"
	switch opts.Sort {
	case "", "clients":
		// default
	case "recent":
		order = "last_heartbeat_at DESC"
	case "name":
		order = "name COLLATE NOCASE ASC"
	default:
		return nil, 0, fmt.Errorf("invalid sort: %q", opts.Sort)
	}

	var where string
	var args []any
	if q := strings.TrimSpace(opts.Query); q != "" {
		where = " WHERE name LIKE ? OR host_nickname LIKE ?"
		pat := "%" + strings.ToLower(q) + "%"
		args = append(args, pat, pat)
	}

	if err := s.db.QueryRowContext(ctx, "SELECT COUNT(*) FROM streams"+where, args...).Scan(&total); err != nil {
		return nil, 0, err
	}

	args = append(args, opts.Limit)
	rows, err := s.db.QueryContext(ctx, `
        SELECT stream_id, owner_token, name, host_nickname, shader,
               resolution_w, resolution_h, fps, codec, password_required,
               endpoint, endpoint_mode, client_count, public_ip, version,
               registered_at, last_heartbeat_at, expires_at
        FROM streams`+where+`
        ORDER BY `+order+`
        LIMIT ?
    `, args...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()

	for rows.Next() {
		st, err := scanStream(rows.Scan)
		if err != nil {
			return nil, 0, err
		}
		streams = append(streams, st)
	}
	return streams, total, rows.Err()
}

// DeleteExpired removes every entry with expires_at < cutoff. Used by
// the reaper goroutine. Returns the count deleted for logging.
func (s *Store) DeleteExpired(ctx context.Context, cutoff time.Time) (int64, error) {
	res, err := s.db.ExecContext(ctx, `DELETE FROM streams WHERE expires_at < ?`, cutoff.Unix())
	if err != nil {
		return 0, err
	}
	n, _ := res.RowsAffected()
	return n, nil
}

// InsertReport appends a moderation report row. Append-only — the
// directory itself never reads these back; the operator pulls them
// out manually for triage. The reportID is the same R-XXXXXXXX
// receipt handed back to the user, indexed so the maintainer can
// look up a report by the number the reporter quoted.
func (s *Store) InsertReport(ctx context.Context, streamID, reportID, reporterIP, reason, contact string) error {
	_, err := s.db.ExecContext(ctx, `
        INSERT INTO stream_reports(stream_id, report_id, reporter_ip, reason, contact, reported_at)
        VALUES(?,?,?,?,?,?)
    `, streamID, reportID, reporterIP, reason, contact, time.Now().Unix())
	return err
}

// --- helpers ---

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func scanStream(scan func(...any) error) (Stream, error) {
	var (
		st           Stream
		pwReq        int
		regAt, lhbAt, expAt int64
	)
	err := scan(
		&st.StreamID, &st.OwnerToken, &st.Name, &st.HostNickname, &st.Shader,
		&st.ResolutionW, &st.ResolutionH, &st.FPS, &st.Codec, &pwReq,
		&st.Endpoint, &st.EndpointMode, &st.ClientCount, &st.PublicIP, &st.Version,
		&regAt, &lhbAt, &expAt,
	)
	if err != nil {
		return Stream{}, err
	}
	st.PasswordRequired = pwReq != 0
	st.RegisteredAt = time.Unix(regAt, 0).UTC()
	st.LastHeartbeatAt = time.Unix(lhbAt, 0).UTC()
	st.ExpiresAt = time.Unix(expAt, 0).UTC()
	return st, nil
}
