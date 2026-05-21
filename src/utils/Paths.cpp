#include "Paths.h"
#include "Logger.h"
#include "FilesystemCompat.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shlobj.h>      // SHGetFolderPathW, CSIDL_*
    // shell32 já vinculado pelo CMakeLists do build Windows.
#else
    #include <unistd.h>      // readlink
    #include <sys/types.h>
    #include <pwd.h>         // getpwuid as last-resort HOME fallback
    #include <fstream>
    #include <sstream>
#endif

namespace {

// Tenta env var; se vazia ou não-set, retorna fallback. Strip trailing
// separator pra normalizar.
std::string envOr(const char *name, const std::string &fallback)
{
    const char *v = std::getenv(name);
    if (v && *v)
    {
        std::string s(v);
        while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
        {
            s.pop_back();
        }
        return s;
    }
    return fallback;
}

#ifdef _WIN32

// Converte UTF-16 (Windows) pra UTF-8. `&out[0]` em vez de `out.data()` pra
// não retornar `const char*` no MinGW antigo (libstdc++ < 7) que esperava
// LPSTR (`char*`) no WideCharToMultiByte.
std::string wideToUtf8(const wchar_t *w)
{
    if (!w) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], needed, nullptr, nullptr);
    return out;
}

// Wrapper pra SHGetFolderPathW (XP+). Devolve "" se falhar. Usa CSIDLs
// porque SHGetKnownFolderPath/FOLDERID_* não estão expostos pelos headers
// do MXE MinGW que usamos no cross-compile.
std::string getShellFolder(int csidl)
{
    wchar_t pathW[MAX_PATH] = {0};
    HRESULT hr = SHGetFolderPathW(nullptr, csidl, nullptr, 0 /* SHGFP_TYPE_CURRENT */, pathW);
    if (FAILED(hr))
    {
        return {};
    }
    return wideToUtf8(pathW);
}

#else

// $HOME com last-resort fallback pra getpwuid (containers / cron).
std::string getHomeDir()
{
    const char *h = std::getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return {};
}

// Lê $XDG_VIDEOS_DIR de ~/.config/user-dirs.dirs (formato shell:
//   XDG_VIDEOS_DIR="$HOME/Videos"
// ). Retorna "" se não definido.
std::string readXdgVideosDir(const std::string &home)
{
    fs::path userDirs = fs::path(home) / ".config" / "user-dirs.dirs";
    if (!fs::exists(userDirs)) return {};

    std::ifstream in(userDirs);
    if (!in.is_open()) return {};

    std::string line;
    while (std::getline(in, line))
    {
        const std::string key = "XDG_VIDEOS_DIR=";
        auto pos = line.find(key);
        if (pos == std::string::npos) continue;
        std::string value = line.substr(pos + key.size());
        // Strip aspas, expand $HOME.
        if (!value.empty() && value.front() == '"') value.erase(0, 1);
        if (!value.empty() && value.back() == '"') value.pop_back();
        const std::string token = "$HOME";
        size_t p = value.find(token);
        if (p != std::string::npos)
        {
            value.replace(p, token.size(), home);
        }
        return value;
    }
    return {};
}

#endif

void ensureDir(const std::string &dir)
{
    if (dir.empty()) return;
    // FilesystemCompat (MinGW antigo) não expõe a sobrecarga com std::error_code,
    // então usamos try/catch — funciona igual nos dois lados.
    try
    {
        fs::create_directories(fs::path(dir));
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Paths: failed to create directory " + dir + " (" + e.what() + ")");
    }
}

} // namespace

std::string Paths::getExecutableDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return {};
    fs::path p = fs::path(wideToUtf8(buf));
    return p.parent_path().string();
#else
    char buf[4096];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return fs::path(buf).parent_path().string();
#endif
}

std::string Paths::getReadOnlyAssetsDir()
{
    {
        std::string over = envOr("RETROCAPTURE_ASSETS_DIR", "");
        if (!over.empty() && fs::exists(over))
        {
            return over;
        }
    }

    // Dev tree / portable run a partir da raiz do projeto: `shaders/`,
    // `web/`, `ssl/` ficam direto em CWD. Probamos pelo subdir
    // `shaders/shaders_glsl`, que só existe no nosso layout de assets.
    {
        fs::path cwd = fs::current_path();
        if (fs::exists(cwd / "shaders" / "shaders_glsl"))
        {
            return cwd.string();
        }
    }

#ifdef _WIN32
    // Windows: cobrir os dois layouts:
    //
    //   - Portátil / dev (CMake copia em build): assets/, shaders/, web/,
    //     ssl/ ficam ao lado do .exe. getReadOnlyAssetsDir() retorna o
    //     próprio exeDir, e os callers compõem `<exeDir>/shaders/...`,
    //     `<exeDir>/web/...`.
    //   - Instalado via NSIS / CPack: o .exe vai em `<install>/bin/` e o
    //     resto em `<install>/share/retrocapture/{shaders,assets,web,ssl}`.
    //     A função então tem que retornar `<install>/share/retrocapture`.
    //
    // Sondamos pelo subdir `shaders/shaders_glsl` que existe nos dois
    // layouts, exatamente como na detecção de dev tree por CWD acima.
    fs::path exeDir(getExecutableDir());

    if (fs::exists(exeDir / "shaders" / "shaders_glsl"))
    {
        return exeDir.string();
    }

    fs::path installRoot   = exeDir.parent_path();             // <install>
    fs::path installShared = installRoot / "share" / "retrocapture";
    if (fs::exists(installShared / "shaders" / "shaders_glsl"))
    {
        return installShared.string();
    }

    {
        std::string pf = getShellFolder(CSIDL_PROGRAM_FILES);
        if (!pf.empty())
        {
            fs::path candidate = fs::path(pf) / "RetroCapture" /
                                 "share" / "retrocapture";
            if (fs::exists(candidate / "shaders" / "shaders_glsl"))
            {
                return candidate.string();
            }
        }
    }

    // Último recurso: assume layout NSIS relativo ao .exe mesmo que os
    // arquivos não estejam lá ainda — alguns callers gravam por baixo.
    return installShared.string();
#else
    // Linux: $XDG_DATA_DIRS é uma lista separada por ':'. Procura por
    // retrocapture/ em cada uma. Fallback /usr/local/share, /usr/share,
    // <exe>/../share/retrocapture, <exe>/assets.
    std::string xdgDirs = envOr("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    std::stringstream ss(xdgDirs);
    std::string entry;
    while (std::getline(ss, entry, ':'))
    {
        if (entry.empty()) continue;
        fs::path candidate = fs::path(entry) / "retrocapture";
        if (fs::exists(candidate)) return candidate.string();
    }

    fs::path exeDir(getExecutableDir());
    fs::path siblingShare = exeDir.parent_path() / "share" / "retrocapture";
    if (fs::exists(siblingShare)) return siblingShare.string();

    fs::path portable = exeDir / "assets";
    if (fs::exists(portable)) return portable.string();

    return portable.string(); // último recurso
#endif
}

std::string Paths::getUserConfigDir()
{
    {
        std::string over = envOr("RETROCAPTURE_CONFIG_DIR", "");
        if (!over.empty())
        {
            ensureDir(over);
            return over;
        }
    }

#ifdef _WIN32
    auto roaming = getShellFolder(CSIDL_APPDATA);
    if (roaming.empty())
    {
        roaming = envOr("APPDATA", "");
    }
    if (roaming.empty()) return {};
    fs::path dir = fs::path(roaming) / "RetroCapture";
    ensureDir(dir.string());
    return dir.string();
#else
    std::string home = getHomeDir();
    std::string base = envOr("XDG_CONFIG_HOME", home.empty() ? "" : home + "/.config");
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / "retrocapture";
    ensureDir(dir.string());
    return dir.string();
#endif
}

std::string Paths::getUserDataDir()
{
    {
        std::string over = envOr("RETROCAPTURE_DATA_DIR", "");
        if (!over.empty())
        {
            ensureDir(over);
            return over;
        }
    }

#ifdef _WIN32
    auto roaming = getShellFolder(CSIDL_APPDATA);
    if (roaming.empty())
    {
        roaming = envOr("APPDATA", "");
    }
    if (roaming.empty()) return {};
    fs::path dir = fs::path(roaming) / "RetroCapture" / "data";
    ensureDir(dir.string());
    return dir.string();
#else
    std::string home = getHomeDir();
    std::string base = envOr("XDG_DATA_HOME", home.empty() ? "" : home + "/.local/share");
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / "retrocapture";
    ensureDir(dir.string());
    return dir.string();
#endif
}

std::string Paths::getCacheDir()
{
    {
        std::string over = envOr("RETROCAPTURE_CACHE_DIR", "");
        if (!over.empty())
        {
            ensureDir(over);
            return over;
        }
    }

#ifdef _WIN32
    auto local = getShellFolder(CSIDL_LOCAL_APPDATA);
    if (local.empty())
    {
        local = envOr("LOCALAPPDATA", "");
    }
    if (local.empty()) return {};
    fs::path dir = fs::path(local) / "RetroCapture" / "Cache";
    ensureDir(dir.string());
    return dir.string();
#else
    std::string home = getHomeDir();
    std::string base = envOr("XDG_CACHE_HOME", home.empty() ? "" : home + "/.cache");
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / "retrocapture";
    ensureDir(dir.string());
    return dir.string();
#endif
}

bool Paths::migrateLegacyDataIfNeeded()
{
    // Idempotente: se MIGRATED.txt já existe no destino, não toca.
    std::string dataDir = getUserDataDir();
    if (dataDir.empty()) return false;

    fs::path marker = fs::path(dataDir) / "MIGRATED.txt";
    if (fs::exists(marker))
    {
        return false;
    }

    // Local legacy: a antiga `assets/` aninhada dentro do config dir.
    // Linux: ~/.config/retrocapture/assets. Windows: %APPDATA%\RetroCapture\assets.
    // (`getUserConfigDir()` retorna a raiz, então legacy vive em raiz/assets.)
    std::string configDir = getUserConfigDir();
    if (configDir.empty()) return false;

    fs::path legacyAssets = fs::path(configDir) / "assets";
    fs::path legacySSL = fs::path(configDir) / "ssl";

    bool movedAnything = false;

    auto moveTree = [&](const fs::path &from, const fs::path &to)
    {
        if (!fs::exists(from) || !fs::is_directory(from)) return;
        try
        {
            fs::create_directories(to);
            // FilesystemCompat (MinGW antigo) não permite range-for em
            // directory_iterator — usamos iterador explícito + helpers.
            fs::directory_iterator it(from);
            fs::directory_iterator end;
            for (; it != end; ++it)
            {
                fs::path src = fs_helper::get_path(it);
                fs::path dst = to / src.filename();
                if (fs::exists(dst))
                {
                    LOG_WARN("Paths migration: skipping " + src.string() +
                             " — destination " + dst.string() + " already exists");
                    continue;
                }
                fs::rename(src, dst);
                LOG_INFO("Paths migration: moved " + src.string() + " -> " + dst.string());
                movedAnything = true;
            }
        }
        catch (const std::exception &e)
        {
            LOG_WARN(std::string("Paths migration: ") + e.what());
        }
    };

    // Conteúdo do antigo `assets/` (presets, thumbnails) é a raiz da nova
    // user-data dir. Mantemos os subdirs (presets/, thumbnails/) como estão.
    moveTree(legacyAssets, fs::path(dataDir));
    // SSL legacy era irmão de assets — vai pra user-data/ssl.
    moveTree(legacySSL, fs::path(dataDir) / "ssl");

    // Marca como migrado mesmo se nada foi movido (próxima execução pula).
    try
    {
        std::ofstream(marker.string()) << "Migration completed.\n"
                                       << "Moved legacy content from " << configDir << "/assets and /ssl\n"
                                       << "to " << dataDir << ".\n";
    }
    catch (...)
    {
        // sem marker, vamos retentar na próxima execução — não é crítico.
    }

    return movedAnything;
}

std::string Paths::getDefaultRecordingsDir()
{
#ifdef _WIN32
    auto videos = getShellFolder(CSIDL_MYVIDEO);
    if (videos.empty())
    {
        auto profile = envOr("USERPROFILE", "");
        if (!profile.empty()) videos = (fs::path(profile) / "Videos").string();
    }
    if (videos.empty()) return {};
    fs::path dir = fs::path(videos) / "RetroCapture";
    ensureDir(dir.string());
    return dir.string();
#else
    std::string home = getHomeDir();
    if (home.empty()) return {};
    std::string videos = readXdgVideosDir(home);
    if (videos.empty()) videos = home + "/Videos";
    fs::path dir = fs::path(videos) / "RetroCapture";
    ensureDir(dir.string());
    return dir.string();
#endif
}
