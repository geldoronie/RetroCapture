#include "RecordingManager.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <limits>

RecordingManager::RecordingManager()
{
    // Default metadata path
    m_metadataPath = "config/recordings.json";
}

RecordingManager::~RecordingManager()
{
    shutdown();
}

int64_t RecordingManager::getTimestampUs() const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

std::string RecordingManager::generateFilename(const RecordingSettings& settings)
{
    // Get current time
    std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    
    // Format filename using template
    std::stringstream ss;
    ss << std::put_time(tm, settings.filenameTemplate.c_str());
    
    std::string filename = ss.str();
    
    // Add container extension
    if (!settings.container.empty())
    {
        filename += "." + settings.container;
    }
    else
    {
        filename += ".mp4"; // Default
    }
    
    // Build full path
    fs::path outputDir(settings.outputPath);
    fs::path fullPath = outputDir / filename;
    
    return fullPath.string();
}

bool RecordingManager::initialize()
{
    if (m_initialized)
    {
        return true;
    }

    // Load existing recordings metadata
    loadRecordingsMetadata();

    m_initialized = true;
    LOG_INFO("RecordingManager: Initialized");
    return true;
}

void RecordingManager::shutdown()
{
    if (m_recording)
    {
        stopRecording();
    }

    if (m_running)
    {
        m_stopRequest = true;
        if (m_encodingThread.joinable())
        {
            m_encodingThread.join();
        }
        m_running = false;
    }

    m_encoder.cleanup();
    m_recorder.cleanup();
    m_synchronizer.clear();

    m_initialized = false;
}

bool RecordingManager::startRecording(const RecordingSettings& settings)
{
    if (m_recording)
    {
        LOG_WARN("RecordingManager: Already recording");
        return false;
    }

    m_settings = settings;

    // Generate output filename
    std::string outputPath = generateFilename(settings);
    
    // Initialize metadata
    m_currentMetadata = RecordingMetadata();
    m_currentMetadata.filename = fs_helper::get_filename_string(fs::path(outputPath));
    m_currentMetadata.filepath = fs::absolute(fs::path(outputPath)).string();
    m_currentMetadata.container = settings.container;
    m_currentMetadata.videoCodec = settings.codec;
    m_currentMetadata.audioCodec = settings.includeAudio ? settings.audioCodec : "";
    m_currentMetadata.width = settings.width;
    m_currentMetadata.height = settings.height;
    m_currentMetadata.fps = settings.fps;
    
    // Generate ID (simple hash from filename + timestamp)
    std::time_t now = std::time(nullptr);
    std::stringstream ss;
    ss << m_currentMetadata.filename << "_" << now;
    m_currentMetadata.id = std::to_string(std::hash<std::string>{}(ss.str()));
    
    // Get creation timestamp
    std::time_t time = std::time(nullptr);
    std::tm* tm = std::gmtime(&time);
    std::stringstream timeStr;
    timeStr << std::put_time(tm, "%Y-%m-%dT%H:%M:%SZ");
    m_currentMetadata.createdAt = timeStr.str();
    
    // Reset timestamp tracking (not needed with absolute timestamps, but keep for cleanup)
    m_recordingStartTimestampUs = 0;
    m_videoFrameCount = 0;
    m_audioSampleCount = 0;

    // Configure MediaSynchronizer
    m_synchronizer.setMaxBufferTime(5 * 1000000LL); // 5 seconds
    m_synchronizer.setMaxVideoBufferSize(10);
    m_synchronizer.setMaxAudioBufferSize(20);
    m_synchronizer.setSyncTolerance(50 * 1000LL); // 50ms
    m_synchronizer.clear();

    // Configure MediaEncoder
    MediaEncoder::VideoConfig videoConfig;
    videoConfig.width = settings.width;
    videoConfig.height = settings.height;
    videoConfig.fps = settings.fps;
    videoConfig.bitrate = settings.bitrate;
    videoConfig.codec = settings.codec;
    videoConfig.preset = settings.preset;
    videoConfig.h265Profile = settings.h265Profile;
    videoConfig.h265Level = settings.h265Level;
    videoConfig.vp8Speed = settings.vp8Speed;
    videoConfig.vp9Speed = settings.vp9Speed;

    MediaEncoder::AudioConfig audioConfig;
    // Use default values if audio format not set (when streaming/audio not enabled)
    audioConfig.sampleRate = (m_audioSampleRate > 0) ? m_audioSampleRate : 44100;
    audioConfig.channels = (m_audioChannels > 0) ? m_audioChannels : 2;
    audioConfig.bitrate = settings.audioBitrate;
    audioConfig.codec = settings.audioCodec;
    
    // If audio is not included, disable audio encoding
    if (!settings.includeAudio)
    {
        audioConfig.codec = ""; // Empty codec means no audio
    }

    if (!m_encoder.initialize(videoConfig, audioConfig))
    {
        LOG_ERROR("RecordingManager: Failed to initialize MediaEncoder");
        return false;
    }

    // Initialize FileRecorder
    if (!m_recorder.initialize(videoConfig, audioConfig,
                               m_encoder.getVideoCodecContext(),
                               m_encoder.getAudioCodecContext(),
                               outputPath))
    {
        LOG_ERROR("RecordingManager: Failed to initialize FileRecorder");
        m_encoder.cleanup();
        return false;
    }

    // Start recording
    if (!m_recorder.startRecording())
    {
        LOG_ERROR("RecordingManager: Failed to start FileRecorder");
        m_recorder.cleanup();
        m_encoder.cleanup();
        return false;
    }

    // Start encoding thread
    m_stopRequest = false;
    m_running = true;
    m_recording = true;
    m_encodingThread = std::thread(&RecordingManager::encodingThread, this);

    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_currentFilename = outputPath;
        m_currentFileSize = 0;
        m_currentDurationUs = 0;
    }

    LOG_INFO("RecordingManager: Started recording to: " + outputPath);
    return true;
}

void RecordingManager::stopRecording()
{
    if (!m_recording)
    {
        return;
    }

    m_stopRequest = true;

    // Wait for encoding thread to finish
    if (m_encodingThread.joinable())
    {
        m_encodingThread.join();
    }

    // Flush encoder and get remaining packets
    if (m_encoder.isInitialized())
    {
        std::vector<MediaEncoder::EncodedPacket> packets;
        m_encoder.flush(packets);

        // Mux remaining packets before stopping
        for (const auto& packet : packets)
        {
            if (m_recorder.isRecording())
            {
                m_recorder.muxPacket(packet);
            }
        }
    }

    // Flush recorder to ensure all data is written
    // Note: stopRecording() will flush and close file, but cleanup() needs file open for av_write_trailer
    if (m_recorder.isRecording())
    {
        m_recorder.flush();
        // Stop recorder (flushes but keeps file open for cleanup)
        m_recorder.stopRecording();
    }

    // Finalize metadata before cleanup
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_currentMetadata.fileSize = m_recorder.getFileSize();
        m_currentMetadata.durationUs = m_recorder.getDurationUs();
    }

    // Add to recordings list (before cleanup to save metadata)
    finalizeCurrentRecording();

    // Cleanup in correct order: 
    // 1. Flush and stop recorder (but keep file open for av_write_trailer)
    // 2. Cleanup recorder (calls av_write_trailer, then closes file)
    // 3. Cleanup encoder
    // 4. Clear synchronizer
    LOG_INFO("RecordingManager: Starting cleanup sequence");
    m_recorder.cleanup();
    LOG_INFO("RecordingManager: Recorder cleaned up, cleaning encoder");
    m_encoder.cleanup();
    LOG_INFO("RecordingManager: Encoder cleaned up, clearing synchronizer");
    m_synchronizer.clear();
    LOG_INFO("RecordingManager: Cleanup sequence completed");

    // Reset state
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_currentFilename.clear();
        m_currentFileSize = 0;
        m_currentDurationUs = 0;
    }

    m_recording = false;
    m_running = false;
    m_stopRequest = false;
    
    // Reset timestamp tracking
    m_recordingStartTimestampUs = 0;
    m_videoFrameCount = 0;
    m_audioSampleCount = 0;

    LOG_INFO("RecordingManager: Stopped recording");
}

void RecordingManager::pushFrame(const uint8_t* data, uint32_t width, uint32_t height)
{
    if (!m_recording)
    {
        return;
    }

    // Use absolute timestamp when frame arrives
    // This ensures frames are timestamped based on when they're actually captured
    int64_t timestampUs = getTimestampUs();
    
    bool added = m_synchronizer.addVideoFrame(data, width, height, timestampUs);
    
    static int frameCount = 0;
    static int logCount = 0;
    frameCount++;
    
    if (!added)
    {
        if (logCount < 3)
        {
            LOG_WARN("RecordingManager: Failed to add video frame to synchronizer");
            logCount++;
        }
    }
    else if (frameCount == 1 || frameCount % 60 == 0)
    {
        // Log first frame and every 60 frames (1 second at 60fps)
        LOG_INFO("RecordingManager: Pushed frame " + std::to_string(frameCount) + 
                 " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    }
}

void RecordingManager::pushAudio(const int16_t* samples, size_t sampleCount)
{
    if (!m_recording || !m_settings.includeAudio)
    {
        return;
    }

    // Use absolute timestamp when audio chunk arrives
    // This ensures audio is timestamped based on when it's actually captured
    int64_t timestampUs = getTimestampUs();
    
    m_synchronizer.addAudioChunk(samples, sampleCount, timestampUs, m_audioSampleRate, m_audioChannels);
}

void RecordingManager::encodingThread()
{
    LOG_INFO("RecordingManager: Encoding thread started");
    // Wait a bit before starting to avoid processing very old frames
    // Same delay as streaming for consistency
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int cleanupCounter = 0;
    const int CLEANUP_INTERVAL = 10;

    while (m_running && !m_stopRequest)
    {
        bool processedAny = false;

        // Cleanup old data occasionally
        cleanupCounter++;
        if (cleanupCounter >= CLEANUP_INTERVAL)
        {
            m_synchronizer.cleanupOldData();
            cleanupCounter = 0;
        }

        // Check for backlog
        size_t videoBufferSize = m_synchronizer.getVideoBufferSize();
        size_t audioBufferSize = m_synchronizer.getAudioBufferSize();
        bool hasBacklog = (videoBufferSize > 5 || audioBufferSize > 10);

        // Log buffer status periodically
        static int bufferLogCounter = 0;
        bufferLogCounter++;
        if (bufferLogCounter == 1 || bufferLogCounter % 100 == 0)
        {
            LOG_INFO("RecordingManager: Buffer status - Video: " + std::to_string(videoBufferSize) + 
                     ", Audio: " + std::to_string(audioBufferSize) + 
                     ", IncludeAudio: " + std::string(m_settings.includeAudio ? "true" : "false") +
                     ", AudioSR: " + std::to_string(m_audioSampleRate) +
                     ", AudioCh: " + std::to_string(m_audioChannels));
        }

        // Calculate sync zone
        MediaSynchronizer::SyncZone syncZone = m_synchronizer.calculateSyncZone();
        
        // Log sync zone status periodically
        static int syncZoneLogCounter = 0;
        syncZoneLogCounter++;
        if (syncZoneLogCounter == 1 || syncZoneLogCounter % 100 == 0)
        {
            if (syncZone.isValid())
            {
                LOG_INFO("RecordingManager: SyncZone valid - Video: [" + std::to_string(syncZone.videoStartIdx) + 
                         "-" + std::to_string(syncZone.videoEndIdx) + "], Audio: [" + 
                         std::to_string(syncZone.audioStartIdx) + "-" + std::to_string(syncZone.audioEndIdx) + 
                         "], Time: [" + std::to_string(syncZone.startTimeUs) + "-" + 
                         std::to_string(syncZone.endTimeUs) + "]");
            }
            else
            {
                LOG_INFO("RecordingManager: SyncZone invalid - Video buffer: " + std::to_string(videoBufferSize) + 
                         ", Audio buffer: " + std::to_string(audioBufferSize));
            }
        }

        // If no valid sync zone, check if we can process video-only
        if (!syncZone.isValid())
        {
            // If audio is not included or not available, process video-only
            if (!m_settings.includeAudio || m_audioSampleRate == 0 || m_audioChannels == 0)
            {
                // Process video frames without audio synchronization
                size_t videoBufferSize = m_synchronizer.getVideoBufferSize();
                if (videoBufferSize > 0)
                {
                    // Create a fake sync zone for video-only processing
                    // Note: isValid() requires audioEndIdx > audioStartIdx, so we set it to 1
                    syncZone = MediaSynchronizer::SyncZone();
                    syncZone.videoStartIdx = 0;
                    syncZone.videoEndIdx = std::min(videoBufferSize, static_cast<size_t>(2)); // Process up to 2 frames
                    syncZone.audioStartIdx = 0;
                    syncZone.audioEndIdx = 1; // Set to 1 to pass isValid() check, but we won't process audio
                    syncZone.startTimeUs = 0;
                    syncZone.endTimeUs = 1; // Set to 1 to pass isValid() check (startTimeUs < endTimeUs)
                    
                    static bool firstVideoOnlyLog = false;
                    if (!firstVideoOnlyLog)
                    {
                        LOG_INFO("RecordingManager: Processing video-only (no audio sync required)");
                        firstVideoOnlyLog = true;
                    }
                }
                else
                {
                    // No video frames yet - wait a bit
                    static int noFramesLogCounter = 0;
                    noFramesLogCounter++;
                    if (noFramesLogCounter == 1 || noFramesLogCounter % 100 == 0)
                    {
                        LOG_INFO("RecordingManager: Waiting for video frames (buffer empty)");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }
            else
            {
                // Audio is required but sync zone is invalid - wait for both
                // Don't process video-only when audio is required to maintain sync
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        
        if (syncZone.isValid() && syncZone.videoEndIdx > syncZone.videoStartIdx)
        {
            // Get synchronized video frames (or video-only if audio not included)
            auto videoFrames = m_synchronizer.getVideoFrames(syncZone);

            // Process video frames
            // Use same limits as streaming for consistency
            size_t framesProcessed = 0;
            size_t MAX_FRAMES_PER_ITERATION = hasBacklog ? 5 : 2; // Same as streaming

            for (const auto& frame : videoFrames)
            {
                if (m_stopRequest)
                {
                    break;
                }

                if (framesProcessed >= MAX_FRAMES_PER_ITERATION)
                {
                    break;
                }

                if (!frame.processed && frame.data && frame.width > 0 && frame.height > 0)
                {
                    // Encode frame
                    std::vector<MediaEncoder::EncodedPacket> packets;
                    if (m_encoder.encodeVideo(frame.data->data(), frame.width, frame.height,
                                             frame.captureTimestampUs, packets))
                    {
                        // Mux packets
                        for (const auto& packet : packets)
                        {
                            if (!m_recorder.muxPacket(packet))
                            {
                                LOG_ERROR("RecordingManager: Failed to mux packet");
                            }
                        }
                        processedAny = true;
                        framesProcessed++;
                        
                        // Log first successful frame
                        static bool firstFrameLogged = false;
                        if (!firstFrameLogged)
                        {
                            LOG_INFO("RecordingManager: First frame encoded and muxed successfully");
                            firstFrameLogged = true;
                        }
                    }
                    else
                    {
                        static int encodeErrorCount = 0;
                        if (encodeErrorCount++ < 3)
                        {
                            LOG_WARN("RecordingManager: Failed to encode video frame");
                        }
                    }
                }
            }

            // Get synchronized audio chunks (only if audio is included and available)
            std::vector<MediaSynchronizer::TimestampedAudio> audioChunks;
            if (m_settings.includeAudio && m_audioSampleRate > 0 && m_audioChannels > 0 && syncZone.audioEndIdx > syncZone.audioStartIdx)
            {
                audioChunks = m_synchronizer.getAudioChunks(syncZone);
            }

            // Process audio chunks
            // Use same limits as streaming for consistency
            size_t chunksProcessed = 0;
            size_t MAX_CHUNKS_PER_ITERATION = hasBacklog ? 8 : 3; // Same as streaming

            for (const auto& chunk : audioChunks)
            {
                if (m_stopRequest)
                {
                    break;
                }

                if (chunksProcessed >= MAX_CHUNKS_PER_ITERATION)
                {
                    break;
                }

                if (!chunk.processed && chunk.samples && chunk.sampleCount > 0)
                {
                    // Encode audio
                    std::vector<MediaEncoder::EncodedPacket> packets;
                    if (m_encoder.encodeAudio(chunk.samples->data(), chunk.sampleCount,
                                             chunk.captureTimestampUs, packets))
                    {
                        // Mux packets
                        for (const auto& packet : packets)
                        {
                            if (!m_recorder.muxPacket(packet))
                            {
                                static int audioMuxErrorCount = 0;
                                if (audioMuxErrorCount++ < 3)
                                {
                                    LOG_ERROR("RecordingManager: Failed to mux audio packet");
                                }
                            }
                        }
                        processedAny = true;
                        chunksProcessed++;
                        
                        // Log first successful audio chunk
                        static bool firstAudioLogged = false;
                        if (!firstAudioLogged)
                        {
                            LOG_INFO("RecordingManager: First audio chunk encoded and muxed successfully (" + 
                                     std::to_string(chunk.sampleCount) + " samples)");
                            firstAudioLogged = true;
                        }
                    }
                    else
                    {
                        static int audioEncodeErrorCount = 0;
                        if (audioEncodeErrorCount++ < 3)
                        {
                            LOG_WARN("RecordingManager: Failed to encode audio chunk (" + 
                                     std::to_string(chunk.sampleCount) + " samples)");
                        }
                    }
                }
            }

            // CRITICAL: Mark only frames that were actually processed
            // Since frames are sorted by timestamp, we need to mark them individually
            // to avoid marking wrong frames as processed
            for (const auto& frame : videoFrames)
            {
                if (frame.processed || !frame.data) continue; // Skip if already processed or invalid
                
                // Find and mark this specific frame in the buffer
                // We need to find it by timestamp since order may have changed
                m_synchronizer.markVideoFrameProcessedByTimestamp(frame.captureTimestampUs);
            }
            
            // Mark audio chunks as processed
            if (syncZone.audioEndIdx > syncZone.audioStartIdx)
            {
                for (const auto& chunk : audioChunks)
                {
                    if (chunk.processed || !chunk.samples) continue;
                    m_synchronizer.markAudioChunkProcessedByTimestamp(chunk.captureTimestampUs);
                }
            }

            // Update status
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_currentFileSize = m_recorder.getFileSize();
                m_currentDurationUs = m_recorder.getDurationUs();
            }
        }

        // Add delay based on FPS (same as streaming) to maintain natural rate
        // Reduce delay when there's backlog to process faster
        if (processedAny)
        {
            if (hasBacklog)
            {
                // When there's backlog, minimal delay to not consume 100% CPU but process fast
                std::this_thread::sleep_for(std::chrono::microseconds(100)); // 100µs only
            }
            else
            {
                // No backlog: delay based on FPS to maintain natural rate
                // For 60 FPS: ~16.67ms per frame, but we process 2 frames, so ~8ms
                int64_t frameTimeUs = 1000000LL / static_cast<int64_t>(m_settings.fps > 0 ? m_settings.fps : 60);
                int64_t delayUs = frameTimeUs / 2; // Delay proportional (since we process 2 frames)
                std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
            }
        }
        else
        {
            // No data processed, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

uint64_t RecordingManager::getCurrentDurationUs()
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_currentDurationUs;
}

uint64_t RecordingManager::getCurrentFileSize()
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_currentFileSize;
}

std::string RecordingManager::getCurrentFilename()
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_currentFilename;
}

bool RecordingManager::loadRecordingsMetadata()
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    try
    {
        if (!fs::exists(m_metadataPath))
        {
            return true; // No metadata file yet, that's OK
        }

        std::ifstream file(m_metadataPath);
        if (!file.is_open())
        {
            LOG_WARN("RecordingManager: Failed to open metadata file: " + m_metadataPath);
            return false;
        }

        nlohmann::json json;
        file >> json;
        file.close();

        if (json.contains("recordings") && json["recordings"].is_array())
        {
            m_recordings.clear();
            for (const auto& item : json["recordings"])
            {
                m_recordings.push_back(RecordingMetadata::fromJSON(item));
            }
        }

        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("RecordingManager: Exception loading metadata: " + std::string(e.what()));
        return false;
    }
}

bool RecordingManager::saveRecordingsMetadata()
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    try
    {
        // Ensure directory exists
        fs::path metadataPath(m_metadataPath);
        fs::path dir = metadataPath.parent_path();
        if (!dir.empty() && !fs::exists(dir))
        {
            fs::create_directories(dir);
        }

        nlohmann::json json;
        json["recordings"] = nlohmann::json::array();
        for (const auto& recording : m_recordings)
        {
            json["recordings"].push_back(recording.toJSON());
        }

        std::ofstream file(m_metadataPath);
        if (!file.is_open())
        {
            LOG_ERROR("RecordingManager: Failed to open metadata file for writing: " + m_metadataPath);
            return false;
        }

        file << json.dump(2);
        file.close();

        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("RecordingManager: Exception saving metadata: " + std::string(e.what()));
        return false;
    }
}

void RecordingManager::finalizeCurrentRecording()
{
    // Update file size and duration from filesystem if needed
    if (fs::exists(m_currentMetadata.filepath))
    {
        try
        {
            m_currentMetadata.fileSize = static_cast<uint64_t>(fs::file_size(m_currentMetadata.filepath));
        }
        catch (...)
        {
            // Ignore errors
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_recordingsMutex);
        m_recordings.push_back(m_currentMetadata);
    }

    saveRecordingsMetadata();
}

std::vector<RecordingMetadata> RecordingManager::listRecordings()
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);
    return m_recordings;
}

bool RecordingManager::deleteRecording(const std::string& recordingId)
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                          [&recordingId](const RecordingMetadata& m) { return m.id == recordingId; });

    if (it == m_recordings.end())
    {
        return false;
    }

    // Delete file
    try
    {
        if (fs::exists(it->filepath))
        {
            fs::remove(it->filepath);
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("RecordingManager: Failed to delete file: " + std::string(e.what()));
    }

    // Remove from list
    m_recordings.erase(it);

    // Save metadata
    return saveRecordingsMetadata();
}

std::string RecordingManager::getRecordingPath(const std::string& recordingId)
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                          [&recordingId](const RecordingMetadata& m) { return m.id == recordingId; });

    if (it != m_recordings.end())
    {
        return it->filepath;
    }

    return "";
}

void RecordingManager::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_audioSampleRate = sampleRate;
    m_audioChannels = channels;
}
