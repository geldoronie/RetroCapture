#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <thread>
#include <atomic>
#include <future>
#include "../recording/RecordingMetadata.h"

// Forward declarations
class UIManager;
class Application;

/**
 * @brief Window for managing video recordings
 * 
 * Displays recordings in a list/table with options to delete, rename, and view details.
 */
class UIRecordings
{
public:
    UIRecordings(UIManager* uiManager);
    ~UIRecordings();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }
    void setJustOpened(bool justOpened) { m_justOpened = justOpened; }

    /**
     * @brief Set Application reference for accessing RecordingManager
     */
    void setApplication(Application* application) { m_application = application; }

    /**
     * @brief Refresh recordings list (reload from RecordingManager)
     */
    void refreshRecordings();

private:
    UIManager* m_uiManager = nullptr;
    Application* m_application = nullptr;
    
    bool m_visible = false;
    bool m_justOpened = false;

    // Recordings list
    std::vector<RecordingMetadata> m_recordings;
    bool m_recordingsLoaded = false;

    // Search/filter
    char m_searchFilter[256] = {0};

    // Rename dialog
    bool m_showRenameDialog = false;
    std::string m_renameRecordingId;
    char m_newRecordingName[512] = {0};

    // Delete confirmation
    bool m_showDeleteDialog = false;
    std::string m_deleteRecordingId;
    std::string m_deleteRecordingName;

    // Selected recording for details
    std::string m_selectedRecordingId;

    void renderRecordingsTable();
    void renderRenameDialog();
    void renderDeleteDialog();
    void renderRecordingDetails();
    void formatDuration(uint64_t durationUs, char* buffer, size_t bufferSize);
    void formatFileSize(uint64_t fileSize, char* buffer, size_t bufferSize);
    void deleteRecording(const std::string& recordingId);
    void renameRecording(const std::string& recordingId, const std::string& newName);
    void openRecordingInSystem(const std::string& filepath);
};
