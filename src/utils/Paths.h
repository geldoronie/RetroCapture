#pragma once

#include <string>

/**
 * Resolução centralizada dos diretórios usados pela aplicação.
 *
 * Linux segue XDG Base Directory; Windows segue Known Folders
 * (`SHGetKnownFolderPath`). Cada getter:
 *   1. Honra um env var de override específico (`RETROCAPTURE_*_DIR`)
 *      pra AppImage / CI / packagers / dev.
 *   2. Aplica a regra padrão do OS.
 *   3. Para diretórios graváveis, cria `directories` se não existir
 *      (lazy, na primeira chamada que precisa escrever).
 *
 * Strings retornadas são UTF-8, sem trailing separator. Vazias só em
 * caso de erro grave (sem `$HOME`, sem `%APPDATA%` — não esperado).
 */
class Paths {
public:
    // Read-only: shaders bundlados, templates do web portal, presets seed,
    // SSL bundle. Procura no install system-wide e cai pra `<exe>/assets/`
    // pra builds portáveis / dev tree.
    static std::string getReadOnlyAssetsDir();

    // User config: settings tipo resolução, FPS, overscan, device, audio,
    // streaming. Linux: $XDG_CONFIG_HOME/retrocapture (~/.config/retrocapture).
    // Windows: %APPDATA%\RetroCapture (FOLDERID_RoamingAppData).
    static std::string getUserConfigDir();

    // User data: presets salvos pelo usuário, thumbnails, SSL certs do user.
    // Linux: $XDG_DATA_HOME/retrocapture (~/.local/share/retrocapture).
    // Windows: %APPDATA%\RetroCapture\data (mesma raiz que config pra moves
    // atômicos).
    static std::string getUserDataDir();

    // Cache: estado regenerável (futura compile cache de shaders, miniaturas
    // remotas, etc.). Linux: $XDG_CACHE_HOME/retrocapture (~/.cache/...).
    // Windows: %LOCALAPPDATA%\RetroCapture\Cache (FOLDERID_LocalAppData,
    // não-roamed).
    static std::string getCacheDir();

    // Default para gravações. Pode ser sobrescrito pelo user em settings.
    // Linux: $XDG_VIDEOS_DIR/RetroCapture (xdg-user-dirs) com fallback
    // ~/Videos/RetroCapture. Windows: <Videos>\RetroCapture
    // (FOLDERID_Videos).
    static std::string getDefaultRecordingsDir();

    // Diretório do executável atual. Útil pra fallback portátil
    // (read-only assets ao lado do binário).
    static std::string getExecutableDir();

    // Migra dados legacy de instalações anteriores pra os diretórios novos.
    // One-shot e idempotente — vê marcador `MIGRATED.txt` no destino.
    // Move (não copia) presets, thumbnails e SSL do antigo
    // `~/.config/retrocapture/assets/` (Linux) ou `%APPDATA%\RetroCapture\assets\`
    // (Windows) pra `getUserDataDir()`. Retorna true se algo foi movido.
    static bool migrateLegacyDataIfNeeded();

private:
    Paths() = delete;
};
