# Remote stream protocol

This document describes the wire format between a RetroCapture **server**
(a host running capture + shader + stream) and a RetroCapture **client**
that wants to consume the host's stream while applying the shader **locally**
at its own native resolution.

Status: **Phase 1 — metadata endpoint only.** The `/raw` stream endpoint,
client mode, asset bundle transport and live-update WebSocket are tracked in
[issue #47](https://github.com/geldoronie/RetroCapture/issues/47).

---

## Endpoint surface

| Path | Purpose | Phase |
| --- | --- | --- |
| `/stream` | Existing post-shader MPEG-TS stream — unchanged, consumed by VLC / mpv / ffplay / the web portal. | already shipped |
| `/meta` | JSON snapshot of the active shader pipeline + source state. **(this document)** | Phase 1 |
| `/raw` | Pre-shader MPEG-TS stream, encoded straight from the capture pipeline. | Phase 2 |
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

  // Source the server is capturing from. The client mirrors dimensions,
  // frame rate, overscan and hardware controls; display-side preferences
  // (fullscreen / monitor index) are NOT mirrored — those are client-local.
  "source": {
    // One of "none", "v4l2", "directshow".
    "type":   "v4l2",
    // V4L2 path (e.g. "/dev/video0") or DirectShow device name.
    // Empty when type == "none".
    "device": "/dev/video0",
    "width":  640,
    "height": 480,
    "fps":    60,
    // Source-side overscan correction (percent on each axis, plus the
    // X/Y lock toggle from the UI).
    "overscan": { "x": 0.0, "y": 0.0, "locked": true },
    // Hardware controls exposed by the active source. Source-type-aware:
    //   - "v4l2"       — populated from V4L2 ioctls.
    //   - "directshow" — empty for now; a unified read-only DirectShow
    //                    controls getter is tracked alongside Phase 2.
    //   - "none"       — empty.
    "controls": [
      {
        "name":      "brightness",
        "value":     50,
        "min":       -100,
        "max":       100,
        "step":      1,
        "available": true
      }
    ]
  },

  // Software image processing applied after capture, before shader. The
  // client mirrors these because they affect the look of the source frame.
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

Responses MUST NOT be cached by the client. Phase 6 will add a WebSocket
upgrade on this same path so the client receives parameter / preset
deltas without polling; until then the client polls on a short interval
(suggested: 1 s during steady state).

---

## Versioning

The wire format is versioned via `protocolVersion`. Breaking changes
bump the integer; additive changes (new optional fields) keep it stable.

| protocolVersion | Notes |
| --- | --- |
| 1 | Initial Phase 1 snapshot. Blocks: `shader`, `source` (type, device, dims, overscan, controls), `image`, `streaming`. |

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
    "type":     "v4l2",
    "device":   "/dev/video0",
    "width":    640,
    "height":   480,
    "fps":      60,
    "overscan": { "x": 0.0, "y": 0.0, "locked": true },
    "controls": [
      { "name": "brightness", "value": 50, "min": -100, "max": 100, "step": 1, "available": true },
      { "name": "contrast",   "value": 40, "min": -100, "max": 100, "step": 1, "available": true }
    ]
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
