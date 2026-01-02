#pragma once

#include "RecordingSettings.h"
#include "RecordingMetadata.h"
#include "FileRecorder.h"
#include "../encoding/MediaEncoder.h"
#include "../encoding/StreamSynchronizer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <string>

/**
 * RecordingManager - Orchestrates video recording
 *
 * Manages recording lifecycle, coordinates MediaEncoder/MediaMuxer,
 * and handles recording files.
 */
class RecordingManager
{
public:
    RecordingManager();
    ~RecordingManager();

    // Initialize recording manager
    bool initialize();
    
    // Shutdown and cleanup
    void shutdown();

    // Recording control
    bool startRecording(const RecordingSettings& settings);
    void stopRecording();
    bool isRecording() const { return m_recording; }

    // Configuration
    void setRecordingSettings(const RecordingSettings& settings) { m_settings = settings; }
    RecordingSettings getRecordingSettings() const { return m_settings; }

    // Status
    uint64_t getCurrentDurationUs();
    uint64_t getCurrentFileSize();
    std::string getCurrentFilename();

    // Frame/Audio input (called by Application)
    void pushFrame(const uint8_t* data, uint32_t width, uint32_t height);
    void pushAudio(const int16_t* samples, size_t sampleCount);
    
    // Set audio format (called by Application)
    void setAudioFormat(uint32_t sampleRate, uint32_t channels);

    // Recording list
    std::vector<RecordingMetadata> listRecordings();
    bool deleteRecording(const std::string& recordingId);
    std::string getRecordingPath(const std::string& recordingId);

private:
    // Encoding thread function
    void encodingThread();

    // Generate output filename from template
    std::string generateFilename(const RecordingSettings& settings);

    // Get timestamp in microseconds
    int64_t getTimestampUs() const;

    // Load recordings metadata from JSON
    bool loadRecordingsMetadata();
    
    // Save recordings metadata to JSON
    bool saveRecordingsMetadata();

    // Add current recording to metadata
    void finalizeCurrentRecording();

    FileRecorder m_recorder;
    MediaEncoder m_encoder;
    StreamSynchronizer m_synchronizer;
    
    RecordingSettings m_settings;
    RecordingMetadata m_currentMetadata;
    
    std::thread m_encodingThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_stopRequest{false};
    
    std::mutex m_statusMutex;
    std::string m_currentFilename;
    uint64_t m_currentFileSize = 0;
    uint64_t m_currentDurationUs = 0;
    
    // Recordings metadata
    std::vector<RecordingMetadata> m_recordings;
    std::mutex m_recordingsMutex;
    std::string m_metadataPath;
    
    // Audio format (from Application)
    uint32_t m_audioSampleRate = 44100;
    uint32_t m_audioChannels = 2;
    
    // Timestamp tracking for synchronization
    int64_t m_recordingStartTimestampUs = 0; // Timestamp absoluto do início da gravação
    uint64_t m_videoFrameCount = 0;          // Contador de frames desde o início
    uint64_t m_audioSampleCount = 0;         // Contador de samples desde o início
    
    bool m_initialized = false;
};
