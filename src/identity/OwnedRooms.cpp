#include "OwnedRooms.h"

#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../utils/FilesystemCompat.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <random>

namespace
{
constexpr const char *kFileName = "owned_rooms.json";

std::string isoNow()
{
    const auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool writeAll(const std::vector<OwnedRoom> &rooms)
{
    const std::string path = ownedrooms::filePath();
    try
    {
        const fs::path parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent))
        {
            fs::create_directories(parent);
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ownedrooms::writeAll mkdir: ") + e.what());
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &r : rooms)
    {
        arr.push_back({
            {"room_id",      r.roomId},
            {"slug",         r.slug},
            {"title",        r.title},
            {"owner_secret", r.ownerSecret},
            {"createdAt",    r.createdAt},
        });
    }
    std::ofstream out(path);
    if (!out.good())
    {
        LOG_ERROR("ownedrooms::writeAll: cannot open " + path);
        return false;
    }
    out << nlohmann::json{{"rooms", arr}}.dump(2);
    return out.good();
}
} // namespace

namespace ownedrooms
{

std::string filePath()
{
    return (fs::path(Paths::getUserDataDir()) / kFileName).string();
}

std::vector<OwnedRoom> loadAll()
{
    std::vector<OwnedRoom> out;
    std::ifstream in(filePath());
    if (!in.good()) return out;
    try
    {
        nlohmann::json j;
        in >> j;
        const auto arr = j.value("rooms", nlohmann::json::array());
        out.reserve(arr.size());
        for (const auto &r : arr)
        {
            OwnedRoom o;
            o.roomId      = r.value("room_id",      std::string{});
            o.slug        = r.value("slug",         std::string{});
            o.title       = r.value("title",        std::string{});
            o.ownerSecret = r.value("owner_secret", std::string{});
            o.createdAt   = r.value("createdAt",    std::string{});
            out.push_back(std::move(o));
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ownedrooms::loadAll parse: ") + e.what());
        return {};
    }
    return out;
}

bool findBySlug(const std::string &slug, OwnedRoom &out)
{
    if (slug.empty()) return false;
    for (auto &r : loadAll())
    {
        if (r.slug == slug)
        {
            out = std::move(r);
            return true;
        }
    }
    return false;
}

bool append(const OwnedRoom &room)
{
    auto rooms = loadAll();
    // Replace existing entry on slug collision (re-create scenario).
    for (auto &existing : rooms)
    {
        if (existing.slug == room.slug)
        {
            existing = room;
            if (existing.createdAt.empty()) existing.createdAt = isoNow();
            return writeAll(rooms);
        }
    }
    OwnedRoom withTs = room;
    if (withTs.createdAt.empty()) withTs.createdAt = isoNow();
    rooms.push_back(withTs);
    return writeAll(rooms);
}

bool remove(const std::string &slug)
{
    if (slug.empty()) return false;
    auto rooms = loadAll();
    const auto before = rooms.size();
    rooms.erase(std::remove_if(rooms.begin(), rooms.end(),
                               [&](const OwnedRoom &r) { return r.slug == slug; }),
                rooms.end());
    if (rooms.size() == before) return false;
    return writeAll(rooms);
}

std::string generateSecret()
{
    // 16 bytes of entropy → 32 hex chars. random_device is good
    // enough here — this isn't a key-derivation operation; the
    // secret only matters as a shared bearer between the user's
    // disk and the chat server.
    std::random_device rd;
    char buf[33];
    for (int i = 0; i < 4; ++i)
    {
        std::uniform_int_distribution<uint32_t> d(0, 0xFFFFFFFFu);
        std::snprintf(buf + i * 8, 9, "%08x", d(rd));
    }
    buf[32] = '\0';
    return buf;
}

} // namespace ownedrooms
