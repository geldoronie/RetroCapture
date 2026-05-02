#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * Snapshot of the streaming-side settings that get persisted as a
 * profile. These fields mirror the `m_streaming*` members of
 * `UIManager` — kept here as a plain struct so the manager and the
 * REST layer don't need to depend on `UIManager.h`.
 */
struct StreamingSettings
{
    uint16_t port = 8000;
    uint32_t width = 640;
    uint32_t height = 480;
    uint32_t fps = 30;
    uint32_t bitrate = 2000000;
    uint32_t audioBitrate = 128000;
    std::string videoCodec = "h264";
    std::string audioCodec = "aac";
    std::string h264Preset = "veryfast";
    std::string h265Preset = "veryfast";
    std::string h265Profile = "main";
    std::string h265Level = "auto";
    int vp8Speed = 12;
    int vp9Speed = 6;
    size_t maxVideoBufferSize = 15;
    size_t maxAudioBufferSize = 30;
    int64_t maxBufferTimeSeconds = 5;
    size_t avioBufferSize = 256 * 1024;
};

/**
 * Persists named StreamingSettings as JSON under
 * `Paths::getUserDataDir()/streaming_profiles/<name>.json`.
 *
 * Mirrors RecordingProfileManager — both expose the same lifecycle
 * (save/load/list/delete/exists) and follow the same path scheme.
 */
class StreamingProfileManager
{
public:
    StreamingProfileManager();
    ~StreamingProfileManager() = default;

    bool save(const std::string &name, const StreamingSettings &settings);
    bool load(const std::string &name, StreamingSettings &settings);
    bool remove(const std::string &name);
    bool exists(const std::string &name) const;
    std::vector<std::string> list();

    std::string getProfilesDirectory() const;

    static std::string sanitizeName(const std::string &name);

private:
    void ensureDirectoryExists();

    std::string m_profilesDir;
};
