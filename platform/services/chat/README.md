# `chat` service

Live chat hosted at `chat.retrocapture.com`. Provides per-stream chat rooms (auto-spawned when a stream publishes to the directory) and standalone rooms (community channels, not yet exposed in the UI). The endpoint surface and message envelopes are summarized below.

Tracking issue: [#84](../../../issues/84).

## Quick start

Orchestration lives at `platform/docker-compose.yml`. From there:

```bash
cd platform
docker compose up chat                                    # run it
docker compose --profile test run --rm chat-test           # test it
```

The service binds to `:8082` by default (override with `CHAT_PORT`) and persists to `./services/chat/data/chat.db`. Verify it's up:

```bash
curl http://localhost:8082/health
# → {"data":{"status":"ok","protocol_version":1},"error":null}
```

## Endpoints

| Method | Path                            | Purpose                                                                                   |
| ------ | ------------------------------- | ----------------------------------------------------------------------------------------- |
| GET    | `/health`                       | Liveness probe + protocol version advertisement.                                          |
| GET    | `/rooms/:roomId`                | Room metadata (kind, owner, title, participant_count).                                    |
| GET    | `/rooms/:roomId/history`        | Paginated chat history, newest first, cursor-based.                                       |
| GET    | `/rooms/by-stream/:streamId`    | Resolve a stream id to its linked room (creates the room if missing).                     |
| GET    | `/ws?room=<id>` or `?stream=<id>` | WebSocket upgrade. Send `hello`, receive `welcome`, then exchange `post` / `message`. |

The message envelopes and storage schema are defined by the service
implementation in this directory.

## Configuration

All env vars are read once at start-up; restart to pick up changes.

| Variable                  | Default              | Notes                                                                                       |
| ------------------------- | -------------------- | ------------------------------------------------------------------------------------------- |
| `CHAT_PORT`               | `8082`               | TCP port to bind. The directory uses 8081 — keep them separate.                              |
| `CHAT_DB_PATH`            | `./data/chat.db`     | SQLite database location. Created if missing.                                                |
| `CHAT_LOG_LEVEL`          | `info`               | `debug` / `info` / `warn` / `error`.                                                         |
| `CHAT_RATE_POST_PER_10S`  | `5`                  | Per-WebSocket-connection post rate cap.                                                      |
| `CHAT_TRUST_PROXY_HEADERS`| `false`              | When the service is behind a reverse proxy (Cf, nginx), honor `X-Forwarded-For` / `Cf-Connecting-Ip` for the real client IP. |

## Layering

```
cmd/chat/main.go               wires config + store + room hub + HTTP
internal/api/                  HTTP handlers, WebSocket upgrade, wire types
internal/room/                 in-memory hub: per-room broadcast + participant tracking
internal/store/                SQLite persistence (rooms, messages)
internal/config/               env-var parsing
internal/ratelimit/            per-connection / per-IP rate limit primitives
```

The `internal/api` package only knows about HTTP / WebSocket and the wire types; it calls into `internal/store` for persistence and `internal/room` for fanout. The room hub is in-memory and not durable — restart loses participant tracking, but messages persist via the store.

## Status (v0.5)

- Anonymous identity only (`POST /auth/verify` for directory-account JWTs lands in v1).
- Stream-linked rooms have full UI flow; standalone rooms exist in schema but no UI/discovery yet.
- No moderation REST endpoints — adds in v1 (`POST /rooms/:id/moderate`).
- Word filter / slow mode UI deferred to v1.

## See also

- [Issue #84](../../../issues/84)
- Sibling [`directory`](../directory/) service for the catalogue half.
