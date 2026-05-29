# Chat protocol (v0.5)

Wire format for the `chat.retrocapture.com` service. Tracking issue: [#84](../../../issues/84).

`v0.5` is the minimal protocol that exists to validate the design end-to-end. As of the late-#84 refactor it covers persistent identity (`rc_<id>`-bound profile), owner-secret room ownership, identity-bound stream chat rooms, password gating, the public-room browser, host badge, and server-side inactivity sweep. The full `v1` (directory-account identity, moderation REST, slow-mode UI, word filter) is a strict superset.

## Transport

- **WebSocket** for live message flow: `wss://chat.retrocapture.com/ws?room=<roomId>`.
  - `roomId` is required. Clients resolve it via `GET /rooms/by-slug/<slug>` first. The legacy `?stream=<streamId>` resolver was removed when stream chats moved to slug-keyed standalone rooms.
  - Origin check: any origin allowed for v0.5 (CORS / Origin enforcement comes with v1 directory-account auth).
- **HTTP** for state queries: `GET /rooms`, `POST /rooms`, `GET /rooms/:roomId`, `PATCH /rooms/:roomId`, `DELETE /rooms/:roomId`, `GET /rooms/:roomId/history`, `GET /rooms/by-slug/:slug`, `GET /health`.
- TLS terminated at the reverse proxy (Cloudflare / nginx / Caddy in front).
- One service-protocol version number per `Server` build; `GET /health` returns `{"protocol_version": 1, …}`. Bumped on every backwards-incompatible change; the service must keep accepting `previous_version` for at least one release.

## Identity (v0.5)

Anonymous only. When a client opens the WebSocket it sends a `hello` message with the nickname it wants to use; the server echoes back a `welcome` carrying an opaque `participant_id` (random, server-issued, scoped to this room and this connection — does NOT correlate across rooms or reconnects). The participant_id is what other messages reference for moderation actions.

There is no `/auth/verify` endpoint in v0.5. Directory-account-backed identity lands in v1 as a separate auth flow that produces a longer-lived token; the WebSocket protocol stays the same, just with a `participant_id` that's stable across sessions for the same account.

Rate limit (v0.5): 5 messages per 10 s per WebSocket connection, plus a global per-IP cap. No slow mode UI; the limits are hard-coded.

## WebSocket envelope

Every frame is a single JSON object with a `kind` discriminator and a payload keyed by the same name. New kinds are additive; clients MUST ignore unknown `kind` values.

```jsonc
{
  "kind": "hello",
  "hello": {
    "nickname": "geldo",
    "role":     "host"          // optional, default ""; see "Host role" below
  }
}
```

Server responses share the same shape:

```jsonc
{
  "kind": "welcome",
  "welcome": {
    "participant_id": "p_8af1c4",
    "room_id":        "r_d4c1f0",
    "room_kind":      "standalone",        // only kind in v0.5+
    "server_time_ms": 1716665490123,
    "protocol_version": 1,
    "is_host":            true,            // optional; this connection won the host claim
    "host_participant_id": "p_8af1c4"      // optional; participant_id of the current host
  }
}
```

### Host role

A stream-linked room MAY have exactly one **host** at a time — the RetroCapture instance that's publishing the stream. The host opts in by including `role: "host"` in its `hello`. First claimant wins; subsequent role-claims in the same room get `is_host: false` in their welcome.

When a participant is the host, the server tags their `message` frames with `is_host: true` and exposes the room's current host via `host_participant_id` on `welcome`, `room_state`, and `presence` (join events). The flag persists on stored messages so history replays render correctly even after the host's WebSocket session id changes on reconnect.

When the host disconnects, the slot is cleared; the next `role: "host"` connection can re-claim. Trust model in v0.5 is the same level as streamId secrecy (anyone with the streamId can claim host); v1 validates against the directory ownerToken.

### Client-to-server kinds

| Kind        | When                                                                 | Payload                                                              |
| ----------- | -------------------------------------------------------------------- | -------------------------------------------------------------------- |
| `hello`     | First frame after upgrade. Server expects this within 5 s or closes. | `{ "nickname": "<string>", "role"?: "host" }`                          |
| `post`      | New chat message from this participant.                              | `{ "body": "<string>" }`                                              |
| `delete`    | Moderation: drop a specific message.                                 | `{ "message_id": "m_…" }`                                              |
| `ping`      | Keepalive (also covered by WS Ping/Pong frames; this is optional).   | `{}`                                                                 |

A `post` frame MUST be sent only after the client has received `welcome`. Server rejects with `error` if posted before.

### Server-to-client kinds

| Kind        | When                                                                                       | Payload                                                                                                                 |
| ----------- | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| `welcome`   | Once, right after a successful `hello`.                                                    | see above                                                                                                               |
| `message`   | Broadcast of a new `post`, or replay of a historical message.                              | `{ "id": "m_…", "room_id": "r_…", "participant_id": "p_…", "nickname": "<string>", "body": "<string>", "posted_at_ms": 1716665490123, "is_host"?: true }` |
| `presence`  | A participant joined or left.                                                              | `{ "participant_id": "p_…", "nickname": "<string>", "event": "join" \| "leave", "is_host"?: true }`                     |
| `deleted`   | A message was deleted by moderation.                                                       | `{ "message_id": "m_…", "deleted_by": "p_…" }`                                                                          |
| `pong`      | Response to client `ping`.                                                                 | `{}`                                                                                                                    |
| `error`     | Server rejecting the previous frame.                                                       | `{ "code": "<symbolic>", "message": "<human-readable>" }`                                                               |
| `room_state` | Sent at welcome time to seed the client.                                                  | `{ "participants": [{ "id": "p_…", "nickname": "<string>", "is_host"?: true }], "settings": { "slow_mode_secs": 0, "word_filter": [] }, "host_participant_id"?: "p_…" }` |

Error `code`s in v0.5: `not_authenticated`, `rate_limited`, `room_not_found`, `room_closed`, `bad_request`, `internal_error`.

## HTTP endpoints (v0.5)

### `GET /health`

```jsonc
200 OK
{
  "data": { "status": "ok", "protocol_version": 1 },
  "error": null
}
```

### `GET /rooms/:roomId/history?before=<cursor>&limit=N`

Paginated chat history, newest-first.

- `cursor` = the `id` of the message in the previous page's last result, or omitted for the first page.
- `limit` ∈ `[1, 200]`, default `50`.

```jsonc
200 OK
{
  "data": {
    "messages": [
      { "id": "m_…", "participant_id": "p_…", "nickname": "geldo", "body": "hi", "posted_at_ms": 1716665490123 },
      …
    ],
    "next_cursor": "m_…"     // null when no older messages exist
  },
  "error": null
}
```

### `GET /rooms`

Public room listing for the in-app browser. Query: `?limit=N` (1..200, default 50). Returns every standalone row with `listed = 1`. The `is_stream_room` boolean discriminates between user-created chat rooms and streamer-provisioned ones so the UI can show different badges.

```jsonc
200 OK
{
  "data": {
    "rooms": [
      {
        "room_id":           "r_…",
        "kind":              "standalone",
        "slug":              "retro-arcade",
        "title":             "Stream of geldo",
        "has_password":      false,
        "is_stream_room":    true,
        "participant_count": 3,
        "created_at_ms":     1716665490123
      }
    ]
  },
  "error": null
}
```

### `POST /rooms`

Create a standalone room. The slug is auto-generated when the request omits it. Body:

```jsonc
{
  "title":            "Stream of geldo",
  "slug":             "retro-arcade",       // optional
  "password":         "hunter2",            // optional; server hashes
  "listed":           true,                 // default true
  "is_stream_room":   true,                 // streamer-provisioned vs user-created
  "owner_client_id":  "rc_3f1a2b…",         // creator's persistent identity
  "owner_secret":     "deadbeef…"           // client-minted plaintext; server hashes + stores
}
```

The `owner_secret` is the trust-bearing handle: subsequent joins that present the same plaintext are flagged `is_owner=true` regardless of `client_id`, and `DELETE`/`PATCH` require it.

### `GET /rooms/by-slug/:slug`

Resolve a public slug to its room id. `404 room_not_found` when the slug isn't taken — used by the client's *revive* probe to detect a room that was reaped by the inactivity sweep so it can be recreated locally with the saved secret.

```jsonc
200 OK
{ "data": { "room_id": "r_…", "slug": "retro-arcade" }, "error": null }
```

### `PATCH /rooms/:roomId`

Owner-initiated room update. Same secret-only auth as `DELETE`. Today only the `listed` flag is editable — title/slug stay frozen because the wire history carries denormalised copies. Used by the host's client to promote a previously-hidden room when the streamer-public default flipped.

```jsonc
PATCH /rooms/r_abc123
Content-Type: application/json
{
  "owner_secret": "deadbeefcafef00ddeadbeefcafef00d",
  "listed":       true
}

200 OK
{ "data": { "updated": true }, "error": null }
```

### `DELETE /rooms/:roomId`

Owner-initiated permanent delete (#84). The server cascades through
`chat_messages` so the room and its history are gone for good — no
soft-delete tombstone, no archived state. Active participants are
evicted from the in-memory registry the moment the row is dropped.

Authorisation: caller must prove ownership via the plaintext
`owner_secret` matching the stored hash. There is intentionally
**no** fallback to `owner_client_id`: a client_id is sender-claimed
and is visible to every participant in `welcome`/`room_state`
frames, so honouring it for an authoritative op would let anyone
who has seen the creator's id wipe their rooms. Rooms with no
`owner_secret_hash` on file (legacy / stream-linked) cannot be
deleted through this endpoint.

```jsonc
DELETE /rooms/r_abc123
Content-Type: application/json
{
  "owner_secret": "deadbeefcafef00ddeadbeefcafef00d"
}

200 OK
{ "data": { "deleted": true }, "error": null }
```

Error envelopes: `403 not_owner` (missing or wrong secret, or
room has no secret on file), `404 room_not_found` (already gone).

### `GET /rooms/:roomId`

Room metadata (visible to anyone — not moderation-gated).

```jsonc
200 OK
{
  "data": {
    "room_id":          "r_…",
    "kind":             "standalone",
    "owner_account_id": "rc_3f1a2b…",
    "title":            "Stream of geldo",
    "created_at_ms":    1716665490123,
    "archived_at_ms":   null,
    "participant_count": 12
  },
  "error": null
}
```

### Identity-bound stream chat (host integration)

When a RetroCapture host starts streaming with chat enabled, the desktop client provisions a single persistent room per identity (slug derived from the user-chosen "Room name" + 4-hex disambiguator) and publishes the slug via `/meta.chat.roomSlug` on the host's own metadata endpoint. Viewers' clients poll `/meta`, pick up `roomSlug`, resolve via `GET /rooms/by-slug/:slug`, and `ConnectBySlug` to it. There is intentionally no per-session room — restarting the stream binds to the same chat across reboots.

The host's local `owned_rooms.json` (sibling to `identity.json` under `$XDG_DATA_HOME/retrocapture/`) caches each room's `owner_secret`. If the inactivity sweep wipes the room while the host is away, the next stream-start probes `GET /rooms/by-slug/<slug>` and re-`POST /rooms`s with the saved secret — ownership survives the reaping.

### Inactivity sweep

The chat service runs a background goroutine that periodically deletes rooms with no participant joins and no message posts inside a configurable window. Two env vars control it:

  - `CHAT_ROOM_INACTIVITY_DAYS` (default `3`, `0` disables the sweep entirely).
  - `CHAT_ROOM_SWEEP_INTERVAL_MINUTES` (default `60`).

Rooms with at least one currently-connected participant are skipped even if their stored `last_activity_ms` is past the cutoff. Sweep deletions are full cascades (`chat_messages` + `chat_rooms` + `Registry.Forget`) — clients see their WebSocket close.

## Data flow

```
1. Host's directory client publishes a stream    ──▶ directory service stores stream id
                                                                    │
2. Viewer opens chat panel                                         │
   GET wss://chat.../ws?stream=<streamId>                          │
                          │                                        │
                          ▼                                        │
3. Chat gateway looks up linked room              ──▶ if none, creates one
                                                       owner_account_id = NULL until v1 directory auth
                          │
                          ▼
4. Client sends `hello`                            ──▶ server creates participant, replies `welcome` + `room_state`
                          │
                          ▼
5. Client sends `post`                             ──▶ rate-limit check
                                                       persist to chat_messages
                                                       fanout `message` to all WS subscribers of this room
```

## Storage schema (v0.5)

```sql
CREATE TABLE chat_rooms (
    id                TEXT PRIMARY KEY,         -- "r_<random>"
    kind              TEXT NOT NULL,            -- 'stream_linked' | 'standalone'
    linked_stream_id  TEXT NULL,                -- nullable, indexed
    owner_account_id  TEXT NULL,                -- nullable in v0.5 (no accounts yet)
    slug              TEXT UNIQUE NULL,         -- for standalone rooms, e.g. "crt-talk"
    title             TEXT NOT NULL DEFAULT '',
    description       TEXT NOT NULL DEFAULT '',
    settings_json     TEXT NOT NULL DEFAULT '{}', -- slow_mode_secs, word_filter[], persist_after_stream
    created_at_ms     INTEGER NOT NULL,
    archived_at_ms    INTEGER NULL
);
CREATE INDEX idx_chat_rooms_stream ON chat_rooms(linked_stream_id) WHERE linked_stream_id IS NOT NULL;

CREATE TABLE chat_messages (
    id                TEXT PRIMARY KEY,         -- "m_<random>"
    room_id           TEXT NOT NULL REFERENCES chat_rooms(id),
    participant_id    TEXT NOT NULL,            -- "p_<random>"
    nickname          TEXT NOT NULL,
    body              TEXT NOT NULL,
    posted_at_ms      INTEGER NOT NULL,
    deleted_at_ms     INTEGER NULL              -- soft-delete; UI shows "[message removed]"
);
CREATE INDEX idx_chat_messages_room_posted ON chat_messages(room_id, posted_at_ms);

-- v0.5 omits chat_bans entirely. Comes in v1.
```

## Acceptance for v0.5

A v0.5 implementation is "done" when:

- The chat service builds with `docker compose up chat` and listens on `:8082`.
- `curl http://localhost:8082/health` returns `{"data":{"status":"ok","protocol_version":1},"error":null}`.
- A WebSocket client can open `ws://localhost:8082/ws?stream=<arbitrary streamId>`, send a `hello`, post messages, and see them broadcast back to a second connected WebSocket on the same room.
- The same flow works against `ws://localhost:8082/ws?room=<existing roomId>`.
- `GET /rooms/:roomId/history` returns messages newest-first with cursor pagination.
- Restarting the service preserves both the room and its messages (SQLite-backed).

UI integration (web portal + native client) is NOT part of v0.5 — those PRs come once this protocol is validated.

## Open questions for v1+

- **Directory account auth.** v1 will introduce `POST /auth/verify` that accepts a directory-issued JWT and returns a chat session token; the WebSocket then accepts a `Bearer <token>` query param or `Sec-WebSocket-Protocol` subprotocol. Token claims include `account_id` (stable across rooms/reconnects) + `display_name`.
- **Moderation REST.** `POST /rooms/:roomId/moderate` with `ban` / `unban` / `set_slow_mode` / `set_word_filter` / `clear_all` actions. Authed by the room owner's JWT.
- **Standalone-room creation.** Schema is already there in v0.5; UI / discovery is v1+. CRUD endpoints will look like `POST /rooms`, `GET /rooms?owner=<id>`, `PATCH /rooms/:id`.
- **`persist_after_stream`.** Boolean in `chat_rooms.settings_json`. When true, stream-linked rooms keep accepting messages after the stream ends (becomes effectively a standalone room). v0.5 defaults to false (read-only after stream ends).
- **Room slugs.** Standalone rooms get human-readable URLs (`chat.retrocapture.com/r/crt-talk`); the slug field exists in the v0.5 schema for forwards compatibility but is unused.
