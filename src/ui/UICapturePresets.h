#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include "../renderer/glad_loader.h"

// Forward declarations
class UIManager;
class PresetManager;
class ThumbnailGenerator;
class Application;

/**
 * @brief Window for managing capture presets
 * 
 * Displays presets in a grid with thumbnails and allows creating/applying presets.
 */
class UICapturePresets
{
public:
    UICapturePresets(UIManager* uiManager);
    ~UICapturePresets();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }
    void setJustOpened(bool justOpened) { m_justOpened = justOpened; }

    /**
     * @brief Set Application reference for applying presets
     */
    void setApplication(Application* application) { m_application = application; }

    /**
     * @brief Refresh preset list (reload from disk)
     */
    void refreshPresets();

private:
    UIManager* m_uiManager = nullptr;
    Application* m_application = nullptr;
    std::unique_ptr<PresetManager> m_presetManager;
    std::unique_ptr<ThumbnailGenerator> m_thumbnailGenerator;
    
    bool m_visible = false;
    bool m_justOpened = false;

    // Preset list (file names)
    std::vector<std::string> m_presetNames;
    // Map from file name to display name (from preset JSON)
    std::map<std::string, std::string> m_presetDisplayNames;
    bool m_presetsLoaded = false;

    // Thumbnail textures (preset name -> OpenGL texture ID)
    std::map<std::string, GLuint> m_thumbnailTextures;
    std::map<std::string, uint32_t> m_thumbnailWidths;
    std::map<std::string, uint32_t> m_thumbnailHeights;

    // Create preset dialog
    bool m_showCreateDialog = false;
    char m_newPresetName[256] = {0};
    char m_newPresetDescription[512] = {0};
    bool m_captureThumbnail = true;

    // Search/filter
    char m_searchFilter[256] = {0};

    // Grid settings
    static constexpr int GRID_COLUMNS = 3;
    static constexpr float THUMBNAIL_WIDTH = 200.0f;
    static constexpr float THUMBNAIL_HEIGHT = 150.0f;

    void renderPresetGrid();
    void renderCreateDialog();
    void renderPresetCard(const std::string& presetName, int index);
    void loadPresetThumbnail(const std::string& presetName);
    void loadAllThumbnails();
    void clearThumbnails();
    void applyPreset(const std::string& presetName);
    void createPresetFromCurrentState();
    bool loadThumbnailTexture(const std::string& thumbnailPath, GLuint& texture, uint32_t& width, uint32_t& height);
};

