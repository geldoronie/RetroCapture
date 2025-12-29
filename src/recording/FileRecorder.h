#pragma once

#include "../encoding/MediaEncoder.h"
#include "../encoding/MediaMuxer.h"
#include <string>
#include <cstdint>
#include <fstream>
#include <mutex>

/**
 * FileRecorder - Specialized MediaMuxer for file recording
 *
 * Writes muxed data to file instead of HTTP stream.
 * Reuses MediaMuxer with a file write callback.
 */
class FileRecorder
{
public:
    FileRecorder();
    ~FileRecorder();

    // Initialize recorder with configurations and output path
    bool initialize(const MediaEncoder::VideoConfig& videoConfig,
                    const MediaEncoder::AudioConfig& audioConfig,
                    void* videoCodecContext,
                    void* audioCodecContext,
                    const std::string& outputPath);

    // Start recording
    bool startRecording();
    
    // Stop recording
    void stopRecording();
    
    // Check if recording
    bool isRecording() const { return m_recording; }

    // Mux a packet (called by RecordingManager)
    bool muxPacket(const MediaEncoder::EncodedPacket& packet);

    // Flush pending packets
    void flush();

    // Cleanup resources
    void cleanup();

    // Get metadata
    std::string getOutputPath() const { return m_outputPath; }
    uint64_t getFileSize();
    uint64_t getDurationUs() const { return m_durationUs; }

private:
    // File write callback for MediaMuxer
    int writeToFile(const uint8_t* data, size_t size);

    // Create output directory if needed
    bool ensureOutputDirectory(const std::string& path);

    MediaMuxer m_muxer;
    std::string m_outputPath;
    std::ofstream m_outputFile;
    std::mutex m_fileMutex;
    bool m_recording = false;
    bool m_initialized = false;
    
    // Metadata
    uint64_t m_durationUs = 0;
    int64_t m_startTimestampUs = 0;
    
    // Codec contexts (stored for MediaMuxer)
    void* m_videoCodecContext = nullptr;
    void* m_audioCodecContext = nullptr;
};
