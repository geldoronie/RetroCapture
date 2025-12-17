#include "PresetManager.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cctype>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

// Include JSON library (nlohmann/json)
#include <nlohmann/json.hpp>
using json = nlohmann::json;

PresetManager::PresetManager()
{
    ensureDirectoriesExist();
}

void PresetManager::ensureDirectoriesExist()
{
    m_presetsDir = getPresetsDirectory();
    m_thumbnailsDir = getThumbnailsDirectory();

    // Create directories if they don't exist
    if (!fs::exists(m_presetsDir))
    {
        try
        {
            fs::create_directories(m_presetsDir);
            LOG_INFO("Created presets directory: " + m_presetsDir);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to create presets directory: " + std::string(e.what()));
        }
    }

    if (!fs::exists(m_thumbnailsDir))
    {
        try
        {
            fs::create_directories(m_thumbnailsDir);
            LOG_INFO("Created thumbnails directory: " + m_thumbnailsDir);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to create thumbnails directory: " + std::string(e.what()));
        }
    }
}

std::string PresetManager::getAssetsDirectory() const
{
    // Try multiple locations (similar to WebPortal::findAssetFile)
    
    // 1. Environment variable (for AppImage)
    const char* assetsEnvPath = std::getenv("RETROCAPTURE_ASSETS_PATH");
    if (assetsEnvPath)
    {
        fs::path envAssetsDir(assetsEnvPath);
        if (fs::exists(envAssetsDir) && fs::is_directory(envAssetsDir))
        {
            return fs::absolute(envAssetsDir).string();
        }
    }

    // 2. User config directory (~/.config/retrocapture/assets/)
    #ifdef _WIN32
    const char* appDataDir = std::getenv("APPDATA");
    if (!appDataDir)
    {
        appDataDir = std::getenv("LOCALAPPDATA");
    }
    if (appDataDir)
    {
        fs::path configDir = fs::path(appDataDir) / "RetroCapture" / "assets";
        if (fs::exists(configDir) && fs::is_directory(configDir))
        {
            return fs::absolute(configDir).string();
        }
    }
    #else
    const char* homeDir = std::getenv("HOME");
    if (homeDir)
    {
        fs::path configDir = fs::path(homeDir) / ".config" / "retrocapture" / "assets";
        if (fs::exists(configDir) && fs::is_directory(configDir))
        {
            return fs::absolute(configDir).string();
        }
    }
    #endif

    // 3. Executable directory/assets/
    char exePath[1024];
    #ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, exePath, sizeof(exePath) - 1);
    if (len != 0)
    {
        exePath[len] = '\0';
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path assetsDir = exeDir / "assets";
        if (fs::exists(assetsDir) && fs::is_directory(assetsDir))
        {
            return fs::absolute(assetsDir).string();
        }
    }
    #else
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1)
    {
        exePath[len] = '\0';
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path assetsDir = exeDir / "assets";
        if (fs::exists(assetsDir) && fs::is_directory(assetsDir))
        {
            return fs::absolute(assetsDir).string();
        }
    }
    #endif

    // 4. Current directory/assets/
    fs::path currentAssets = fs::current_path() / "assets";
    if (fs::exists(currentAssets) && fs::is_directory(currentAssets))
    {
        return fs::absolute(currentAssets).string();
    }

    // 5. Fallback: create in user config directory
    #ifdef _WIN32
    const char* fallbackDir = std::getenv("APPDATA");
    if (!fallbackDir)
    {
        fallbackDir = std::getenv("LOCALAPPDATA");
    }
    if (fallbackDir)
    {
        fs::path fallbackPath = fs::path(fallbackDir) / "RetroCapture" / "assets";
        return fs::absolute(fallbackPath).string();
    }
    #else
    const char* fallbackDir = std::getenv("HOME");
    if (fallbackDir)
    {
        fs::path fallbackPath = fs::path(fallbackDir) / ".config" / "retrocapture" / "assets";
        return fs::absolute(fallbackPath).string();
    }
    #endif

    // Last resort: current directory
    return (fs::current_path() / "assets").string();
}

std::string PresetManager::getPresetsDirectory() const
{
    fs::path assetsDir = getAssetsDirectory();
    return (assetsDir / "presets").string();
}

std::string PresetManager::getThumbnailsDirectory() const
{
    fs::path assetsDir = getAssetsDirectory();
    return (assetsDir / "thumbnails").string();
}

std::string PresetManager::sanitizeName(const std::string& name)
{
    std::string sanitized = name;
    
    // Remove or replace invalid characters
    for (char& c : sanitized)
    {
        // Replace spaces with underscores
        if (c == ' ')
        {
            c = '_';
        }
        // Remove invalid characters for filenames
        else if (c == '/' || c == '\\' || c == ':' || c == '*' || 
                 c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
        // Keep alphanumeric, dots, underscores, and hyphens
        else if (!std::isalnum(c) && c != '.' && c != '_' && c != '-')
        {
            c = '_';
        }
    }
    
    // Remove leading/trailing dots and spaces
    sanitized.erase(0, sanitized.find_first_not_of(" ."));
    sanitized.erase(sanitized.find_last_not_of(" .") + 1);
    
    // Ensure not empty
    if (sanitized.empty())
    {
        sanitized = "preset";
    }
    
    return sanitized;
}

bool PresetManager::presetExists(const std::string& name) const
{
    std::string sanitized = sanitizeName(name);
    fs::path presetPath = fs::path(m_presetsDir) / (sanitized + ".json");
    return fs::exists(presetPath) && fs::is_regular_file(presetPath);
}

// Helper function to generate ISO 8601 timestamp
static std::string getCurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return ss.str();
}

bool PresetManager::savePreset(const std::string& name, const PresetData& data)
{
    if (name.empty())
    {
        LOG_ERROR("Cannot save preset with empty name");
        return false;
    }

    std::string sanitized = sanitizeName(name);
    fs::path presetPath = fs::path(m_presetsDir) / (sanitized + ".json");

    try
    {
        json presetJson;
        
        // Metadata
        presetJson["version"] = data.version.empty() ? "1.0" : data.version;
        presetJson["name"] = data.name.empty() ? name : data.name;
        presetJson["description"] = data.description;
        presetJson["created"] = data.created.empty() ? getCurrentTimestamp() : data.created;
        presetJson["thumbnail"] = data.thumbnailPath;

        // Shader configuration
        if (!data.shaderPath.empty())
        {
            presetJson["shader"]["path"] = data.shaderPath;
            if (!data.shaderParameters.empty())
            {
                json params;
                for (const auto& [key, value] : data.shaderParameters)
                {
                    params[key] = value;
                }
                presetJson["shader"]["parameters"] = params;
            }
        }

        // Capture configuration
        // Note: devicePath is NOT saved - it varies between systems
        if (data.captureWidth > 0 && data.captureHeight > 0)
        {
            presetJson["capture"]["width"] = data.captureWidth;
            presetJson["capture"]["height"] = data.captureHeight;
            presetJson["capture"]["fps"] = data.captureFps;
            // devicePath is NOT saved - varies between systems
            presetJson["capture"]["sourceType"] = data.sourceType;
        }

        // Image settings
        // Note: fullscreen and monitorIndex are NOT saved - they vary per user/system
        presetJson["image"]["brightness"] = data.imageBrightness;
        presetJson["image"]["contrast"] = data.imageContrast;
        presetJson["image"]["maintainAspect"] = data.maintainAspect;
        // fullscreen is NOT saved - varies per user preference
        // monitorIndex is NOT saved - varies per system

        // Streaming settings (if any)
        if (data.streamingWidth > 0 || data.streamingHeight > 0)
        {
            presetJson["streaming"]["width"] = data.streamingWidth;
            presetJson["streaming"]["height"] = data.streamingHeight;
            presetJson["streaming"]["fps"] = data.streamingFps;
            presetJson["streaming"]["bitrate"] = data.streamingBitrate;
            presetJson["streaming"]["audioBitrate"] = data.streamingAudioBitrate;
            if (!data.streamingVideoCodec.empty())
            {
                presetJson["streaming"]["videoCodec"] = data.streamingVideoCodec;
            }
            if (!data.streamingAudioCodec.empty())
            {
                presetJson["streaming"]["audioCodec"] = data.streamingAudioCodec;
            }
            if (!data.streamingH264Preset.empty())
            {
                presetJson["streaming"]["h264Preset"] = data.streamingH264Preset;
            }
            if (!data.streamingH265Preset.empty())
            {
                presetJson["streaming"]["h265Preset"] = data.streamingH265Preset;
            }
            if (!data.streamingH265Profile.empty())
            {
                presetJson["streaming"]["h265Profile"] = data.streamingH265Profile;
            }
            if (!data.streamingH265Level.empty())
            {
                presetJson["streaming"]["h265Level"] = data.streamingH265Level;
            }
            presetJson["streaming"]["vp8Speed"] = data.streamingVP8Speed;
            presetJson["streaming"]["vp9Speed"] = data.streamingVP9Speed;
        }

        // V4L2 controls (if any)
        if (!data.v4l2Controls.empty())
        {
            json v4l2Json;
            for (const auto& [key, value] : data.v4l2Controls)
            {
                v4l2Json[key] = value;
            }
            presetJson["v4l2Controls"] = v4l2Json;
        }

        // Write to file
        std::ofstream file(presetPath.string());
        if (!file.is_open())
        {
            LOG_ERROR("Failed to open preset file for writing: " + presetPath.string());
            return false;
        }

        file << presetJson.dump(4); // Pretty print with 4 spaces indentation
        file.close();

        LOG_INFO("Preset saved: " + presetPath.string());
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Error saving preset: " + std::string(e.what()));
        return false;
    }
}

bool PresetManager::loadPreset(const std::string& name, PresetData& data)
{
    std::string sanitized = sanitizeName(name);
    fs::path presetPath = fs::path(m_presetsDir) / (sanitized + ".json");

    if (!fs::exists(presetPath) || !fs::is_regular_file(presetPath))
    {
        LOG_ERROR("Preset file not found: " + presetPath.string());
        return false;
    }

    try
    {
        std::ifstream file(presetPath.string());
        if (!file.is_open())
        {
            LOG_ERROR("Failed to open preset file: " + presetPath.string());
            return false;
        }

        json presetJson;
        file >> presetJson;
        file.close();

        // Load metadata
        if (presetJson.contains("version"))
        {
            data.version = presetJson["version"].get<std::string>();
        }
        if (presetJson.contains("name"))
        {
            data.name = presetJson["name"].get<std::string>();
        }
        if (presetJson.contains("description"))
        {
            data.description = presetJson["description"].get<std::string>();
        }
        if (presetJson.contains("created"))
        {
            data.created = presetJson["created"].get<std::string>();
        }
        if (presetJson.contains("thumbnail"))
        {
            data.thumbnailPath = presetJson["thumbnail"].get<std::string>();
        }

        // Load shader configuration
        if (presetJson.contains("shader"))
        {
            const auto& shader = presetJson["shader"];
            if (shader.contains("path"))
            {
                data.shaderPath = shader["path"].get<std::string>();
            }
            if (shader.contains("parameters"))
            {
                data.shaderParameters.clear();
                for (const auto& [key, value] : shader["parameters"].items())
                {
                    data.shaderParameters[key] = value.get<float>();
                }
            }
        }

        // Load capture configuration
        if (presetJson.contains("capture"))
        {
            const auto& capture = presetJson["capture"];
            if (capture.contains("width"))
            {
                data.captureWidth = capture["width"].get<uint32_t>();
            }
            if (capture.contains("height"))
            {
                data.captureHeight = capture["height"].get<uint32_t>();
            }
            if (capture.contains("fps"))
            {
                data.captureFps = capture["fps"].get<uint32_t>();
            }
            if (capture.contains("device"))
            {
                data.devicePath = capture["device"].get<std::string>();
            }
            if (capture.contains("sourceType"))
            {
                data.sourceType = capture["sourceType"].get<int>();
            }
        }

        // Load image settings
        if (presetJson.contains("image"))
        {
            const auto& image = presetJson["image"];
            if (image.contains("brightness"))
            {
                data.imageBrightness = image["brightness"].get<float>();
            }
            if (image.contains("contrast"))
            {
                data.imageContrast = image["contrast"].get<float>();
            }
            if (image.contains("maintainAspect"))
            {
                data.maintainAspect = image["maintainAspect"].get<bool>();
            }
            if (image.contains("fullscreen"))
            {
                data.fullscreen = image["fullscreen"].get<bool>();
            }
            if (image.contains("monitorIndex"))
            {
                data.monitorIndex = image["monitorIndex"].get<int>();
            }
        }

        // Load streaming settings
        if (presetJson.contains("streaming"))
        {
            const auto& streaming = presetJson["streaming"];
            if (streaming.contains("width"))
            {
                data.streamingWidth = streaming["width"].get<uint32_t>();
            }
            if (streaming.contains("height"))
            {
                data.streamingHeight = streaming["height"].get<uint32_t>();
            }
            if (streaming.contains("fps"))
            {
                data.streamingFps = streaming["fps"].get<uint32_t>();
            }
            if (streaming.contains("bitrate"))
            {
                data.streamingBitrate = streaming["bitrate"].get<uint32_t>();
            }
            if (streaming.contains("audioBitrate"))
            {
                data.streamingAudioBitrate = streaming["audioBitrate"].get<uint32_t>();
            }
            if (streaming.contains("videoCodec"))
            {
                data.streamingVideoCodec = streaming["videoCodec"].get<std::string>();
            }
            if (streaming.contains("audioCodec"))
            {
                data.streamingAudioCodec = streaming["audioCodec"].get<std::string>();
            }
            if (streaming.contains("h264Preset"))
            {
                data.streamingH264Preset = streaming["h264Preset"].get<std::string>();
            }
            if (streaming.contains("h265Preset"))
            {
                data.streamingH265Preset = streaming["h265Preset"].get<std::string>();
            }
            if (streaming.contains("h265Profile"))
            {
                data.streamingH265Profile = streaming["h265Profile"].get<std::string>();
            }
            if (streaming.contains("h265Level"))
            {
                data.streamingH265Level = streaming["h265Level"].get<std::string>();
            }
            if (streaming.contains("vp8Speed"))
            {
                data.streamingVP8Speed = streaming["vp8Speed"].get<int>();
            }
            if (streaming.contains("vp9Speed"))
            {
                data.streamingVP9Speed = streaming["vp9Speed"].get<int>();
            }
        }

        // Load V4L2 controls
        if (presetJson.contains("v4l2Controls"))
        {
            data.v4l2Controls.clear();
            for (const auto& [key, value] : presetJson["v4l2Controls"].items())
            {
                data.v4l2Controls[key] = value.get<int32_t>();
            }
        }

        LOG_INFO("Preset loaded: " + presetPath.string());
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Error loading preset: " + std::string(e.what()));
        return false;
    }
}

bool PresetManager::deletePreset(const std::string& name)
{
    std::string sanitized = sanitizeName(name);
    fs::path presetPath = fs::path(m_presetsDir) / (sanitized + ".json");
    fs::path thumbnailPath = fs::path(m_thumbnailsDir) / (sanitized + ".png");

    bool success = true;

    // Delete preset file
    if (fs::exists(presetPath))
    {
        try
        {
            if (!fs::remove(presetPath))
            {
                LOG_ERROR("Failed to delete preset file: " + presetPath.string());
                success = false;
            }
            else
            {
                LOG_INFO("Deleted preset: " + presetPath.string());
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Error deleting preset file: " + std::string(e.what()));
            success = false;
        }
    }

    // Delete thumbnail (optional, don't fail if it doesn't exist)
    if (fs::exists(thumbnailPath))
    {
        try
        {
            fs::remove(thumbnailPath);
            LOG_INFO("Deleted thumbnail: " + thumbnailPath.string());
        }
        catch (const std::exception& e)
        {
            LOG_WARN("Failed to delete thumbnail: " + std::string(e.what()));
        }
    }

    return success;
}

std::vector<std::string> PresetManager::listPresets()
{
    std::vector<std::string> presets;

    if (!fs::exists(m_presetsDir) || !fs::is_directory(m_presetsDir))
    {
        return presets;
    }

    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(m_presetsDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                std::string filename = entry.path().filename().string();
                // Remove .json extension
                if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json")
                {
                    presets.push_back(filename.substr(0, filename.size() - 5));
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Error listing presets: " + std::string(e.what()));
    }

    // Sort alphabetically
    std::sort(presets.begin(), presets.end());

    return presets;
}

