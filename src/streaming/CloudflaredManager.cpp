#include "CloudflaredManager.h"

#include "CloudflaredDownloader.h"
#include "../utils/FilesystemCompat.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#include <cstdio>
#include <cstring>
#include <regex>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <cerrno>
#endif

namespace
{
    // Cloudflared's `tunnel run` reads ingress + credentials from a
    // YAML config file. We generate one per-run inside the user-data
    // cloudflared cache so each tunnel id has its own minimal config
    // pointing at the local stream port. The user's ~/.cloudflared
    // credentials JSON (written by `cloudflared tunnel login`) is
    // referenced by absolute path so we don't duplicate it.
    std::string writeNamedTunnelConfig(const std::string &tunnelId,
                                       uint16_t           localPort)
    {
        fs::path dir = fs::path(Paths::getUserDataDir()) / "cloudflared";
        try { fs::create_directories(dir); } catch (...) {}
        fs::path cfgPath = dir / (tunnelId + ".yml");

        // The credentials file is at ~/.cloudflared/<id>.json on both
        // platforms after `cloudflared tunnel login` + create. We
        // hand its path verbatim — quote it to survive spaces in the
        // user's home directory (typical on Windows).
        std::string credentialsPath;
#ifdef _WIN32
        const char *userProfile = ::getenv("USERPROFILE");
        if (userProfile) credentialsPath = std::string(userProfile) + "\\.cloudflared\\" + tunnelId + ".json";
#else
        const char *home = ::getenv("HOME");
        if (home) credentialsPath = std::string(home) + "/.cloudflared/" + tunnelId + ".json";
#endif

        std::string yaml;
        yaml += "tunnel: " + tunnelId + "\n";
        yaml += "credentials-file: \"" + credentialsPath + "\"\n";
        yaml += "ingress:\n";
        yaml += "  - service: http://localhost:" + std::to_string(localPort) + "\n";

        FILE *fp = ::fopen(cfgPath.string().c_str(), "wb");
        if (!fp) return {};
        ::fwrite(yaml.data(), 1, yaml.size(), fp);
        ::fclose(fp);
        return cfgPath.string();
    }
}

namespace
{
    // Cloudflare quick-tunnel announces itself with a line of the form
    //     Your quick tunnel has been created! Visit it at: https://abc-words.trycloudflare.com
    // We pick out the first https://*.trycloudflare.com we see in
    // stdout — both the formal announcement line and the slightly
    // different formatting Cloudflare has used across versions match.
    const std::regex &tunnelUrlRegex()
    {
        static const std::regex re(R"((https://[a-z0-9-]+\.trycloudflare\.com))",
                                   std::regex::optimize | std::regex::icase);
        return re;
    }
}

CloudflaredManager::CloudflaredManager() = default;

CloudflaredManager::~CloudflaredManager()
{
    stop();
}

std::string CloudflaredManager::getUrl() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_url;
}

std::string CloudflaredManager::getLastError() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_lastError;
}

bool CloudflaredManager::start(uint16_t localPort)
{
    // Backwards-compat wrapper for the original Quick-only call site.
    TunnelConfig cfg;
    cfg.mode      = Mode::Quick;
    cfg.localPort = localPort;
    return start(cfg);
}

bool CloudflaredManager::start(const TunnelConfig &cfg)
{
    if (m_state.load() != State::Idle)
    {
        // Already running; caller should stop() first to reconfigure.
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        // For Named mode the public URL is known up front — pre-fill
        // it so getUrl() works before cloudflared has even printed a
        // line. The state stays Spawning until the child actually
        // launches; the regex parse in readerLoop is a no-op in this
        // mode since trycloudflare.com never appears in named-tunnel
        // logs.
        m_url = (cfg.mode == Mode::Named) ? cfg.publicUrl : std::string();
        m_lastError.clear();
    }
    m_stopRequested.store(false);

    const std::string portStr  = std::to_string(cfg.localPort);
    const std::string localUrl = "http://localhost:" + portStr;

    // Named mode needs a per-tunnel YAML at <user-data>/cloudflared/<id>.yml
    // that points the tunnel id at our local stream port. Quick mode
    // doesn't use --config at all (the --url flag carries that info).
    std::string namedConfigPath;
    if (cfg.mode == Mode::Named)
    {
        if (cfg.tunnelId.empty())
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_lastError = "Named tunnel requested but tunnel id is empty";
            m_state.store(State::NotFound);
            return false;
        }
        namedConfigPath = writeNamedTunnelConfig(cfg.tunnelId, cfg.localPort);
        if (namedConfigPath.empty())
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_lastError = "Failed to write cloudflared config for tunnel " + cfg.tunnelId;
            m_state.store(State::NotFound);
            return false;
        }
    }

    // Resolve which cloudflared binary to invoke. Falls back through
    // CLI override → user-data-dir cache → system PATH; empty means
    // no binary at all and the UI is expected to offer the download
    // flow. Caching the path locally keeps the rest of this function
    // identical between platforms.
    const std::string binaryPath = CloudflaredDownloader::resolveBinaryPath();
    if (binaryPath.empty())
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastError = "cloudflared not available — accept the download "
                      "in the Streaming tab or pass --cloudflared-binary";
        m_state.store(State::NotFound);
        LOG_WARN("CloudflaredManager: no cloudflared binary resolved");
        return false;
    }
    LOG_INFO("CloudflaredManager: using binary at " + binaryPath);

#ifdef _WIN32
    // Pipe for stdout. HANDLE_FLAG_INHERIT so the child inherits the
    // write end (and only that one — the read end stays parent-only).
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd  = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastError = "CreatePipe failed";
        m_state.store(State::NotFound);
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    PROCESS_INFORMATION pi{};
    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.hStdOutput = writeEnd;
    si.hStdError  = writeEnd;
    si.dwFlags    = STARTF_USESTDHANDLES;

    // CreateProcess wants a mutable cmdline buffer. argv[0] is
    // double-quoted so a binaryPath with spaces (typical on Windows:
    // C:\Users\<name>\AppData\Roaming\RetroCapture\…) parses
    // correctly. cmdline shape diverges by mode:
    //   Quick:  <bin> tunnel --url http://localhost:<port>
    //   Named:  <bin> tunnel run --config "<path>.yml" <tunnelId>
    std::string cmd;
    if (cfg.mode == Mode::Named)
    {
        cmd = "\"" + binaryPath + "\" tunnel run --config \"" +
              namedConfigPath + "\" " + cfg.tunnelId;
    }
    else
    {
        cmd = "\"" + binaryPath + "\" tunnel --url " + localUrl;
    }
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(
        binaryPath.c_str(),       // explicit image path; no PATH lookup needed
        mutableCmd.data(),
        nullptr, nullptr,
        TRUE,                     // inherit handles
        CREATE_NO_WINDOW,         // don't pop up a console
        nullptr, nullptr,
        &si, &pi);

    CloseHandle(writeEnd); // parent never writes
    if (!ok)
    {
        CloseHandle(readEnd);
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastError = "Failed to launch cloudflared at " + binaryPath +
                      " (Win32 error " + std::to_string(GetLastError()) + ")";
        m_state.store(State::NotFound);
        return false;
    }
    CloseHandle(pi.hThread);

    m_processHandle = pi.hProcess;
    m_stdoutHandle  = readEnd;
#else
    // Pipe + fork + execvp. The parent keeps the read end; the child
    // dup2s the write end onto fd 1 (and 2).
    int pipeFds[2];
    if (pipe(pipeFds) < 0)
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastError = std::string("pipe() failed: ") + std::strerror(errno);
        m_state.store(State::NotFound);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastError = std::string("fork() failed: ") + std::strerror(errno);
        m_state.store(State::NotFound);
        return false;
    }
    if (pid == 0)
    {
        // Child: redirect stdout + stderr to the pipe's write end,
        // then exec cloudflared. If execvp fails we exit with a code
        // the parent can detect (waitpid path).
        ::close(pipeFds[0]);
        if (pipeFds[1] != STDOUT_FILENO)
        {
            ::dup2(pipeFds[1], STDOUT_FILENO);
        }
        ::dup2(pipeFds[1], STDERR_FILENO);
        if (pipeFds[1] != STDOUT_FILENO && pipeFds[1] != STDERR_FILENO)
        {
            ::close(pipeFds[1]);
        }

        // exec by absolute path — binaryPath came from
        // resolveBinaryPath() which already knows where the binary
        // is (cache, CLI override, or a PATH lookup we did
        // ourselves). argv shape diverges by mode:
        //   Quick:  <bin> tunnel --url http://localhost:<port>
        //   Named:  <bin> tunnel run --config <path>.yml <tunnelId>
        std::string p0 = binaryPath;
        std::string p1 = "tunnel";
        if (cfg.mode == Mode::Named)
        {
            std::string p2 = "run";
            std::string p3 = "--config";
            std::string p4 = namedConfigPath;
            std::string p5 = cfg.tunnelId;
            char *argv[] = { p0.data(), p1.data(), p2.data(),
                             p3.data(), p4.data(), p5.data(), nullptr };
            ::execv(binaryPath.c_str(), argv);
        }
        else
        {
            std::string p2 = "--url";
            std::string p3 = localUrl;
            char *argv[] = { p0.data(), p1.data(), p2.data(), p3.data(), nullptr };
            ::execv(binaryPath.c_str(), argv);
        }
        // If we got here, execv failed. _exit so destructors don't run.
        _exit(127);
    }

    // Parent
    ::close(pipeFds[1]);
    m_pid      = pid;
    m_stdoutFd = pipeFds[0];
#endif

    // Quick mode waits on stdout for the trycloudflare URL → state
    // stays Spawning until that line lands. Named mode already has
    // the URL (caller pre-set it from the configured hostname), so
    // it's effectively Active the moment the child launches —
    // readerLoop still runs to detect crashes but it has nothing to
    // parse.
    if (cfg.mode == Mode::Named)
    {
        m_state.store(State::Active);
    }
    else
    {
        m_state.store(State::Spawning);
    }
    m_thread = std::thread([this] { readerLoop(); });
    LOG_INFO(std::string("CloudflaredManager: started cloudflared (") +
             (cfg.mode == Mode::Named ? "named, " + cfg.tunnelId : "quick") +
             ") for " + localUrl);
    return true;
}

void CloudflaredManager::stop()
{
    State s = m_state.load();
    if (s == State::Idle && !m_thread.joinable()) return;

    m_stopRequested.store(true);
    m_state.store(State::Stopping);
    terminateChild();

    if (m_thread.joinable()) m_thread.join();

#ifdef _WIN32
    if (m_processHandle) { CloseHandle(static_cast<HANDLE>(m_processHandle)); m_processHandle = nullptr; }
    if (m_stdoutHandle)  { CloseHandle(static_cast<HANDLE>(m_stdoutHandle));  m_stdoutHandle  = nullptr; }
#else
    if (m_stdoutFd >= 0) { ::close(m_stdoutFd); m_stdoutFd = -1; }
    if (m_pid > 0)
    {
        // Reap zombie if the SIGTERM landed.
        int status = 0;
        ::waitpid(m_pid, &status, WNOHANG);
        m_pid = -1;
    }
#endif

    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_url.clear();
    }
    m_state.store(State::Idle);
}

void CloudflaredManager::terminateChild()
{
#ifdef _WIN32
    if (m_processHandle)
    {
        TerminateProcess(static_cast<HANDLE>(m_processHandle), 0);
        WaitForSingleObject(static_cast<HANDLE>(m_processHandle), 2000);
    }
#else
    if (m_pid > 0)
    {
        ::kill(m_pid, SIGTERM);
        // Give it ~2 s to exit gracefully; SIGKILL if it doesn't.
        for (int i = 0; i < 20; ++i)
        {
            int status = 0;
            pid_t r = ::waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) return;
            struct timespec ts{0, 100'000'000}; // 100 ms
            ::nanosleep(&ts, nullptr);
        }
        ::kill(m_pid, SIGKILL);
        ::waitpid(m_pid, nullptr, 0);
    }
#endif
}

void CloudflaredManager::readerLoop()
{
    std::string lineBuf;
    lineBuf.reserve(512);

    char chunk[1024];
    while (!m_stopRequested.load())
    {
#ifdef _WIN32
        DWORD got = 0;
        // ReadFile blocks until at least one byte or pipe closes.
        if (!ReadFile(static_cast<HANDLE>(m_stdoutHandle), chunk, sizeof(chunk), &got, nullptr))
        {
            break;
        }
        if (got == 0) break;
        const size_t n = got;
#else
        ssize_t got = ::read(m_stdoutFd, chunk, sizeof(chunk));
        if (got <= 0)
        {
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        const size_t n = static_cast<size_t>(got);
#endif

        for (size_t i = 0; i < n; ++i)
        {
            char c = chunk[i];
            if (c == '\n')
            {
                // Try to extract a tunnel URL from this line.
                std::smatch m;
                if (std::regex_search(lineBuf, m, tunnelUrlRegex()))
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    m_url = m[1].str();
                    m_state.store(State::Active);
                    LOG_INFO("CloudflaredManager: tunnel URL = " + m_url);
                }
                lineBuf.clear();
            }
            else if (c != '\r')
            {
                lineBuf.push_back(c);
                if (lineBuf.size() > 4096) lineBuf.clear(); // sanity cap
            }
        }
    }

    // Reader exited — either we asked it to, or cloudflared died.
    if (!m_stopRequested.load())
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_url.empty() && m_lastError.empty())
        {
            m_lastError = "cloudflared exited before producing a URL";
        }
        m_state.store(State::Crashed);
        LOG_WARN(std::string("CloudflaredManager: child exited unexpectedly — ") + m_lastError);
    }
}
