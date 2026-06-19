# Remote stream protocol

This document describes the wire format between a RetroCapture **server**
(a host running capture + shader + stream) and a RetroCapture **client**
that wants to consume the host's stream while applying the shader **locally**
at its own native resolution.

Status: **Phases 1–6 landed.** Asset bundle transport is tracked in
[issue #47](https://github.com/geldoronie/RetroCapture/issues/47).

---

## Endpoint surface

| Path | Purpose | Phase |
| --- | --- | --- |
| `/stream` | Existing post-shader MPEG-TS stream — unchanged, consumed by VLC / mpv / ffplay / the web portal. May or may not have a shader applied, depending on the host's per-pipeline override. | already shipped |
| `/meta` | JSON snapshot of the active shader pipeline + source state. | Phase 1 |
| `/raw` | **Always** pre-shader MPEG-TS stream, encoded from the same capture pipeline as `/stream` but tapped before the shader pass. | Phase 2 |
| `/meta/shader-bundle?preset=<name>` | Tarball with the preset file plus every `#include`d file and referenced LUT texture. | Phase 4 |

The client only needs the server's **base URL** (e.g. `http://host:8080`) —
it discovers the endpoints by convention.

---

## `GET /meta`

Returns a single JSON snapshot of the server's current shader-pipeline and
source state. Content-type is `application/json`.

### Response schema

```jsonc
{
  // Bumped only when the wire format breaks compatibility. A client that
  // doesn't recognize the value MUST refuse to connect.
  "protocolVersion": 1,

  // Server build version (matches PROJECT_VERSION in CMakeLists.txt).
  "serverVersion": "0.6.0",

  // Active shader pipeline state.
  "shader": {
    // Whether a preset is currently loaded and engaged.
    "active": true,
    // Master pipeline toggle from the UI / web portal.
    "pipelineEnabled": true,
    // Preset name relative to the shader root (e.g. "crt/crt-mattias.glslp").
    // Empty when no preset is loaded.
    "preset": "crt/crt-mattias.glslp",
    // Opaque content hash of the preset file. Format: "<algo>:<hex>".
    // Phase 1 uses FNV-1a 64-bit ("fnv1a64:..."), which is sufficient for
    // cache-invalidation and not advertised as cryptographic. The client
    // compares this against its locally-cached preset to decide whether to
    // refetch via the shader-bundle endpoint (Phase 4).
    "presetHash": "fnv1a64:1a2b3c4d5e6f7a8b",
    // Current parameter values + their definitions. Empty array when no
    // shader is active.
    "parameters": [
      {
        "name":         "CURVATURE",
        "value":        0.05,
        "defaultValue": 0.04,
        "min":          0.0,
        "max":          0.10,
        "step":         0.01
      }
    ]
  },

  // Source-frame configuration as it will arrive on /raw.
  //
  // The client's "source" is always the remote stream itself — the server's
  // capture-chain specifics (V4L2 vs. DirectShow, /dev path, hardware ioctl
  // values) are intentionally NOT carried here, since the client cannot act
  // on them. What IS carried is the configuration that describes how each
  // frame is shaped before reaching the wire.
  "source": {
    "width":  640,
    "height": 480,
    "fps":    60,
    // Source-side overscan correction (percent on each axis, plus the
    // X/Y lock toggle from the UI).
    "overscan": { "x": 0.0, "y": 0.0, "locked": true }
  },

  // Image-processing settings applied between capture and shader. Carried
  // for client-side display ("this is how the server is configured") and,
  // depending on where /raw is tapped in Phase 2, possibly for the client
  // to reproduce locally as well.
  "image": {
    // Multiplicative software brightness / contrast (1.0 = neutral).
    "brightness":     1.0,
    "contrast":       1.0,
    // Whether the renderer preserves the source aspect ratio.
    "maintainAspect": true,
    // Output resolution applied after shader, before drawing to window.
    // 0/0 means "match shader output".
    "outputWidth":    0,
    "outputHeight":   0
  },

  // Whether /stream and /raw are currently producing frames.
  "streaming": {
    "active": true,
    // Convenience URL for /stream — already adjusted for proxy prefixes.
    "url":    "http://host:8080/stream"
  }
}
```

### Status codes

- `200 OK` — snapshot returned.
- `500 Internal Server Error` — server is not fully initialized (e.g. the
  capture pipeline has not been brought up yet). Body contains a JSON
  `{ "error": "..." }`.

### Caching

Responses MUST NOT be cached by the client.

### Live updates via Server-Sent Events (Phase 6)

The same `/meta` path accepts an SSE upgrade. When the client sends:

```
GET /meta HTTP/1.1
Accept: text/event-stream
```

…the server responds with `Content-Type: text/event-stream` and keeps
the connection open, pushing a `data: {snapshot}\n\n` event whenever
the snapshot changes. The first event sent is the current snapshot
(equivalent to the regular `GET /meta` body). A `: keepalive\n\n`
comment is sent every 30 s while idle.

Clients that accept `application/json` (no `Accept: text/event-stream`)
continue to get the one-shot snapshot — no breaking change for older
consumers. The reference client (`RemoteMetaSync` in this repo) tries
SSE first and transparently falls back to short-poll if the server
doesn't advertise `text/event-stream`.

---

---

## `GET /raw`

Identical to `/stream` in wire format (MPEG-TS over HTTP, `Content-Type:
video/mp2t`) but the video frames are tapped from the capture pipeline
**before** the shader pass. Audio is the same as `/stream`. Codec config
(bitrate, codec, preset) is shared with `/stream` — running a `/raw`
client at a different bitrate than the host's `/stream` is not supported
in Phase 2.

Properties:

- The host's per-pipeline shader-bypass toggle ("Apply shader pipeline"
  in the UI) flips `/stream`'s contents but **does not affect `/raw`** —
  `/raw` is always pre-shader by contract.
- The raw encoder is **lazy**: when no clients are connected to `/raw`,
  no pre-shader frames are pushed into its pipeline. The CPU cost of
  the second encoder only materializes while a remote client is
  actively consuming.
- A single MPEG-TS muxer state per output — a brand-new `/raw` client
  starts receiving packets from the next keyframe boundary.
- **Frame orientation (since 0.8.2-alpha):** `/raw` carries the capture
  in the host's canonical **bottom-up** orientation — the same as the
  post-shader `/stream` and the recording encoder. Earlier builds tapped
  the raw source one vertical flip off from that, so a 0.8.2-alpha client
  and an older host (or vice-versa) will render the remote picture
  upside-down. This was an intentional standardization break: there is no
  longer a per-tap orientation special-case, and the client compensates
  the remote feed's inversion uniformly (with or without a client-side
  shader). Mixing 0.8.2-alpha with pre-0.8.2-alpha peers is unsupported.

```console
$ ffplay http://host:8080/raw     # raw source feed, no shader
$ ffplay http://host:8080/stream  # current /stream contents
```

A future RetroCapture client (Phase 3+) consumes `/raw` and applies the
shader described in `/meta` locally at its own native render resolution.

---

## Versioning

The wire format is versioned via `protocolVersion`. Breaking changes
bump the integer; additive changes (new optional fields) keep it stable.

| protocolVersion | Notes |
| --- | --- |
| 1 | Initial Phase 1 snapshot. Blocks: `shader`, `source` (dims, fps, overscan), `image`, `streaming`. |

---

## Example

```console
$ curl -s http://localhost:8080/meta | jq .
{
  "protocolVersion": 1,
  "serverVersion": "0.6.0",
  "shader": {
    "active": true,
    "pipelineEnabled": true,
    "preset": "crt/crt-mattias.glslp",
    "presetHash": "fnv1a64:de5f853079d66290",
    "parameters": [
      { "name": "CURVATURE", "value": 0.5, "defaultValue": 0.5, "min": 0.0, "max": 1.0,  "step": 0.05 },
      { "name": "SCANSPEED", "value": 1.0, "defaultValue": 1.0, "min": 0.0, "max": 10.0, "step": 0.5  }
    ]
  },
  "source": {
    "width":    640,
    "height":   480,
    "fps":      60,
    "overscan": { "x": 0.0, "y": 0.0, "locked": true }
  },
  "image": {
    "brightness":     1.0,
    "contrast":       1.0,
    "maintainAspect": true,
    "outputWidth":    0,
    "outputHeight":   0
  },
  "streaming": { "active": true, "url": "http://localhost:8080/stream" }
}
```
