# Directory protocol

Wire format for the public RetroCapture stream directory (issue #49).

The directory is a small HTTP/JSON service. A host that wants its stream
to appear in the in-app browse list registers an entry, sends a
heartbeat every 30 seconds to keep it alive, and either lets the TTL
expire it or explicitly deletes it. A client fetches the list, picks a
stream, and connects to that stream's `endpoint` directly — the
directory **never proxies stream bytes**.

Service implementation: `platform/services/directory/`.

## Versioning

- Schema version is communicated via the `version` field on each entry
  (mirrors the RetroCapture build's `protocolVersion` from #47). Clients
  ignore entries whose `version` they don't recognise rather than
  failing.
- The service itself advertises the protocol version it speaks at
  `GET /health` (response includes `"protocol_version"`).
- Backwards-incompatible changes increment the protocol version and the
  service must continue accepting the previous one for at least one
  release cycle.

Current protocol version: **1**.

## Response envelope

Every JSON response has the same top-level shape:

```jsonc
{ "data": <payload> | null, "error": <error-object> | null }
```

Exactly one of `data` / `error` is non-null.

`error-object`:

```jsonc
{ "code": "invalid_request" | "not_found" | "forbidden" | "rate_limited" | "internal",
  "message": "human-readable explanation" }
```

`200`–`299`: `error` is `null`.
`400`–`499`: `data` is `null`, `error` is populated.
`500`–`599`: `data` is `null`, `error` is populated.

A `204 No Content` response has no body. Used for successful mutations
that have nothing useful to return.

## Entry schema

```jsonc
{
  "streamId":         "uuid-v4",                     // assigned by the directory at register
  "name":             "My CRT setup",                // user-supplied, 1..120 chars
  "hostNickname":     "alice",                       // optional, 0..40 chars
  "shader":           "crt/crt-mattias.glslp",       // mirrors host's active preset name
  "resolution":       { "w": 1920, "h": 1080 },      // host's source dims
  "fps":              60,                            // host's source fps
  "codec":            "h264",                        // "h264" | "h265"
  "passwordRequired": false,                         // bool; password itself never crosses this wire
  "endpoint":         "https://abc-123.trycloudflare.com",
  "endpointMode":     "direct" | "tunnel-cloudflare" | "custom",
  "clientCount":      3,                             // last value the host reported via heartbeat
  "publicIp":         "1.2.3.4",                     // inferred from request origin, for moderation
  "version":          "0.7.0-alpha",                 // RetroCapture build's protocolVersion (#47)
  "registeredAt":     "2026-05-15T12:00:00Z",        // RFC 3339 UTC
  "lastHeartbeatAt":  "2026-05-15T12:01:30Z",
  "expiresAt":        "2026-05-15T12:03:30Z"
}
```

`ownerToken` is intentionally absent from any GET response. The token
is returned **once** at `POST /register` and never echoed back, so a
malicious browser or man-in-the-middle on the read path can't hijack an
existing entry.

## Endpoints

### `GET /health`

Liveness / readiness probe.

```http
GET /health HTTP/1.1
```

Response `200`:

```json
{ "data": { "status": "ok", "protocol_version": 1 }, "error": null }
```

### `POST /register`

Create a new directory entry. Returns the `streamId` (a uuid-v4 the
client must keep) and an `ownerToken` (a random 32-byte hex string the
client must keep and present on every subsequent mutation).

Request body:

```jsonc
{
  "name":             "My CRT setup",
  "hostNickname":     "alice",                  // optional
  "shader":           "crt/crt-mattias.glslp",
  "resolution":       { "w": 1920, "h": 1080 },
  "fps":              60,
  "codec":            "h264",
  "passwordRequired": false,
  "endpoint":         "https://example.com:8080",
  "endpointMode":     "direct" | "tunnel-cloudflare" | "custom",
  "version":          "0.7.0-alpha"
}
```

Server-derived fields (host cannot override them):
- `streamId` — uuid-v4
- `ownerToken` — 32 random bytes, hex-encoded (returned exactly once)
- `clientCount` — starts at 0
- `publicIp` — taken from the HTTP request source IP
- `registeredAt`, `lastHeartbeatAt`, `expiresAt` — set to now / now / now+TTL

Response `201`:

```json
{
  "data": {
    "streamId":   "550e8400-e29b-41d4-a716-446655440000",
    "ownerToken": "9f0d1e22…(64 hex chars)…3c4a",
    "expiresAt":  "2026-05-15T12:03:30Z"
  },
  "error": null
}
```

Validation errors → `400` with `error.code = "invalid_request"`.

### `POST /heartbeat`

Keeps an existing entry alive. Authenticated by `ownerToken`.

Request body:

```jsonc
{
  "streamId":    "550e8400-…",
  "ownerToken":  "9f0d1e22…",
  "clientCount": 3                  // optional; omit to leave unchanged
}
```

Response `200`:

```json
{ "data": { "expiresAt": "2026-05-15T12:04:00Z" }, "error": null }
```

Errors:
- `404 not_found` — no entry with that `streamId` (already expired or never existed)
- `403 forbidden` — `ownerToken` doesn't match the entry's token

### `PATCH /streams/<streamId>`

Update mutable fields of an existing entry without re-registering.
Authenticated by `ownerToken`. Intended for "host changed shader
mid-stream" and similar updates.

Request body (every field except `ownerToken` is optional):

```jsonc
{
  "ownerToken":       "9f0d1e22…",
  "name":             "My CRT setup (updated)",
  "shader":           "crt/crt-easymode.glslp",
  "resolution":       { "w": 1280, "h": 720 },
  "fps":              60,
  "codec":            "h265",
  "passwordRequired": true,
  "endpoint":         "https://new-url.example.com",
  "endpointMode":     "tunnel-cloudflare"
}
```

Response `204 No Content`.

Errors: same as `/heartbeat` (`404`, `403`, `400`).

### `DELETE /streams/<streamId>`

Explicit removal. Used when the host stops streaming or toggles
publishing off, so the entry disappears within ~1 second instead of
waiting for TTL.

Request body:

```jsonc
{ "ownerToken": "9f0d1e22…" }
```

Response `204 No Content`. Idempotent — deleting an already-gone entry
also returns `204`.

Errors:
- `403 forbidden` — token mismatch (entry exists but caller isn't the owner)

### `GET /streams`

Public list. No authentication. The response excludes `ownerToken` from
every entry.

Query parameters (all optional):

| Param   | Type     | Default     | Notes                                                            |
|---------|----------|-------------|------------------------------------------------------------------|
| `sort`  | string   | `clients`   | `clients` (desc), `recent` (lastHeartbeatAt desc), `name` (asc). |
| `q`     | string   | —           | Case-insensitive substring match against `name` and `hostNickname`. |
| `limit` | int      | `100`       | `1..500`.                                                        |

Response `200`:

```json
{
  "data": {
    "streams": [ /* …entries… */ ],
    "totalCount": 47
  },
  "error": null
}
```

`totalCount` is the total before `limit` was applied, so a client can
tell when more results exist than were returned.

### `GET /streams/<streamId>`

Single entry. Useful when joining via a deep link.

Response `200`:

```json
{ "data": { /* …entry, no ownerToken… */ }, "error": null }
```

`404 not_found` if the entry is gone.

### `POST /streams/<streamId>/report`

Anyone can flag an entry for the maintainer to review out-of-band. No
auth required (deliberately — a report mechanism the moderator
controls is more useful than one tied to having an account).

Request body:

```jsonc
{ "reason": "explicit name", "reporterContact": "optional email or @handle" }
```

Response `202 Accepted`. The directory logs the report and the
moderator decides what to do with it. **No automatic takedown.**

Rate-limited per source IP to prevent griefing.

## Rate limits

All limits are token-bucket per **source IP**. Keying per-streamId
would require parsing the request body in middleware, and the
practical abuse surface (filling the DB, spamming reads, griefing the
report queue) is well-covered by IP-scoped limits with generous
ceilings that accommodate several concurrent streams from the same
home network.

| Endpoint                       | Default per IP      | Env override                          |
|--------------------------------|---------------------|---------------------------------------|
| `POST /register`               | 60 / hour           | `DIRECTORY_RATE_REGISTER_PER_HOUR`    |
| `POST /heartbeat`              | 600 / hour          | `DIRECTORY_RATE_HEARTBEAT_PER_HOUR`   |
| `PATCH /streams/<id>`          | 60 / hour           | `DIRECTORY_RATE_PATCH_PER_HOUR`       |
| `DELETE /streams/<id>`         | unlimited           | —                                     |
| `GET /streams`                 | 600 / hour          | `DIRECTORY_RATE_LIST_PER_HOUR`        |
| `GET /streams/<id>`            | 600 / hour          | (same as `/streams`)                  |
| `POST /streams/<id>/report`    | 30 / hour           | `DIRECTORY_RATE_REPORT_PER_HOUR`      |
| `GET /health`                  | unlimited           | —                                     |

The defaults are tuned to be generous enough that legitimate use —
including a developer restarting the host process many times during a
debugging session — doesn't hit them. Production deploys can tighten
the values via the env vars listed above.

`POST /heartbeat` at 600/h supports up to ~5 concurrent streams from
one IP at the natural 30-second cadence, with headroom for retries.
`POST /register` at 60/h accommodates active iteration (one publish
toggle / app restart per minute on average) while still preventing a
single IP from filling the database in seconds.

Hitting a limit → `429` with `error.code = "rate_limited"` and a
`Retry-After` header in seconds (rounded up, always >= 1).

## TTL semantics

- A fresh entry's `expiresAt` is `now + DIRECTORY_TTL_SECONDS` (default 120 s).
- Each `POST /heartbeat` resets `expiresAt` to `now + TTL`.
- A background reaper sweeps the database every ~60 s and deletes any
  entry whose `expiresAt < now`.
- Clients should beat at half the TTL or faster (the RetroCapture host
  uses 30 s).
- An entry can disappear from `GET /streams` up to one reaper-cycle
  after `expiresAt` even though it's logically expired — that's a
  visible-but-stale window of < 60 s.

## Password handling

The directory only stores `passwordRequired: bool`. The actual password
never crosses this wire. Clients joining a passworded stream prompt the
user for the password and authenticate **directly against the host's
HTTP surface** — not against the directory.

The host gates the *entire* HTTP surface behind the password whenever
one is configured, not just `/raw` and `/meta`. That includes the web
portal HTML, its JS / CSS / font assets, the MPEG-TS `/stream`, the
PWA service worker, `/meta`, and the raw `/raw` endpoint. Two auth
schemes are accepted on the same routes:

- **Basic** — `Authorization: Basic base64("any:<password>")`, what
  any browser hitting `https://stream-host/` will produce after the
  first `401`. Username is ignored; only the password is checked.
- **Bearer** — `Authorization: Bearer <sha256-hex(password)>`, what
  the native client sends so the password itself never crosses the
  wire after the user types it once.

The host compares the supplied value (either the basic-auth password
or the bearer hash) against the sha256 of its configured password.
Wrong / missing credentials return `401 WWW-Authenticate: Basic
realm="RetroCapture"` so a browser pops the login dialog.

## Privacy

- `publicIp` is recorded for every entry (taken from the registering
  request's source IP). It is returned in `GET /streams` responses
  because a client browsing the directory must know where to connect,
  and `endpoint` already implies the IP in most modes. Hosts who don't
  want their home IP listed should use `endpointMode: "tunnel-cloudflare"`
  or `"custom"` with a tunnelled URL.
- `ownerToken` is never returned in any read endpoint. Only the host
  that registered it sees it (once, in the register response).
- The directory does not record viewer / browse identities. There is no
  cookie, no session.

## Discovery

The canonical directory URL is **`http://directory.retrocapture.com`** —
that's the default the binary ships with for both the in-app browse
window and the `--browse-directory` CLI flag. It's plain HTTP because
RetroCapture's embedded HTTP client doesn't speak TLS yet; the same
host also serves HTTPS for browsers and `curl` reaching the service
from outside the app.

The default is overridable on the client side (`--directory-url` on
the CLI, or the "Directory URL" field under Advanced in the publish
section of the Streaming tab), so anyone can run their own instance
and point a client at it. The default URL is baked into
RetroCapture's binary and controlled by the project maintainer.
