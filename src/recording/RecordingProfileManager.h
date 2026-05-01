#pragma once

#include "RecordingSettings.h"
#include <string>
#include <vector>

/**
 * Persists named RecordingSettings as JSON under
 * `Paths::getUserDataDir()/recording_profiles/<name>.json`.
 *
 * Simplified analogue of PresetManager — kept separate on purpose
 * because recording profiles are orthogonal to capture presets (the
 * same capture can be paired with different recording profiles).
 */
class RecordingProfileManager
{
public:
    RecordingProfileManager();
    ~RecordingProfileManager() = default;

    bool save(const std::string &name, const RecordingSettings &settings);
    bool load(const std::string &name, RecordingSettings &settings);
    bool remove(const std::string &name);
    bool exists(const std::string &name) const;
    std::vector<std::string> list();

    std::string getProfilesDirectory() const;

    static std::string sanitizeName(const std::string &name);

private:
    void ensureDirectoryExists();

    std::string m_profilesDir;
};
