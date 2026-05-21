-- 001_initial.sql
-- First schema. Two tables: streams (the live directory) and
-- stream_reports (moderation queue, append-only).

CREATE TABLE streams (
    stream_id          TEXT PRIMARY KEY,
    owner_token        TEXT NOT NULL,
    name               TEXT NOT NULL,
    host_nickname      TEXT NOT NULL DEFAULT '',
    shader             TEXT NOT NULL DEFAULT '',
    resolution_w       INTEGER NOT NULL,
    resolution_h       INTEGER NOT NULL,
    fps                INTEGER NOT NULL,
    codec              TEXT NOT NULL,
    password_required  INTEGER NOT NULL DEFAULT 0,
    endpoint           TEXT NOT NULL,
    endpoint_mode      TEXT NOT NULL,
    client_count       INTEGER NOT NULL DEFAULT 0,
    public_ip          TEXT NOT NULL DEFAULT '',
    version            TEXT NOT NULL DEFAULT '',
    registered_at      INTEGER NOT NULL,    -- unix seconds (UTC)
    last_heartbeat_at  INTEGER NOT NULL,
    expires_at         INTEGER NOT NULL
);

-- Reaper scans this index every minute looking for expires_at < now.
CREATE INDEX idx_streams_expires_at ON streams(expires_at);

-- Default browse sort key. SQLite reads indexes forward; we want
-- descending so the reverse-direction is built-in.
CREATE INDEX idx_streams_client_count ON streams(client_count DESC);

-- 'recent' sort.
CREATE INDEX idx_streams_last_heartbeat ON streams(last_heartbeat_at DESC);

CREATE TABLE stream_reports (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    stream_id    TEXT NOT NULL,
    reporter_ip  TEXT NOT NULL DEFAULT '',
    reason       TEXT NOT NULL DEFAULT '',
    contact      TEXT NOT NULL DEFAULT '',
    reported_at  INTEGER NOT NULL
);

CREATE INDEX idx_reports_stream ON stream_reports(stream_id);
