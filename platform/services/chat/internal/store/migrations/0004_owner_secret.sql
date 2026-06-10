-- 0004_owner_secret.sql
-- #84 per-room owner secret. Augments the existing owner_account_id
-- (which carries the creator's rc_<...> client_id) with a separate
-- secret hash the client persists locally on the machine that
-- created the room. Joins carrying the matching secret in hello are
-- flagged is_owner regardless of which client_id they present —
-- mirrors the directory service's per-stream ownerToken model and
-- gives the room creator a recoverable, copy-with-the-file form of
-- ownership independent of their identity.json.

ALTER TABLE chat_rooms ADD COLUMN owner_secret_hash TEXT NULL;
