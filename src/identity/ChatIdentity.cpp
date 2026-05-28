#include "ChatIdentity.h"

#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../utils/PasswordHash.h"
#include "../utils/FilesystemCompat.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace
{

constexpr const char *kFileName = "identity.json";

std::string isoNow()
{
    const auto now   = std::chrono::system_clock::now();
    const auto t     = std::chrono::system_clock::to_time_t(now);
    std::tm    tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string randomHex16()
{
    // 16 bytes of entropy → 32 hex chars. Mixed into the ID hash to
    // ensure two users typing identical name/nick/age at the same
    // second don't collide. std::random_device is good enough; this
    // is not a cryptographic key derivation.
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> d(0, 0xFFFFFFFFu);
    char buf[33];
    for (int i = 0; i < 4; ++i)
    {
        std::snprintf(buf + i * 8, 9, "%08x", d(rd));
    }
    buf[32] = '\0';
    return buf;
}

} // namespace

namespace identity
{

std::string filePath()
{
    return (fs::path(Paths::getUserDataDir()) / kFileName).string();
}

ChatIdentity load()
{
    ChatIdentity out;
    const std::string path = filePath();
    std::ifstream in(path);
    if (!in.good())
    {
        return out; // file missing — uninitialized identity is the signal
    }
    try
    {
        nlohmann::json j;
        in >> j;
        out.id        = j.value("id",        std::string{});
        out.name      = j.value("name",      std::string{});
        out.nickname  = j.value("nickname",  std::string{});
        out.age       = j.value("age",       0);
        out.createdAt = j.value("createdAt", std::string{});
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ChatIdentity::load: parse failed: ") + e.what());
        return ChatIdentity{}; // fall back to empty
    }
    return out;
}

bool save(ChatIdentity &id)
{
    // Generate id on the first save only. Once non-empty it is
    // immutable; subsequent saves just refresh the other fields.
    if (id.id.empty())
    {
        const std::string ts   = isoNow();
        const std::string rand = randomHex16();
        std::ostringstream src;
        src << id.name << '\0' << id.nickname << '\0' << id.age << '\0'
            << ts << '\0' << rand;
        const std::string hash = PasswordHash::sha256Hex(src.str());
        if (hash.size() < 12)
        {
            LOG_ERROR("ChatIdentity::save: sha256 produced unexpected size");
            return false;
        }
        id.id        = "rc_" + hash.substr(0, 12);
        id.createdAt = ts;
    }

    const std::string path = filePath();
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
        LOG_WARN(std::string("ChatIdentity::save: mkdir failed: ") + e.what());
    }

    nlohmann::json j = {
        {"id",        id.id},
        {"name",      id.name},
        {"nickname",  id.nickname},
        {"age",       id.age},
        {"createdAt", id.createdAt},
    };
    std::ofstream out(path);
    if (!out.good())
    {
        LOG_ERROR("ChatIdentity::save: cannot open " + path + " for write");
        return false;
    }
    out << j.dump(2);
    return out.good();
}

} // namespace identity
