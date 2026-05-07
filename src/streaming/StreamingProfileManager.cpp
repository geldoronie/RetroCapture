#include "StreamingProfileManager.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../utils/FilesystemCompat.h"

#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <algorithm>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace {

std::string nowIso8601()
{
    auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

} // namespace

StreamingProfileManager::StreamingProfileManager()
{
    ensureDirectoryExists();
}

void StreamingProfileManager::ensureDirectoryExists()
{
    m_profilesDir = getProfilesDirectory();
    if (!fs::exists(m_profilesDir))
    {
        try
        {
            fs::create_directories(m_profilesDir);
            LOG_INFO("Created streaming profiles directory: " + m_profilesDir);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to create streaming profiles directory: " + std::string(e.what()));
        }
    }
}

std::string StreamingProfileManager::getProfilesDirectory() const
{
    const char *over = std::getenv("RETROCAPTURE_STREAMING_PROFILES_DIR");
    if (over && *over) return over;
    return (fs::path(Paths::getUserDataDir()) / "streaming_profiles").string();
}

std::string StreamingProfileManager::sanitizeName(const std::string &name)
{
    std::string out = name;
    for (char &c : out)
    {
        if (c == ' ') c = '_';
        else if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                 c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ' || out.front() == '_')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == ' ' || out.back() == '_')) out.pop_back();
    return out;
}

bool StreamingProfileManager::save(const std::string &name, const StreamingSettings &s)
{
    std::string clean = sanitizeName(name);
    if (clean.empty())
    {
        LOG_ERROR("StreamingProfileManager::save: empty/invalid name");
        return false;
    }

    json j;
    j["name"] = clean;
    j["created"] = nowIso8601();
    j["version"] = 1;

    j["network"] = {
        {"port", s.port},
    };
    j["video"] = {
        {"width", s.width},
        {"height", s.height},
        {"fps", s.fps},
        {"bitrate", s.bitrate},
        {"codec", s.videoCodec},
        {"h264Preset", s.h264Preset},
        {"h265Preset", s.h265Preset},
        {"h265Profile", s.h265Profile},
        {"h265Level", s.h265Level},
        {"vp8Speed", s.vp8Speed},
        {"vp9Speed", s.vp9Speed},
    };
    j["audio"] = {
        {"bitrate", s.audioBitrate},
        {"codec", s.audioCodec},
    };
    j["buffer"] = {
        {"maxVideoBufferSize", s.maxVideoBufferSize},
        {"maxAudioBufferSize", s.maxAudioBufferSize},
        {"maxBufferTimeSeconds", s.maxBufferTimeSeconds},
        {"avioBufferSize", s.avioBufferSize},
    };

    fs::path path = fs::path(m_profilesDir) / (clean + ".json");
    try
    {
        std::ofstream f(path.string());
        if (!f.is_open())
        {
            LOG_ERROR("StreamingProfileManager::save: cannot open " + path.string());
            return false;
        }
        f << j.dump(2);
        LOG_INFO("Streaming profile saved: " + path.string());
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("StreamingProfileManager::save: ") + e.what());
        return false;
    }
}

bool StreamingProfileManager::load(const std::string &name, StreamingSettings &s)
{
    std::string clean = sanitizeName(name);
    if (clean.empty()) return false;

    fs::path path = fs::path(m_profilesDir) / (clean + ".json");
    if (!fs::exists(path))
    {
        LOG_ERROR("StreamingProfileManager::load: not found " + path.string());
        return false;
    }

    try
    {
        std::ifstream f(path.string());
        json j; f >> j;

        // Defaults come from the struct itself — any missing field
        // keeps its default, so older profiles remain loadable.
        if (j.contains("network"))
        {
            const auto &n = j["network"];
            if (n.contains("port")) s.port = n["port"].get<uint16_t>();
        }
        if (j.contains("video"))
        {
            const auto &v = j["video"];
            if (v.contains("width")) s.width = v["width"].get<uint32_t>();
            if (v.contains("height")) s.height = v["height"].get<uint32_t>();
            if (v.contains("fps")) s.fps = v["fps"].get<uint32_t>();
            if (v.contains("bitrate")) s.bitrate = v["bitrate"].get<uint32_t>();
            if (v.contains("codec")) s.videoCodec = v["codec"].get<std::string>();
            if (v.contains("h264Preset")) s.h264Preset = v["h264Preset"].get<std::string>();
            if (v.contains("h265Preset")) s.h265Preset = v["h265Preset"].get<std::string>();
            if (v.contains("h265Profile")) s.h265Profile = v["h265Profile"].get<std::string>();
            if (v.contains("h265Level")) s.h265Level = v["h265Level"].get<std::string>();
            if (v.contains("vp8Speed")) s.vp8Speed = v["vp8Speed"].get<int>();
            if (v.contains("vp9Speed")) s.vp9Speed = v["vp9Speed"].get<int>();
        }
        if (j.contains("audio"))
        {
            const auto &a = j["audio"];
            if (a.contains("bitrate")) s.audioBitrate = a["bitrate"].get<uint32_t>();
            if (a.contains("codec")) s.audioCodec = a["codec"].get<std::string>();
        }
        if (j.contains("buffer"))
        {
            const auto &b = j["buffer"];
            if (b.contains("maxVideoBufferSize")) s.maxVideoBufferSize = b["maxVideoBufferSize"].get<size_t>();
            if (b.contains("maxAudioBufferSize")) s.maxAudioBufferSize = b["maxAudioBufferSize"].get<size_t>();
            if (b.contains("maxBufferTimeSeconds")) s.maxBufferTimeSeconds = b["maxBufferTimeSeconds"].get<int64_t>();
            if (b.contains("avioBufferSize")) s.avioBufferSize = b["avioBufferSize"].get<size_t>();
        }
        LOG_INFO("Streaming profile loaded: " + path.string());
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("StreamingProfileManager::load: ") + e.what());
        return false;
    }
}

bool StreamingProfileManager::remove(const std::string &name)
{
    std::string clean = sanitizeName(name);
    if (clean.empty()) return false;
    fs::path path = fs::path(m_profilesDir) / (clean + ".json");
    if (!fs::exists(path)) return false;
    try
    {
        fs::remove(path);
        LOG_INFO("Streaming profile deleted: " + path.string());
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(std::string("StreamingProfileManager::remove: ") + e.what());
        return false;
    }
}

bool StreamingProfileManager::exists(const std::string &name) const
{
    std::string clean = sanitizeName(name);
    if (clean.empty()) return false;
    fs::path path = fs::path(m_profilesDir) / (clean + ".json");
    return fs::exists(path);
}

std::vector<std::string> StreamingProfileManager::list()
{
    std::vector<std::string> names;
    if (!fs::exists(m_profilesDir)) return names;

    try
    {
        fs::directory_iterator it(m_profilesDir);
        fs::directory_iterator end;
        for (; it != end; ++it)
        {
            fs::path p = fs_helper::get_path(it);
            std::string ext = fs_helper::get_extension_string(p);
            if (ext != ".json") continue;
            std::string stem = p.stem();
            if (!stem.empty()) names.push_back(stem);
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("StreamingProfileManager::list: ") + e.what());
    }

    std::sort(names.begin(), names.end());
    return names;
}
