-- 0005_drop_stream_linked.sql
-- #84 — identity-bound stream chat rooms. The pre-rework model
-- created a new chat_rooms row every time a streamer started a
-- session (keyed by per-session directory streamId), so the DB
-- ended up with one orphan room per restart, polluting the public
-- listing. The rework moves the streamer's chat to a user-chosen,
-- persistent standalone room (linked via /meta.chat.roomSlug
-- instead of /meta.chat.streamId), so the stream_linked legacy
-- rows have no path back into use and should go.
--
-- This wipes every stream_linked room AND its messages. Standalone
-- rooms (the user's owned rooms) are untouched. Tables aren't
-- structurally changed — kind = 'stream_linked' just stops getting
-- written by the gateway after this rollout.

DELETE FROM chat_messages
  WHERE room_id IN (SELECT id FROM chat_rooms WHERE kind = 'stream_linked');

DELETE FROM chat_rooms
  WHERE kind = 'stream_linked';
