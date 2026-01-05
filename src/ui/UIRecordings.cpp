#include "UIRecordings.h"
#include "UIManager.h"
#include "../core/Application.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include <imgui.h>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef PLATFORM_LINUX
#include <cstdlib>
#elif PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

UIRecordings::UIRecordings(UIManager* uiManager)
    : m_uiManager(uiManager)
{
}

UIRecordings::~UIRecordings()
{
}

void UIRecordings::setVisible(bool visible)
{
    if (visible && !m_visible)
    {
        m_justOpened = true;
        if (!m_recordingsLoaded)
        {
            refreshRecordings();
        }
    }
    m_visible = visible;
}

void UIRecordings::refreshRecordings()
{
    if (!m_application)
    {
        return;
    }

    m_recordings = m_application->listRecordings();
    m_recordingsLoaded = true;
    
    // Sort by creation date (newest first)
    std::sort(m_recordings.begin(), m_recordings.end(),
              [](const RecordingMetadata& a, const RecordingMetadata& b) {
                  return a.createdAt > b.createdAt;
              });
}

void UIRecordings::render()
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
        ImVec2 initialSize(900.0f, 600.0f);
        
        ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
        
        m_justOpened = false;
    }

    // Main window
    ImGui::Begin("Recordings", &m_visible, ImGuiWindowFlags_NoSavedSettings);

    // Header with Refresh button and search
    if (ImGui::Button("Refresh"))
    {
        refreshRecordings();
    }

    ImGui::SameLine();
    ImGui::Text("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##search", m_searchFilter, sizeof(m_searchFilter));

    ImGui::Separator();

    // Recordings table
    renderRecordingsTable();

    // Dialogs (always render, they handle their own visibility)
    renderRenameDialog();
    renderDeleteDialog();

    ImGui::End();
}

void UIRecordings::renderRecordingsTable()
{
    // Filter recordings based on search
    std::vector<RecordingMetadata> filteredRecordings;
    if (strlen(m_searchFilter) == 0)
    {
        filteredRecordings = m_recordings;
    }
    else
    {
        std::string filterLower = m_searchFilter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
        
        for (const auto& recording : m_recordings)
        {
            std::string filenameLower = recording.filename;
            std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);
            if (filenameLower.find(filterLower) != std::string::npos)
            {
                filteredRecordings.push_back(recording);
            }
        }
    }

    if (filteredRecordings.empty())
    {
        ImGui::Text("No recordings found.");
        return;
    }

    // Table header
    ImGui::BeginTable("recordings_table", 7, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
    ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableHeadersRow();

    // Table rows
    for (size_t i = 0; i < filteredRecordings.size(); ++i)
    {
        const auto& recording = filteredRecordings[i];
        ImGui::TableNextRow();
        
        // Filename
        ImGui::TableSetColumnIndex(0);
        bool isSelected = (m_selectedRecordingId == recording.id);
        if (ImGui::Selectable(recording.filename.c_str(), isSelected, ImGuiSelectableFlags_None))
        {
            m_selectedRecordingId = recording.id;
        }

        // Resolution
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%ux%u @ %u fps", recording.width, recording.height, recording.fps);

        // Duration
        ImGui::TableSetColumnIndex(2);
        char durationStr[64];
        formatDuration(recording.durationUs, durationStr, sizeof(durationStr));
        ImGui::Text("%s", durationStr);

        // Size
        ImGui::TableSetColumnIndex(3);
        char sizeStr[64];
        formatFileSize(recording.fileSize, sizeStr, sizeof(sizeStr));
        ImGui::Text("%s", sizeStr);

        // Created
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%s", recording.createdAt.c_str());

        // Actions
        ImGui::TableSetColumnIndex(5);
        ImGui::PushID(("actions_" + recording.id).c_str());
        
        if (ImGui::SmallButton("Open"))
        {
            openRecordingInSystem(recording.filepath);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename"))
        {
            m_renameRecordingId = recording.id;
            strncpy(m_newRecordingName, recording.filename.c_str(), sizeof(m_newRecordingName) - 1);
            m_newRecordingName[sizeof(m_newRecordingName) - 1] = '\0';
            m_showRenameDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            m_deleteRecordingId = recording.id;
            m_deleteRecordingName = recording.filename;
            m_showDeleteDialog = true;
        }
        
        ImGui::PopID();

        // Details indicator
        ImGui::TableSetColumnIndex(6);
        if (isSelected)
        {
            ImGui::Text(">");
        }
    }

    ImGui::EndTable();

    // Details panel
    if (!m_selectedRecordingId.empty())
    {
        renderRecordingDetails();
    }
}

void UIRecordings::renderRecordingDetails()
{
    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                          [this](const RecordingMetadata& r) { return r.id == m_selectedRecordingId; });
    
    if (it == m_recordings.end())
    {
        m_selectedRecordingId.clear();
        return;
    }

    const auto& recording = *it;

    ImGui::Separator();
    ImGui::Text("Details:");
    ImGui::BeginChild("details", ImVec2(0, 150.0f), true);
    
    ImGui::Text("ID: %s", recording.id.c_str());
    ImGui::Text("File: %s", recording.filename.c_str());
    ImGui::Text("Path: %s", recording.filepath.c_str());
    ImGui::Text("Container: %s", recording.container.c_str());
    ImGui::Text("Video Codec: %s", recording.videoCodec.c_str());
    ImGui::Text("Audio Codec: %s", recording.audioCodec.empty() ? "None" : recording.audioCodec.c_str());
    
    ImGui::EndChild();
}

void UIRecordings::renderRenameDialog()
{
    if (m_showRenameDialog)
    {
        ImGui::OpenPopup("Rename Recording");
        m_showRenameDialog = false; // Reset flag after opening
    }
    
    if (ImGui::BeginPopupModal("Rename Recording", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Find recording name
        std::string currentName;
        auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                              [this](const RecordingMetadata& r) { return r.id == m_renameRecordingId; });
        if (it != m_recordings.end())
        {
            currentName = it->filename;
        }
        
        ImGui::Text("Rename recording:");
        ImGui::Text("%s", currentName.c_str());
        ImGui::Separator();
        
        ImGui::Text("New name:");
        ImGui::InputText("##newname", m_newRecordingName, sizeof(m_newRecordingName));
        
        if (ImGui::Button("OK", ImVec2(120, 0)))
        {
            if (strlen(m_newRecordingName) > 0)
            {
                renameRecording(m_renameRecordingId, m_newRecordingName);
                m_renameRecordingId.clear();
                refreshRecordings();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_renameRecordingId.clear();
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void UIRecordings::renderDeleteDialog()
{
    if (m_showDeleteDialog)
    {
        ImGui::OpenPopup("Delete Recording");
        m_showDeleteDialog = false; // Reset flag after opening
    }
    
    if (ImGui::BeginPopupModal("Delete Recording", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Are you sure you want to delete this recording?");
        ImGui::Text("%s", m_deleteRecordingName.c_str());
        ImGui::Separator();
        
        if (ImGui::Button("Yes, Delete", ImVec2(120, 0)))
        {
            std::string idToDelete = m_deleteRecordingId;
            deleteRecording(idToDelete);
            m_deleteRecordingId.clear();
            m_deleteRecordingName.clear();
            if (m_selectedRecordingId == idToDelete)
            {
                m_selectedRecordingId.clear();
            }
            refreshRecordings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_deleteRecordingId.clear();
            m_deleteRecordingName.clear();
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void UIRecordings::formatDuration(uint64_t durationUs, char* buffer, size_t bufferSize)
{
    uint64_t seconds = durationUs / 1000000ULL;
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    
    seconds %= 60;
    minutes %= 60;
    
    if (hours > 0)
    {
        snprintf(buffer, bufferSize, "%llu:%02llu:%02llu", 
                 static_cast<unsigned long long>(hours),
                 static_cast<unsigned long long>(minutes),
                 static_cast<unsigned long long>(seconds));
    }
    else
    {
        snprintf(buffer, bufferSize, "%llu:%02llu", 
                 static_cast<unsigned long long>(minutes),
                 static_cast<unsigned long long>(seconds));
    }
}

void UIRecordings::formatFileSize(uint64_t fileSize, char* buffer, size_t bufferSize)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(fileSize);
    
    while (size >= 1024.0 && unitIndex < 4)
    {
        size /= 1024.0;
        unitIndex++;
    }
    
    snprintf(buffer, bufferSize, "%.2f %s", size, units[unitIndex]);
}

void UIRecordings::deleteRecording(const std::string& recordingId)
{
    if (!m_application)
    {
        return;
    }

    if (m_application->deleteRecording(recordingId))
    {
        LOG_INFO("UIRecordings: Deleted recording: " + recordingId);
    }
    else
    {
        LOG_ERROR("UIRecordings: Failed to delete recording: " + recordingId);
    }
}

void UIRecordings::renameRecording(const std::string& recordingId, const std::string& newName)
{
    if (!m_application)
    {
        return;
    }

    if (m_application->renameRecording(recordingId, newName))
    {
        LOG_INFO("UIRecordings: Renamed recording: " + recordingId + " to " + newName);
    }
    else
    {
        LOG_ERROR("UIRecordings: Failed to rename recording: " + recordingId);
    }
}

void UIRecordings::openRecordingInSystem(const std::string& filepath)
{
#ifdef PLATFORM_LINUX
    std::string command = "xdg-open \"" + filepath + "\" &";
    // Ignore return value - we don't care if xdg-open succeeds or fails
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-result"
    system(command.c_str());
    #pragma GCC diagnostic pop
#elif PLATFORM_WINDOWS
    ShellExecuteA(nullptr, "open", filepath.c_str(), nullptr, nullptr, SW_SHOW);
#else
    // macOS
    std::string command = "open \"" + filepath + "\"";
    // Ignore return value - we don't care if open succeeds or fails
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-result"
    system(command.c_str());
    #pragma GCC diagnostic pop
#endif
}
