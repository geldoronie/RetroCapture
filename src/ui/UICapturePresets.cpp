#include "UICapturePresets.h"
#include "UIManager.h"
#include "../utils/PresetManager.h"
#include "../utils/ThumbnailGenerator.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include "../core/Application.h"
#include <imgui.h>
#include <png.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

UICapturePresets::UICapturePresets(UIManager* uiManager)
    : m_uiManager(uiManager)
{
    m_presetManager = std::make_unique<PresetManager>();
    m_thumbnailGenerator = std::make_unique<ThumbnailGenerator>();
}

UICapturePresets::~UICapturePresets()
{
    clearThumbnails();
}

void UICapturePresets::setVisible(bool visible)
{
    if (visible && !m_visible)
    {
        m_justOpened = true;
        if (!m_presetsLoaded)
        {
            refreshPresets();
        }
    }
    m_visible = visible;
}

void UICapturePresets::refreshPresets()
{
    if (!m_presetManager)
    {
        return;
    }

    m_presetNames = m_presetManager->listPresets();
    m_presetsLoaded = true;
    
    // Load thumbnails for all presets
    loadAllThumbnails();
}

void UICapturePresets::render()
{
    if (!m_visible)
    {
        return;
    }

    // Apply initial position and size when window is opened
    if (m_justOpened)
    {
        float menuBarHeight = ImGui::GetFrameHeight();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;
        
        ImVec2 initialPos(workPos.x + 50.0f, workPos.y + menuBarHeight + 50.0f);
        ImVec2 initialSize(800.0f, 600.0f);
        
        ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
        
        m_justOpened = false;
    }

    // Main window
    ImGui::Begin("Capture Presets", &m_visible, ImGuiWindowFlags_NoSavedSettings);

    // Header with Create button and search
    if (ImGui::Button("Create New Preset"))
    {
        m_showCreateDialog = true;
        memset(m_newPresetName, 0, sizeof(m_newPresetName));
        memset(m_newPresetDescription, 0, sizeof(m_newPresetDescription));
        m_captureThumbnail = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        refreshPresets();
    }

    ImGui::SameLine();
    ImGui::Text("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##search", m_searchFilter, sizeof(m_searchFilter));

    ImGui::Separator();

    // Preset grid
    renderPresetGrid();

    // Create preset dialog
    if (m_showCreateDialog)
    {
        renderCreateDialog();
    }

    ImGui::End();
}

void UICapturePresets::renderPresetGrid()
{
    // Filter presets based on search
    std::vector<std::string> filteredPresets;
    if (strlen(m_searchFilter) == 0)
    {
        filteredPresets = m_presetNames;
    }
    else
    {
        std::string filterLower = m_searchFilter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
        
        for (const auto& name : m_presetNames)
        {
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(filterLower) != std::string::npos)
            {
                filteredPresets.push_back(name);
            }
        }
    }

    if (filteredPresets.empty())
    {
        ImGui::Text("No presets found. Click 'Create New Preset' to create one.");
        return;
    }

    // Calculate grid layout
    float windowWidth = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>(windowWidth / (THUMBNAIL_WIDTH + 20.0f));
    if (columns < 1) columns = 1;
    if (columns > 5) columns = 5; // Max 5 columns

    // Render grid
    int index = 0;
    for (const auto& presetName : filteredPresets)
    {
        if (index % columns != 0)
        {
            ImGui::SameLine();
        }
        
        renderPresetCard(presetName, index);
        index++;
    }
}

void UICapturePresets::renderPresetCard(const std::string& presetName, int index)
{
    ImGui::PushID(index);
    
    // Card frame
    ImGui::BeginChild(("card_" + presetName).c_str(), ImVec2(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT + 40.0f), true);
    
    // Thumbnail
    GLuint thumbnailTexture = 0;
    uint32_t thumbWidth = 0, thumbHeight = 0;
    
    auto thumbIt = m_thumbnailTextures.find(presetName);
    if (thumbIt != m_thumbnailTextures.end())
    {
        thumbnailTexture = thumbIt->second;
        thumbWidth = m_thumbnailWidths[presetName];
        thumbHeight = m_thumbnailHeights[presetName];
    }
    
    if (thumbnailTexture != 0)
    {
        // Calculate aspect ratios
        float thumbnailAspect = static_cast<float>(thumbWidth) / static_cast<float>(thumbHeight);
        float targetAspect = THUMBNAIL_WIDTH / THUMBNAIL_HEIGHT;
        
        // Calculate UV coordinates for centered crop
        // Note: ImGui uses top-left (0,0) to bottom-right (1,1) coordinate system
        // Since we're saving thumbnails correctly (not inverted), we need to flip UVs vertically
        ImVec2 uv0, uv1;
        if (thumbnailAspect > targetAspect)
        {
            // Thumbnail is wider - crop horizontally (left/right)
            float cropWidth = thumbHeight * targetAspect;
            float cropOffset = (thumbWidth - cropWidth) / 2.0f;
            uv0 = ImVec2(cropOffset / thumbWidth, 0.0f); // Top-left (flipped)
            uv1 = ImVec2(1.0f - (cropOffset / thumbWidth), 1.0f); // Bottom-right (flipped)
        }
        else
        {
            // Thumbnail is taller - crop vertically (top/bottom)
            float cropHeight = thumbWidth / targetAspect;
            float cropOffset = (thumbHeight - cropHeight) / 2.0f;
            uv0 = ImVec2(0.0f, cropOffset / thumbHeight); // Top-left (flipped)
            uv1 = ImVec2(1.0f, 1.0f - (cropOffset / thumbHeight)); // Bottom-right (flipped)
        }
        
        ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(thumbnailTexture)),
                     ImVec2(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT), uv0, uv1);
        
        // Click on thumbnail to apply preset
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
        {
            applyPreset(presetName);
        }
    }
    else
    {
        // Placeholder
        ImGui::Dummy(ImVec2(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT));
        ImGui::SameLine();
        ImGui::Text("No thumbnail");
        
        // Click on placeholder to apply preset
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
        {
            applyPreset(presetName);
        }
    }
    
    // Preset name (use display name from preset, or fallback to file name)
    std::string displayName = presetName;
    auto nameIt = m_presetDisplayNames.find(presetName);
    if (nameIt != m_presetDisplayNames.end())
    {
        displayName = nameIt->second;
    }
    ImGui::TextWrapped("%s", displayName.c_str());
    
    // Click on name to apply preset
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
    {
        applyPreset(presetName);
    }
    
    // Right-click context menu - use window context for the child window (card)
    // Use unique ID for each card to avoid conflicts
    std::string popupId = "preset_popup_" + presetName;
    if (ImGui::BeginPopupContextWindow(popupId.c_str()))
    {
        if (ImGui::MenuItem("Apply"))
        {
            applyPreset(presetName);
        }
        if (ImGui::MenuItem("Delete"))
        {
            if (m_presetManager)
            {
                m_presetManager->deletePreset(presetName);
                refreshPresets();
            }
        }
        ImGui::EndPopup();
    }
    
    ImGui::EndChild();
    ImGui::PopID();
}

void UICapturePresets::renderCreateDialog()
{
    ImGui::OpenPopup("Create Preset");
    
    if (ImGui::BeginPopupModal("Create Preset", &m_showCreateDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Preset Name:");
        ImGui::InputText("##name", m_newPresetName, sizeof(m_newPresetName));
        
        ImGui::Text("Description (optional):");
        ImGui::InputTextMultiline("##description", m_newPresetDescription, sizeof(m_newPresetDescription), ImVec2(300, 60));
        
        ImGui::Checkbox("Capture thumbnail from current viewport", &m_captureThumbnail);
        
        ImGui::Separator();
        
        if (ImGui::Button("Create"))
        {
            if (strlen(m_newPresetName) > 0)
            {
                createPresetFromCurrentState();
                m_showCreateDialog = false;
                refreshPresets();
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_showCreateDialog = false;
        }
        
        ImGui::EndPopup();
    }
}

void UICapturePresets::loadPresetThumbnail(const std::string& presetName)
{
    if (!m_presetManager)
    {
        return;
    }

    PresetManager::PresetData data;
    if (!m_presetManager->loadPreset(presetName, data))
    {
        return;
    }

    if (data.thumbnailPath.empty())
    {
        return;
    }

    // Resolve thumbnail path (may be relative or absolute)
    fs::path thumbnailPathObj(data.thumbnailPath);
    std::string resolvedPath;
    
    if (thumbnailPathObj.is_absolute() && fs::exists(thumbnailPathObj))
    {
        // Already absolute and exists
        resolvedPath = data.thumbnailPath;
    }
    else
    {
        // Try as relative to thumbnails directory
        fs::path thumbnailsDir = fs::path(m_presetManager->getThumbnailsDirectory());
        fs::path relativePath = thumbnailsDir / thumbnailPathObj;
        
        if (fs::exists(relativePath))
        {
            resolvedPath = relativePath.string();
        }
        else
        {
            // Fallback: try just filename in thumbnails directory
            fs::path fallbackPath = thumbnailsDir / thumbnailPathObj.filename();
            if (fs::exists(fallbackPath))
            {
                resolvedPath = fallbackPath.string();
            }
            else
            {
                // Last resort: try preset name + .png
                fs::path lastResort = thumbnailsDir / (presetName + ".png");
                if (fs::exists(lastResort))
                {
                    resolvedPath = lastResort.string();
                }
                else
                {
                    return; // Thumbnail not found
                }
            }
        }
    }

    loadThumbnailTexture(resolvedPath, m_thumbnailTextures[presetName],
                         m_thumbnailWidths[presetName], m_thumbnailHeights[presetName]);
}

void UICapturePresets::loadAllThumbnails()
{
    clearThumbnails();
    
    for (const auto& presetName : m_presetNames)
    {
        loadPresetThumbnail(presetName);
    }
}

void UICapturePresets::clearThumbnails()
{
    for (auto& pair : m_thumbnailTextures)
    {
        if (pair.second != 0)
        {
            glDeleteTextures(1, &pair.second);
        }
    }
    m_thumbnailTextures.clear();
    m_thumbnailWidths.clear();
    m_thumbnailHeights.clear();
}

void UICapturePresets::applyPreset(const std::string& presetName)
{
    if (!m_presetManager || !m_application)
    {
        LOG_ERROR("Cannot apply preset: missing PresetManager or Application");
        return;
    }

    // Use Application's applyPreset method
    m_application->applyPreset(presetName);
}

void UICapturePresets::createPresetFromCurrentState()
{
    if (!m_presetManager || !m_uiManager || !m_application)
    {
        LOG_ERROR("Cannot create preset: missing dependencies");
        return;
    }

    std::string presetName(m_newPresetName);
    if (presetName.empty())
    {
        LOG_ERROR("Preset name cannot be empty");
        return;
    }

    std::string description(m_newPresetDescription);

    // Use Application's createPresetFromCurrentState method (handles thumbnail capture internally)
    bool captureThumbnail = m_captureThumbnail && m_thumbnailGenerator != nullptr;
    m_application->createPresetFromCurrentState(presetName, description, captureThumbnail);
}

bool UICapturePresets::loadThumbnailTexture(const std::string& thumbnailPath, GLuint& texture, uint32_t& width, uint32_t& height)
{
    if (!fs::exists(thumbnailPath))
    {
        return false;
    }

    FILE* fp = fopen(thumbnailPath.c_str(), "rb");
    if (!fp)
    {
        return false;
    }

    // Verify PNG signature
    png_byte header[8];
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
    {
        fclose(fp);
        return false;
    }

    // Initialize libpng
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(fp);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to RGBA 8-bit
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY)
        png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);

    png_read_update_info(png_ptr, info_ptr);

    // Allocate image data
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
    int rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    std::vector<uint8_t> imageData(rowbytes * height);

    for (uint32_t y = 0; y < height; y++)
    {
        row_pointers[y] = imageData.data() + y * rowbytes;
    }

    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, info_ptr);

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(fp);
    free(row_pointers);

    // Create OpenGL texture
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, imageData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return true;
}

