-- 0006_last_activity.sql
-- #84 — Per-room "last activity" timestamp for the inactivity-sweep
-- worker. The sweep deletes rooms that have had no joins and no
-- posts within a configurable window (CHAT_ROOM_INACTIVITY_DAYS,
-- default 3 days) and are currently empty.
--
-- We backfill last_activity_ms = MAX(latest message posted_at_ms,
-- room created_at_ms) so the sweep doesn't immediately wipe every
-- pre-existing room on first boot after this migration lands.
--
-- The gateway updates the column on (a) WS hello (a participant
-- joined), (b) message persist (a post landed). That covers both
-- "connection" and "post" activity as defined in the feature spec.

ALTER TABLE chat_rooms ADD COLUMN last_activity_ms INTEGER NULL;

UPDATE chat_rooms
   SET last_activity_ms = COALESCE(
       (SELECT MAX(posted_at_ms) FROM chat_messages
         WHERE room_id = chat_rooms.id),
       created_at_ms);

-- Index the sweep query (`WHERE last_activity_ms < ?`).
CREATE INDEX IF NOT EXISTS idx_chat_rooms_last_activity
    ON chat_rooms (last_activity_ms);
