# `directory` service

Public registry of opt-in RetroCapture streams. A host that wants its
stream to appear in the in-app browse list publishes here; clients
fetch the list and decide which one to join. The service never sees
stream bytes — only metadata.

Tracking issue: [#49](../../../issues/49).
The endpoint surface is summarized below.

## Quick start

Orchestration lives at `platform/docker-compose.yml`. From there:

```bash
cd platform
docker compose up directory                                    # run it
docker compose --profile test run --rm directory-test           # test it
```

The service binds to `:8081` by default (override with `DIRECTORY_PORT`)
and persists to `./services/directory/data/directory.db`. Verify it's
up:

```bash
curl http://localhost:8081/health
# → {"data":{"status":"ok","protocol_version":1},"error":null}
```

## Configuration

All configuration is via environment variables. Defaults are tuned for
local development.

| Variable                | Default                | Purpose                                              |
|-------------------------|------------------------|------------------------------------------------------|
| `DIRECTORY_PORT`        | `8081`                 | TCP port the HTTP server binds to.                   |
| `DIRECTORY_DB_PATH`     | `./data/directory.db`  | SQLite file path. Parent directory must exist.       |
| `DIRECTORY_LOG_LEVEL`   | `info`                 | `debug` / `info` / `warn` / `error`.                 |
| `DIRECTORY_TTL_SECONDS` | `120`                  | How long an entry survives without a fresh heartbeat. |

## Endpoints

At Phase 1 bootstrap only `GET /health` is implemented; the rest land
incrementally as Phase 1 of #49 progresses.

## Local development

The service has no runtime dependencies beyond SQLite (which is
embedded). For the most ergonomic loop, all commands run from
`platform/`:

```bash
docker compose up directory                              # run
docker compose --profile test run --rm directory-test     # test
docker compose down                                       # stop
rm -rf services/directory/data/                          # nuke local state
```

If you do have Go installed locally and prefer not to use Docker:

```bash
cd platform/services/directory
go run ./cmd/directory
```

CI and production both go through the Dockerfile so the local
non-Docker path is a convenience, not the canonical way to run it.

## Layout

See [`platform/services/README.md`](../README.md) for the convention
this service follows.
