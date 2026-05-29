-- 0008_drop_legacy_stream_linked.sql
-- #84 final cleanup — after the identity-bound rework (migrations
-- 0005 wiped legacy rows, 0007 added is_stream_room), neither the
-- 'stream_linked' kind nor the linked_stream_id column are written
-- by anything in the codebase anymore. Drop both so the schema
-- matches the live data model and new contributors don't see
-- vestigial fields they have to guess about.
--
-- SQLite needs a table-rewrite for column drops, so we do the
-- standard rename + recreate dance. Same column set as the post-
-- 0007 schema minus linked_stream_id. CHECK constraint on kind
-- is removed entirely — every row is 'standalone' now and
-- whether a row is a stream's chat room is the is_stream_room
-- boolean's job.

PRAGMA foreign_keys = OFF;

CREATE TABLE chat_rooms_new (
    id                TEXT PRIMARY KEY,
    kind              TEXT NOT NULL DEFAULT 'standalone',
    owner_account_id  TEXT,
    slug              TEXT UNIQUE,
    title             TEXT NOT NULL DEFAULT '',
    description       TEXT NOT NULL DEFAULT '',
    settings_json     TEXT NOT NULL DEFAULT '{}',
    password_hash     TEXT,
    owner_secret_hash TEXT,
    listed            INTEGER NOT NULL DEFAULT 1,
    is_stream_room    INTEGER NOT NULL DEFAULT 0,
    created_at_ms     INTEGER NOT NULL,
    archived_at_ms    INTEGER,
    last_activity_ms  INTEGER
);

INSERT INTO chat_rooms_new (
    id, kind, owner_account_id, slug, title, description,
    settings_json, password_hash, owner_secret_hash, listed,
    is_stream_room, created_at_ms, archived_at_ms, last_activity_ms
)
SELECT
    id, kind, owner_account_id, slug, title, description,
    settings_json, password_hash, owner_secret_hash, listed,
    is_stream_room, created_at_ms, archived_at_ms, last_activity_ms
  FROM chat_rooms;

DROP TABLE chat_rooms;
ALTER TABLE chat_rooms_new RENAME TO chat_rooms;

-- Re-create indexes that lived on the old table. (idx_chat_rooms_
-- last_activity from migration 0006.)
CREATE INDEX IF NOT EXISTS idx_chat_rooms_last_activity
    ON chat_rooms (last_activity_ms);

PRAGMA foreign_keys = ON;
