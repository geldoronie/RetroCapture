#include "CloudflaredAccount.h"

#include "CloudflaredDownloader.h"
#include "../utils/FilesystemCompat.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace
{
    using json = nlohmann::json;

    // ──────────────────────────────────────────────────────────────
    // Path of the credentials file produced by `cloudflared tunnel login`.
    // ──────────────────────────────────────────────────────────────
    fs::path credentialsPath()
    {
#ifdef _WIN32
        const char *userProfile = ::getenv("USERPROFILE");
        if (!userProfile) return {};
        return fs::path(userProfile) / ".cloudflared" / "cert.pem";
#else
        const char *home = ::getenv("HOME");
        if (!home) return {};
        return fs::path(home) / ".cloudflared" / "cert.pem";
#endif
    }

    // ──────────────────────────────────────────────────────────────
    // Synchronous subprocess wrapper.
    //
    // Spawns cloudflared with the given args, captures stdout into
    // `stdoutOut`, returns the exit code. Hard timeout (seconds);
    // on expiry the child is force-killed and the function returns
    // exit code -1 with `stdoutOut` reflecting whatever made it
    // through the pipe before the kill.
    //
    // This is *deliberately* not factored into a generic util — the
    // pattern is the same as CloudflaredManager's spawn but with
    // sync/wait semantics. Reusing the manager's machinery would mean
    // dragging its supervision thread into territory it wasn't
    // designed for; a 100-line duplicated helper keeps lifetimes
    // obvious.
    // ──────────────────────────────────────────────────────────────
    int runCloudflared(const std::vector<std::string> &args,
                       std::string                    &stdoutOut,
                       std::string                    &errorOut,
                       int                             timeoutSec)
    {
        stdoutOut.clear();
        errorOut.clear();

        const std::string binaryPath = CloudflaredDownloader::resolveBinaryPath();
        if (binaryPath.empty())
        {
            errorOut = "cloudflared binary not available";
            return -2;
        }

#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readEnd  = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
        {
            errorOut = "CreatePipe failed";
            return -1;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        std::string cmd = "\"" + binaryPath + "\"";
        for (const auto &a : args) cmd += " \"" + a + "\"";
        std::vector<char> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back('\0');

        PROCESS_INFORMATION pi{};
        STARTUPINFOA       si{};
        si.cb         = sizeof(si);
        si.hStdOutput = writeEnd;
        si.hStdError  = writeEnd;
        si.dwFlags    = STARTF_USESTDHANDLES;

        BOOL ok = CreateProcessA(
            binaryPath.c_str(), mutableCmd.data(),
            nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (!ok)
        {
            CloseHandle(readEnd);
            errorOut = "CreateProcess failed (Win32 error " +
                       std::to_string(GetLastError()) + ")";
            return -1;
        }
        CloseHandle(pi.hThread);

        // Drain stdout until pipe closes or timeout.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(timeoutSec);
        std::vector<char> buf(4096);
        for (;;)
        {
            DWORD bytesAvail = 0;
            if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &bytesAvail, nullptr) && bytesAvail > 0)
            {
                DWORD got = 0;
                if (ReadFile(readEnd, buf.data(),
                             static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
                {
                    stdoutOut.append(buf.data(), got);
                    continue;
                }
            }
            DWORD waitRes = WaitForSingleObject(pi.hProcess, 50);
            if (waitRes == WAIT_OBJECT_0) break;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 500);
                CloseHandle(pi.hProcess);
                CloseHandle(readEnd);
                errorOut = "cloudflared timed out after " +
                           std::to_string(timeoutSec) + " s";
                return -1;
            }
        }
        // Drain the final tail of stdout.
        DWORD bytesAvail = 0;
        while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &bytesAvail, nullptr) && bytesAvail > 0)
        {
            DWORD got = 0;
            if (!ReadFile(readEnd, buf.data(),
                          static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) break;
            stdoutOut.append(buf.data(), got);
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(readEnd);
        return static_cast<int>(exitCode);
#else
        int pipeFds[2];
        if (pipe(pipeFds) < 0)
        {
            errorOut = std::string("pipe() failed: ") + std::strerror(errno);
            return -1;
        }
        pid_t pid = fork();
        if (pid < 0)
        {
            ::close(pipeFds[0]); ::close(pipeFds[1]);
            errorOut = std::string("fork() failed: ") + std::strerror(errno);
            return -1;
        }
        if (pid == 0)
        {
            ::close(pipeFds[0]);
            ::dup2(pipeFds[1], STDOUT_FILENO);
            ::dup2(pipeFds[1], STDERR_FILENO);
            if (pipeFds[1] > STDERR_FILENO) ::close(pipeFds[1]);

            std::vector<std::string> ownedArgs;
            ownedArgs.reserve(args.size() + 1);
            ownedArgs.push_back(binaryPath);
            for (const auto &a : args) ownedArgs.push_back(a);
            std::vector<char *> argv;
            argv.reserve(ownedArgs.size() + 1);
            for (auto &s : ownedArgs) argv.push_back(s.data());
            argv.push_back(nullptr);
            ::execv(binaryPath.c_str(), argv.data());
            _exit(127);
        }

        // Parent. Make the read end non-blocking so we can poll with
        // a deadline.
        ::close(pipeFds[1]);
        int flags = ::fcntl(pipeFds[0], F_GETFL, 0);
        ::fcntl(pipeFds[0], F_SETFL, flags | O_NONBLOCK);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(timeoutSec);
        std::vector<char> buf(4096);
        for (;;)
        {
            ssize_t n = ::read(pipeFds[0], buf.data(), buf.size());
            if (n > 0)
            {
                stdoutOut.append(buf.data(), static_cast<size_t>(n));
                continue;
            }
            if (n == 0) break; // pipe closed
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                int    status = 0;
                pid_t  w      = ::waitpid(pid, &status, WNOHANG);
                if (w == pid)
                {
                    // Drain any final bytes.
                    while ((n = ::read(pipeFds[0], buf.data(), buf.size())) > 0)
                    {
                        stdoutOut.append(buf.data(), static_cast<size_t>(n));
                    }
                    ::close(pipeFds[0]);
                    if (WIFEXITED(status))   return WEXITSTATUS(status);
                    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
                    return -1;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    ::kill(pid, SIGTERM);
                    ::waitpid(pid, nullptr, 0);
                    ::close(pipeFds[0]);
                    errorOut = "cloudflared timed out after " +
                               std::to_string(timeoutSec) + " s";
                    return -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (errno == EINTR) continue;
            break;
        }
        int   status = 0;
        ::waitpid(pid, &status, 0);
        ::close(pipeFds[0]);
        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
#endif
    }

    // ──────────────────────────────────────────────────────────────
    // Login state (singleton; only one login at a time).
    // ──────────────────────────────────────────────────────────────
    std::mutex         g_loginMu;
    std::atomic<bool>  g_loginInFlight{false};
    std::atomic<bool>  g_loginCancelRequested{false};
#ifdef _WIN32
    HANDLE g_loginProcess = nullptr;
#else
    pid_t  g_loginPid     = -1;
#endif
}

namespace CloudflaredAccount
{
    bool hasCredentials()
    {
        fs::path p = credentialsPath();
        if (p.string().empty()) return false;
        try { return fs::exists(p); } catch (...) { return false; }
    }

    bool isLoginInFlight()
    {
        return g_loginInFlight.load();
    }

    // ──────────────────────────────────────────────────────────────
    // Async login.
    //
    // Flow:
    //  1. Spawn `cloudflared tunnel login`, redirect stdout+stderr
    //     to a pipe.
    //  2. Reader loop scans for "https://..." in stdout. The first
    //     match becomes LoginProgress::oauthUrl; cb fires with
    //     AwaitingAuth so the UI can show "open this URL".
    //  3. In parallel, poll for cert.pem appearing on disk. The
    //     daemon writes it once the user has clicked through the
    //     OAuth dance. When seen, terminate the child cleanly, fire
    //     cb with Complete.
    //  4. If cancelLogin() is called, kill the child and fire cb
    //     with Cancelled.
    // ──────────────────────────────────────────────────────────────
    bool beginLoginAsync(LoginCallback cb)
    {
        bool expected = false;
        if (!g_loginInFlight.compare_exchange_strong(expected, true))
        {
            return false;
        }
        g_loginCancelRequested.store(false);

        std::thread([cb = std::move(cb)]() mutable {
            auto fire = [&cb](LoginProgress::Stage s,
                              const std::string  &oauthUrl,
                              const std::string  &error = {})
            {
                if (!cb) return;
                LoginProgress pr;
                pr.stage    = s;
                pr.oauthUrl = oauthUrl;
                pr.error    = error;
                cb(pr);
            };

            fire(LoginProgress::Stage::Starting, "");

            const std::string binaryPath = CloudflaredDownloader::resolveBinaryPath();
            if (binaryPath.empty())
            {
                fire(LoginProgress::Stage::Failed, "", "cloudflared binary not available");
                g_loginInFlight.store(false);
                return;
            }

            // ── spawn ───────────────────────────────────────────
#ifdef _WIN32
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
            {
                fire(LoginProgress::Stage::Failed, "", "CreatePipe failed");
                g_loginInFlight.store(false);
                return;
            }
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

            std::string cmd = "\"" + binaryPath + "\" tunnel login";
            std::vector<char> mutableCmd(cmd.begin(), cmd.end());
            mutableCmd.push_back('\0');

            PROCESS_INFORMATION pi{};
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.hStdOutput = writeEnd;
            si.hStdError  = writeEnd;
            si.dwFlags    = STARTF_USESTDHANDLES;

            BOOL ok = CreateProcessA(binaryPath.c_str(), mutableCmd.data(),
                                     nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
            CloseHandle(writeEnd);
            if (!ok)
            {
                CloseHandle(readEnd);
                fire(LoginProgress::Stage::Failed, "",
                     "CreateProcess failed (Win32 error " +
                         std::to_string(GetLastError()) + ")");
                g_loginInFlight.store(false);
                return;
            }
            CloseHandle(pi.hThread);
            {
                std::lock_guard<std::mutex> lock(g_loginMu);
                g_loginProcess = pi.hProcess;
            }
            HANDLE childHandle = pi.hProcess;
            HANDLE readHandle  = readEnd;
#else
            int pipeFds[2];
            if (pipe(pipeFds) < 0)
            {
                fire(LoginProgress::Stage::Failed, "",
                     std::string("pipe() failed: ") + std::strerror(errno));
                g_loginInFlight.store(false);
                return;
            }
            pid_t childPid = fork();
            if (childPid < 0)
            {
                ::close(pipeFds[0]); ::close(pipeFds[1]);
                fire(LoginProgress::Stage::Failed, "",
                     std::string("fork() failed: ") + std::strerror(errno));
                g_loginInFlight.store(false);
                return;
            }
            if (childPid == 0)
            {
                ::close(pipeFds[0]);
                ::dup2(pipeFds[1], STDOUT_FILENO);
                ::dup2(pipeFds[1], STDERR_FILENO);
                if (pipeFds[1] > STDERR_FILENO) ::close(pipeFds[1]);
                std::string p0 = binaryPath, p1 = "tunnel", p2 = "login";
                char *argv[] = { p0.data(), p1.data(), p2.data(), nullptr };
                ::execv(binaryPath.c_str(), argv);
                _exit(127);
            }
            ::close(pipeFds[1]);
            int flags = ::fcntl(pipeFds[0], F_GETFL, 0);
            ::fcntl(pipeFds[0], F_SETFL, flags | O_NONBLOCK);
            {
                std::lock_guard<std::mutex> lock(g_loginMu);
                g_loginPid = childPid;
            }
            int readHandle = pipeFds[0];
#endif

            // ── reader + cert.pem poll loop ─────────────────────
            std::string lineBuf;
            std::string capturedUrl;
            bool        sentAwaitingAuth = false;
            const auto  loginDeadline = std::chrono::steady_clock::now() +
                                        std::chrono::minutes(15);

            // Cloudflared prints something like:
            //   Please open the following URL and log in with your Cloudflare account:
            //   https://dash.cloudflare.com/argotunnel?...
            // Pick the first https:// we see.
            static const std::regex urlRegex(R"((https://[^\s]+))");

            while (!g_loginCancelRequested.load())
            {
                // Did the user finish OAuth? Check disk first.
                if (hasCredentials())
                {
                    LOG_INFO("CloudflaredAccount: cert.pem detected — login complete");
                    break;
                }

                // Read whatever's in the pipe.
                std::vector<char> buf(4096);
#ifdef _WIN32
                DWORD bytesAvail = 0;
                if (PeekNamedPipe(readHandle, nullptr, 0, nullptr, &bytesAvail, nullptr) && bytesAvail > 0)
                {
                    DWORD got = 0;
                    if (ReadFile(readHandle, buf.data(),
                                 static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
                    {
                        for (DWORD i = 0; i < got; ++i)
                        {
                            char c = buf[i];
                            if (c == '\n')
                            {
                                std::smatch m;
                                if (capturedUrl.empty() &&
                                    std::regex_search(lineBuf, m, urlRegex))
                                {
                                    capturedUrl = m[1].str();
                                    LOG_INFO("CloudflaredAccount: OAuth URL = " + capturedUrl);
                                }
                                lineBuf.clear();
                            }
                            else if (c != '\r')
                            {
                                lineBuf.push_back(c);
                                if (lineBuf.size() > 4096) lineBuf.clear();
                            }
                        }
                    }
                }
#else
                ssize_t n = ::read(readHandle, buf.data(), buf.size());
                if (n > 0)
                {
                    for (ssize_t i = 0; i < n; ++i)
                    {
                        char c = buf[i];
                        if (c == '\n')
                        {
                            std::smatch m;
                            if (capturedUrl.empty() &&
                                std::regex_search(lineBuf, m, urlRegex))
                            {
                                capturedUrl = m[1].str();
                                LOG_INFO("CloudflaredAccount: OAuth URL = " + capturedUrl);
                            }
                            lineBuf.clear();
                        }
                        else if (c != '\r')
                        {
                            lineBuf.push_back(c);
                            if (lineBuf.size() > 4096) lineBuf.clear();
                        }
                    }
                }
#endif

                if (!capturedUrl.empty() && !sentAwaitingAuth)
                {
                    fire(LoginProgress::Stage::AwaitingAuth, capturedUrl);
                    sentAwaitingAuth = true;
                }

                if (std::chrono::steady_clock::now() >= loginDeadline)
                {
                    LOG_WARN("CloudflaredAccount: login timed out");
                    fire(LoginProgress::Stage::Failed, capturedUrl,
                         "Login timed out — try again");
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // ── terminate child + cleanup ───────────────────────
            const bool wasCancelled = g_loginCancelRequested.load();
            const bool didComplete  = hasCredentials();

#ifdef _WIN32
            if (childHandle)
            {
                TerminateProcess(childHandle, 0);
                WaitForSingleObject(childHandle, 1000);
                CloseHandle(childHandle);
            }
            CloseHandle(readHandle);
            {
                std::lock_guard<std::mutex> lock(g_loginMu);
                g_loginProcess = nullptr;
            }
#else
            if (childPid > 0)
            {
                ::kill(childPid, SIGTERM);
                ::waitpid(childPid, nullptr, 0);
            }
            ::close(readHandle);
            {
                std::lock_guard<std::mutex> lock(g_loginMu);
                g_loginPid = -1;
            }
#endif

            if (wasCancelled)
            {
                fire(LoginProgress::Stage::Cancelled, capturedUrl);
            }
            else if (didComplete)
            {
                fire(LoginProgress::Stage::Complete, capturedUrl);
            }
            // else: Failed was already fired above on timeout.
            g_loginInFlight.store(false);
        }).detach();

        return true;
    }

    void cancelLogin()
    {
        if (!g_loginInFlight.load()) return;
        g_loginCancelRequested.store(true);
        // The worker thread observes the flag on its next iteration
        // and tears the child down itself.
    }

    // ──────────────────────────────────────────────────────────────
    // Sync calls.
    // ──────────────────────────────────────────────────────────────
    std::vector<TunnelInfo> listTunnels(std::string &errorOut)
    {
        std::vector<TunnelInfo> out;
        std::string stdoutBuf;
        int rc = runCloudflared({ "tunnel", "list", "--output", "json" },
                                stdoutBuf, errorOut, /*timeoutSec=*/15);
        if (rc != 0)
        {
            if (errorOut.empty())
            {
                errorOut = "cloudflared tunnel list failed (exit " +
                           std::to_string(rc) + ")";
                // Include the first line of stdout — cloudflared often
                // puts a useful "you must login first" message there.
                size_t nl = stdoutBuf.find('\n');
                if (nl != std::string::npos && nl > 0)
                {
                    errorOut += ": " + stdoutBuf.substr(0, nl);
                }
            }
            return out;
        }
        try
        {
            auto j = json::parse(stdoutBuf);
            if (j.is_array())
            {
                for (const auto &t : j)
                {
                    TunnelInfo ti;
                    ti.id        = t.value("id",         std::string{});
                    ti.name      = t.value("name",       std::string{});
                    ti.createdAt = t.value("created_at", std::string{});
                    if (!ti.id.empty()) out.push_back(std::move(ti));
                }
            }
        }
        catch (const std::exception &ex)
        {
            errorOut = std::string("Failed to parse tunnel list JSON: ") + ex.what();
        }
        return out;
    }

    std::string createTunnel(const std::string &name, std::string &errorOut)
    {
        std::string stdoutBuf;
        int rc = runCloudflared({ "tunnel", "create", name },
                                stdoutBuf, errorOut, /*timeoutSec=*/30);
        if (rc != 0)
        {
            if (errorOut.empty())
            {
                errorOut = "cloudflared tunnel create failed (exit " +
                           std::to_string(rc) + "): " + stdoutBuf;
            }
            return {};
        }
        // cloudflared prints something like:
        //   Created tunnel <name> with id <uuid>
        static const std::regex idRegex(R"(id ([0-9a-fA-F-]{36}))");
        std::smatch m;
        if (std::regex_search(stdoutBuf, m, idRegex))
        {
            return m[1].str();
        }
        errorOut = "cloudflared returned no tunnel id: " + stdoutBuf;
        return {};
    }

    bool routeDns(const std::string &tunnelId,
                  const std::string &hostname,
                  std::string       &errorOut)
    {
        std::string stdoutBuf;
        int rc = runCloudflared({ "tunnel", "route", "dns", tunnelId, hostname },
                                stdoutBuf, errorOut, /*timeoutSec=*/30);
        if (rc != 0)
        {
            if (errorOut.empty())
            {
                errorOut = "cloudflared tunnel route dns failed (exit " +
                           std::to_string(rc) + "): " + stdoutBuf;
            }
            return false;
        }
        return true;
    }
}
