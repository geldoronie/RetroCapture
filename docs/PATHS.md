# Storage Layout

RetroCapture splits storage into four roles, mapped to platform conventions
(XDG Base Directory on Linux, Known Folders on Windows). All path
resolution goes through `src/utils/Paths.h`; no subsystem rolls its own
`~/.config/...` or `%APPDATA%` lookup.

## Roles

| Role | Linux (XDG) | Windows |
|---|---|---|
| **Read-only assets** (shaders, web portal, SSL bundle, seed presets — shipped with the app) | `$XDG_DATA_DIRS` → `/usr/local/share/retrocapture/` → `/usr/share/retrocapture/` → `<exe>/../share/retrocapture/` → `<exe>/assets/` (portable / dev) | `<install>\assets\` (next to `retrocapture.exe`) → `Program Files\RetroCapture\assets\` |
| **User config** (settings: resolution, FPS, overscan, device, audio, streaming) | `$XDG_CONFIG_HOME/retrocapture/` → `~/.config/retrocapture/` | `%APPDATA%\RetroCapture\` (`CSIDL_APPDATA`) |
| **User data** (saved capture presets, thumbnails, user SSL certs) | `$XDG_DATA_HOME/retrocapture/` → `~/.local/share/retrocapture/` | `%APPDATA%\RetroCapture\data\` |
| **User cache** (regenerable state) | `$XDG_CACHE_HOME/retrocapture/` → `~/.cache/retrocapture/` | `%LOCALAPPDATA%\RetroCapture\Cache\` (`CSIDL_LOCAL_APPDATA`) |
| **Recordings** (user-overridable in settings) | `$XDG_VIDEOS_DIR/RetroCapture/` (xdg-user-dirs) → `~/Videos/RetroCapture/` | `<Videos>\RetroCapture\` (`CSIDL_MYVIDEO`) |

## Resolution rules

1. **Env-var override** wins for everything (AppImage / CI / packagers /
   dev). One per role:
   - `RETROCAPTURE_ASSETS_DIR` — read-only assets
   - `RETROCAPTURE_CONFIG_DIR` — user config
   - `RETROCAPTURE_DATA_DIR` — user data
   - `RETROCAPTURE_CACHE_DIR` — user cache
   - `RETROCAPTURE_PRESETS_DIR`, `RETROCAPTURE_THUMBNAILS_DIR` —
     finer-grained overrides specific to the preset manager.
2. **OS default** when no override set (table above).
3. **Lazy creation**: writable directories are `mkdir -p`'d on first
   write (`Paths::getUserConfigDir()` etc.), so a fresh user account
   needs no installer step.
4. **Read-only fall-through**: the `getReadOnlyAssetsDir()` chain stops
   at the first directory that exists; portable builds find assets
   next to the binary, system installs find them under
   `/usr/share/retrocapture/`.

## Migration from previous versions

Pre-XDG layouts (everything inside `~/.config/retrocapture/assets/` or
`%APPDATA%\RetroCapture\assets\`) are migrated automatically on first
run by `Paths::migrateLegacyDataIfNeeded()`. Files are **moved**, not
copied. A `MIGRATED.txt` marker is written into the new user-data dir
so the migration runs only once.

If you want to clear the marker and re-migrate manually:

```sh
# Linux
rm ~/.local/share/retrocapture/MIGRATED.txt

# Windows
del %APPDATA%\RetroCapture\data\MIGRATED.txt
```

## Quick examples

Where do my files end up?

| Action | Linux | Windows |
|---|---|---|
| Saving a capture preset | `~/.local/share/retrocapture/presets/<name>.json` | `%APPDATA%\RetroCapture\data\presets\<name>.json` |
| Settings file | `~/.config/retrocapture/config.json` | `%APPDATA%\RetroCapture\config.json` |
| Default recording | `~/Videos/RetroCapture/recording_*.mp4` | `<Videos>\RetroCapture\recording_*.mp4` |
| User SSL cert | `~/.local/share/retrocapture/ssl/server.crt` | `%APPDATA%\RetroCapture\data\ssl\server.crt` |

System installs (Linux only):

| File | Location |
|---|---|
| Binary | `/usr/local/bin/retrocapture` (default `CMAKE_INSTALL_PREFIX`) |
| Read-only assets | `/usr/local/share/retrocapture/` |
| Shaders | `/usr/local/share/retrocapture/shaders/shaders_glsl/` |
