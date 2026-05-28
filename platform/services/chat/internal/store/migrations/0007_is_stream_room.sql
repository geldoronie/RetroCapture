-- 0007_is_stream_room.sql
-- #84 — After the identity-bound rework (migration 0005) every
-- room kind became 'standalone', which left the DB unable to
-- distinguish a user's manually-created chat room from the
-- automatically-provisioned room a host uses for their stream's
-- chat. Re-introduce the distinction as a boolean flag on the
-- existing standalone-room shape (slug + owner_secret + listed),
-- instead of resurrecting the `stream_linked` kind that carried
-- per-session-streamId baggage.
--
-- Clients set it on POST /rooms via the host's auto-provisioning
-- path. The flag is non-secret and exposed in GET /rooms so the
-- in-app browser can render the right badge ([STREAM] vs [ROOM]).
--
-- Default false on backfill: existing standalone rooms that pre-
-- date this column stay tagged as plain rooms, which is correct
-- — they were created from the OSD "Create new..." dialog.

ALTER TABLE chat_rooms ADD COLUMN is_stream_room INTEGER NOT NULL DEFAULT 0;
