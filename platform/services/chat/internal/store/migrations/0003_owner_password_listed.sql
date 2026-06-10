-- 0003_owner_password_listed.sql
-- #84 standalone-room expansion:
--   1) password protection — optional sha256-hex password stored on
--      the room; the WS hello must include a matching plaintext
--      password (we hash on the server side and compare).
--   2) public listing — the new GET /rooms endpoint returns rooms
--      whose `listed` flag is on. Defaults true so the create form's
--      "List publicly" toggle is opt-out, matching how the directory
--      service treats stream publication.
--   3) owner identity — we already had owner_account_id sitting NULL.
--      Now used to store the rc_<...> client_id the creator sent at
--      POST /rooms time. When a participant connects with the same
--      client_id, they get the is_owner flag (standalone analog of
--      stream-linked is_host).

ALTER TABLE chat_rooms ADD COLUMN password_hash TEXT NULL;
ALTER TABLE chat_rooms ADD COLUMN listed        INTEGER NOT NULL DEFAULT 1;

-- Index for the listing endpoint — most queries filter by listed +
-- kind. Sort by created desc handled by query ORDER BY.
CREATE INDEX idx_chat_rooms_listed_kind ON chat_rooms(listed, kind);

-- is_owner is the standalone-room analog of chat_messages.is_host;
-- sticky on the row so backlog replays correctly.
ALTER TABLE chat_messages ADD COLUMN is_owner INTEGER NOT NULL DEFAULT 0;
