# `platform/services/`

Backend programs that run continuously and expose an HTTP (or similar)
API. Each service lives in its own subdirectory with its own dependency
manifest and is deployable in isolation.

## Inventory

| Service     | Purpose                                                       | Tracks issue |
|-------------|---------------------------------------------------------------|--------------|
| `directory` | Public registry of opt-in RetroCapture streams ("what's online right now"). | #49 |

New services should be appended to this table when they're scaffolded.

## Conventions

These are defaults — a service can deviate when there's a concrete
reason, but the deviation should be documented in that service's
`README.md`.

### Language

**Go**, targeting the latest stable release. Reasons:

- Single static binary, easy `scp + systemctl restart` deploy.
- HTTP server in the standard library, no framework decision needed.
- Goroutines + channels make background workers (TTL reapers, rate
  limiters, etc.) trivial.
- ~10–30 MB resident memory per service, comfortable on small VMs.

### Storage

**SQLite with WAL** for any service that needs persistence. It's a
single file, easy to back up (`rsync`), zero servers to manage, and
sufficient for the throughput we expect at alpha scale.

Services that genuinely outgrow SQLite (sustained > 1 k writes/second,
multi-writer, etc.) can migrate to Postgres or similar — but until that
threshold is hit, the operational cost of SQLite is roughly zero.

### Layout inside a service

```
<service>/
├── README.md
├── go.mod
├── go.sum
├── Dockerfile
├── docker-compose.yml
├── cmd/
│   └── <service>/
│       └── main.go         entrypoint, flag parsing, lifecycle
├── internal/
│   ├── api/                HTTP handlers
│   ├── store/              persistence layer + migrations
│   ├── <other>/            cohesive sub-packages as needed
│   └── config/             env / flag wiring
└── test/                   integration tests (run against real SQLite)
```

`cmd/<service>/main.go` is the only `main` package; everything else
lives under `internal/` so it can't accidentally be imported by another
service.

### HTTP surface

- Every service exposes `GET /health` returning `200 {"status":"ok"}`
  for liveness / readiness probes.
- JSON request and response bodies, `Content-Type: application/json`.
- Error response shape: `{"error":{"code":"…","message":"…"}}`.
- Success response wraps payload in `{"data":…}` to leave room for
  pagination / metadata fields on the same response.

### Logging

Structured JSON via `log/slog` stdlib. One line per request at minimum,
including method, path, status, latency, source IP. No `fmt.Println`
sprinkled around.

### Dev workflow

Orchestration lives one level up at `platform/docker-compose.yml` so a
single command can bring the whole platform stack online. To work on a
single service in isolation:

```bash
cd platform
docker compose up <service>                                 # run it
docker compose --profile test run --rm <service>-test        # test it
```

Each service's `Dockerfile` (multi-stage: `deps → test → build →
runtime`) is the source of truth for how it builds. CI mirrors the
same Dockerfile so behaviour matches.

### Naming

Substantive singular, lowercase, no `service-` / `srv-` / `-api`
suffix — those are already implied by living in `platform/services/`.

✅ `directory`, `auth`, `shader-bundle`
❌ `directory-service`, `srv-auth`, `shader-bundle-api`
