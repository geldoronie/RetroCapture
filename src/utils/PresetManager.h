#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "../utils/FilesystemCompat.h"

/**
 * @brief Manages capture presets (save, load, list, delete)
 * 
 * Presets are stored as JSON files in assets/presets/ directory
 * and thumbnails in assets/thumbnails/ directory.
 */
class PresetManager
{
public:
    /**
     * @brief Complete preset data structure
     */
    struct PresetData
    {
        std::string name;
        std::string description;
        std::string thumbnailPath;
        
        // Shader configuration
        std::string shaderPath;
        std::map<std::string, float> shaderParameters;
        
        // Capture configuration
        uint32_t captureWidth = 0;
        uint32_t captureHeight = 0;
        uint32_t captureFps = 0;
        std::string devicePath;
        int sourceType = 0; // 0=None, 1=V4L2, 2=DirectShow
        
        // Image settings
        float imageBrightness = 1.0f;
        float imageContrast = 1.0f;
        bool maintainAspect = true;
        bool fullscreen = false;
        int monitorIndex = 0;
        
        // Streaming settings (optional)
        uint32_t streamingWidth = 0;
        uint32_t streamingHeight = 0;
        uint32_t streamingFps = 0;
        uint32_t streamingBitrate = 0;
        uint32_t streamingAudioBitrate = 0;
        std::string streamingVideoCodec;
        std::string streamingAudioCodec;
        std::string streamingH264Preset;
        std::string streamingH265Preset;
        std::string streamingH265Profile;
        std::string streamingH265Level;
        int streamingVP8Speed = 0;
        int streamingVP9Speed = 0;
        
        // V4L2 controls (optional)
        std::map<std::string, int32_t> v4l2Controls;
        
        // Metadata
        std::string created; // ISO 8601 timestamp
        std::string version; // Preset format version
    };

    PresetManager();
    ~PresetManager() = default;

    /**
     * @brief Save a preset to disk
     * @param name Preset name (will be sanitized for filename)
     * @param data Preset data to save
     * @return true if successful, false otherwise
     */
    bool savePreset(const std::string& name, const PresetData& data);

    /**
     * @brief Load a preset from disk
     * @param name Preset name
     * @param data Output preset data
     * @return true if successful, false otherwise
     */
    bool loadPreset(const std::string& name, PresetData& data);

    /**
     * @brief Delete a preset from disk
     * @param name Preset name
     * @return true if successful, false otherwise
     */
    bool deletePreset(const std::string& name);

    /**
     * @brief List all available presets
     * @return Vector of preset names (without .json extension)
     */
    std::vector<std::string> listPresets();

    /**
     * @brief Get the presets directory path
     * @return Full path to presets directory
     */
    std::string getPresetsDirectory() const;

    /**
     * @brief Get the thumbnails directory path
     * @return Full path to thumbnails directory
     */
    std::string getThumbnailsDirectory() const;

    /**
     * @brief Sanitize a name for use as filename
     * @param name Original name
     * @return Sanitized name safe for filesystem
     */
    static std::string sanitizeName(const std::string& name);

    /**
     * @brief Check if a preset exists
     * @param name Preset name
     * @return true if preset exists, false otherwise
     */
    bool presetExists(const std::string& name) const;

private:
    /**
     * @brief Ensure presets and thumbnails directories exist
     */
    void ensureDirectoriesExist();

    /**
     * @brief Get the base assets directory
     * @return Path to assets directory
     */
    std::string getAssetsDirectory() const;

    std::string m_presetsDir;
    std::string m_thumbnailsDir;
};

