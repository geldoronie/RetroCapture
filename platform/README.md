# `platform/`

Supporting infrastructure for RetroCapture that is **not** part of the
desktop application itself.

The desktop app lives in `src/`. Anything in `platform/` exists to make
the desktop app more useful — backend services it talks to, web
frontends that complement it, infrastructure-as-code that deploys them,
etc.

## Layout

```
platform/
├── services/     backend programs that run continuously
│                 (HTTP/gRPC/etc), e.g. the public stream directory
├── frontends/    web UIs (landing page, admin dashboard, …)         [future]
├── infra/        IaC: terraform / pulumi / k8s manifests             [future]
└── shared/       libraries reused across services / frontends        [future]
```

Only directories that exist today are populated. The rest are listed
above as a forward-looking guide so future additions land in a
predictable place.

## What goes here vs. `src/`

| Lives in `src/` | Lives in `platform/` |
|---|---|
| The capture / encoding / playback pipeline | A service the app calls over HTTP |
| OpenGL renderer, shaders, ImGui UI | A web frontend the user opens in a browser |
| Anything compiled into the `retrocapture` binary | Anything that runs on a server, not on the user's machine |
| C++ source | Any language appropriate to the job (Go, TS, …) |

Wire protocols between the two sides (e.g.
`docs/REMOTE_STREAM_PROTOCOL.md`,
`docs/DIRECTORY_PROTOCOL.md`) live in the top-level `docs/` directory —
they're contracts, not implementation details, so they sit in a neutral
location both sides can consult.

## Conventions

- Each subtree (`services/`, `frontends/`, …) has its own `README.md`
  documenting language / framework / deployment conventions for that
  category.
- Builds are independent of the C++ app's CMake. `platform/` does not
  appear in `CMakeLists.txt`.
- Each individual deliverable (one service, one frontend) lives in its
  own subdirectory with its own dependency manifest (`go.mod`,
  `package.json`, …), so it can be developed, versioned, and eventually
  extracted to its own repository without untangling shared state.
