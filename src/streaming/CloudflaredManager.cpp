#include "CloudflaredManager.h"

#include "CloudflaredDownloader.h"
#include "../utils/Logger.h"

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
    if (m_state.load() != State::Idle)
    {
        // Already running; caller should stop() first to reconfigure.
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_url.clear();
        m_lastError.clear();
    }
    m_stopRequested.store(false);

    const std::string portStr = std::to_string(localPort);
    const std::string localUrl = "http://localhost:" + portStr;

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
    // correctly.
    std::string cmd = "\"" + binaryPath + "\" tunnel --url " + localUrl;
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
        // resolveBinaryPath() which already knows where the binary is
        // (cache, CLI override, or a PATH lookup we did ourselves), so
        // there's no reason to ask the kernel to search again. argv[0]
        // by convention is the binary's name; using the full path also
        // works and matches what shell-launched processes see.
        std::string p0 = binaryPath;
        std::string p1 = "tunnel";
        std::string p2 = "--url";
        std::string p3 = localUrl;
        char *argv[] = { p0.data(), p1.data(), p2.data(), p3.data(), nullptr };
        ::execv(binaryPath.c_str(), argv);
        // If we got here, execv failed. _exit so destructors don't run.
        _exit(127);
    }

    // Parent
    ::close(pipeFds[1]);
    m_pid      = pid;
    m_stdoutFd = pipeFds[0];
#endif

    m_state.store(State::Spawning);
    m_thread = std::thread([this] { readerLoop(); });
    LOG_INFO("CloudflaredManager: started cloudflared for " + localUrl);
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
