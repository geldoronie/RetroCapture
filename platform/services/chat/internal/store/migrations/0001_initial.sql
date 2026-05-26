-- 0001_initial.sql
-- First chat schema (v0.5). Two tables: chat_rooms (the conversation
-- contexts) and chat_messages (the actual lines). Bans / mutes /
-- settings come in v1.

CREATE TABLE chat_rooms (
    id                TEXT PRIMARY KEY,           -- "r_<uuid-short>"
    kind              TEXT NOT NULL,              -- 'stream_linked' | 'standalone'
    linked_stream_id  TEXT NULL,                  -- stream id from the directory; nullable
    owner_account_id  TEXT NULL,                  -- nullable in v0.5 (no accounts yet)
    slug              TEXT UNIQUE NULL,           -- standalone-room human URL; unused in v0.5
    title             TEXT NOT NULL DEFAULT '',
    description       TEXT NOT NULL DEFAULT '',
    settings_json     TEXT NOT NULL DEFAULT '{}', -- {slow_mode_secs, word_filter[], persist_after_stream}
    created_at_ms     INTEGER NOT NULL,
    archived_at_ms    INTEGER NULL
);

-- One linked_stream_id per stream — when the gateway resolves a
-- /ws?stream=<id> request it looks up the row via this index.
CREATE UNIQUE INDEX idx_chat_rooms_stream
    ON chat_rooms(linked_stream_id)
    WHERE linked_stream_id IS NOT NULL;

CREATE TABLE chat_messages (
    id                TEXT PRIMARY KEY,           -- "m_<uuid-short>"
    room_id           TEXT NOT NULL REFERENCES chat_rooms(id),
    participant_id    TEXT NOT NULL,              -- "p_<random>" (per-connection, see hub)
    nickname          TEXT NOT NULL,
    body              TEXT NOT NULL,
    posted_at_ms      INTEGER NOT NULL,
    deleted_at_ms     INTEGER NULL                -- soft-delete; UI shows "[message removed]"
);

-- History queries page by (room_id, posted_at_ms DESC, id DESC) so
-- this composite index covers the hot path.
CREATE INDEX idx_chat_messages_room_posted
    ON chat_messages(room_id, posted_at_ms);
