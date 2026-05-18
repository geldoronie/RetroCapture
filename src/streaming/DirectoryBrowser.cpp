#include "DirectoryBrowser.h"

#include "../utils/HttpClient.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    std::string trimSlash(std::string u)
    {
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
}

DirectoryBrowser::DirectoryBrowser() = default;

DirectoryBrowser::~DirectoryBrowser()
{
    stop();
}

bool DirectoryBrowser::start(const std::string &directoryUrl)
{
    if (directoryUrl.empty() || directoryUrl.rfind("http://", 0) != 0)
    {
        LOG_WARN("DirectoryBrowser::start — invalid URL: " + directoryUrl);
        return false;
    }

    if (m_running.load())
    {
        // Same URL → no-op. Different URL → restart with new target.
        if (m_directoryUrl == trimSlash(directoryUrl)) return true;
        stop();
    }

    m_directoryUrl = trimSlash(directoryUrl);
    m_stopRequested.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void DirectoryBrowser::refreshNow()
{
    if (!m_running.load()) return;
    std::lock_guard<std::mutex> lock(m_wakeMu);
    m_refreshRequested = true;
    m_wakeCv.notify_all();
}

void DirectoryBrowser::stop()
{
    if (!m_running.load() && !m_thread.joinable()) return;
    m_stopRequested.store(true);
    {
        std::lock_guard<std::mutex> lock(m_wakeMu);
        m_wakeCv.notify_all();
    }
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

DirectoryBrowser::Snapshot DirectoryBrowser::getSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_snapshotMu);
    return m_snapshot;
}

void DirectoryBrowser::workerLoop()
{
    // First fetch immediately so the UI doesn't show "Loading…" for
    // 30 s on initial open.
    {
        Snapshot s;
        if (fetchOnce(s))
        {
            std::lock_guard<std::mutex> lock(m_snapshotMu);
            m_snapshot = std::move(s);
        }
        else
        {
            // Record the failure into the existing snapshot's
            // lastError field so the UI can surface it.
            std::lock_guard<std::mutex> lock(m_snapshotMu);
            m_snapshot.lastError = s.lastError;
        }
    }

    while (!m_stopRequested.load())
    {
        std::unique_lock<std::mutex> lock(m_wakeMu);
        m_wakeCv.wait_for(lock, kPollInterval, [this] {
            return m_stopRequested.load() || m_refreshRequested;
        });
        m_refreshRequested = false;
        lock.unlock();

        if (m_stopRequested.load()) break;

        Snapshot s;
        if (fetchOnce(s))
        {
            std::lock_guard<std::mutex> snap(m_snapshotMu);
            m_snapshot = std::move(s);
        }
        else
        {
            // Leave the previous snapshot entries in place; just
            // update the error field so the UI shows "stale, retrying"
            // instead of a blank list.
            std::lock_guard<std::mutex> snap(m_snapshotMu);
            m_snapshot.lastError = s.lastError;
        }
    }
}

bool DirectoryBrowser::fetchOnce(Snapshot &out)
{
    out.fetchedAt = std::chrono::steady_clock::now();

    auto resp = HttpClient::send(HttpClient::Method::GET,
                                 m_directoryUrl + "/streams");
    if (!resp.ok)
    {
        out.lastError = "fetch failed: " + resp.error;
        return false;
    }
    if (resp.statusCode != 200)
    {
        out.lastError = "HTTP " + std::to_string(resp.statusCode);
        return false;
    }

    try
    {
        auto j = json::parse(resp.body);
        auto data = j.at("data");

        if (data.contains("totalCount"))
        {
            out.totalCount = data["totalCount"].get<int>();
        }
        if (data.contains("streams"))
        {
            for (const auto &row : data["streams"])
            {
                Entry e;
                e.streamId         = row.value("streamId", "");
                e.name             = row.value("name", "");
                e.hostNickname     = row.value("hostNickname", "");
                e.shader           = row.value("shader", "");
                if (row.contains("resolution"))
                {
                    e.resolutionW = row["resolution"].value("w", 0u);
                    e.resolutionH = row["resolution"].value("h", 0u);
                }
                e.fps              = row.value("fps", 0u);
                e.codec            = row.value("codec", "");
                e.passwordRequired = row.value("passwordRequired", false);
                e.endpoint         = row.value("endpoint", "");
                e.endpointMode     = row.value("endpointMode", "");
                e.clientCount      = row.value("clientCount", 0);
                e.version          = row.value("version", "");
                e.expiresAt        = row.value("expiresAt", "");
                out.entries.push_back(std::move(e));
            }
        }
    }
    catch (const std::exception &e)
    {
        out.lastError = std::string("parse failed: ") + e.what();
        return false;
    }

    out.lastError.clear();
    return true;
}
