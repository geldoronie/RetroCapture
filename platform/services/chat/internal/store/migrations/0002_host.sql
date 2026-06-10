-- 0002_host.sql
-- #84 host-role tracking. Adds an is_host flag to chat_messages so
-- the host badge sticks across server restarts and history replays
-- (the live host_participant_id is session-scoped and would otherwise
-- be lost on every reconnect).

ALTER TABLE chat_messages ADD COLUMN is_host INTEGER NOT NULL DEFAULT 0;
