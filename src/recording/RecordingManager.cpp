#include "RecordingManager.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>

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
    m_currentMetadata.filename = fs::path(outputPath).filename().string();
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

    // Configure StreamSynchronizer
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
    audioConfig.sampleRate = m_audioSampleRate;
    audioConfig.channels = m_audioChannels;
    audioConfig.bitrate = settings.audioBitrate;
    audioConfig.codec = settings.audioCodec;

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

    // Flush encoder
    if (m_encoder.isInitialized())
    {
        std::vector<MediaEncoder::EncodedPacket> packets;
        m_encoder.flush(packets);

        // Mux remaining packets
        for (const auto& packet : packets)
        {
            m_recorder.muxPacket(packet);
        }
    }

    // Stop recorder
    m_recorder.stopRecording();

    // Finalize metadata
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_currentMetadata.fileSize = m_recorder.getFileSize();
        m_currentMetadata.durationUs = m_recorder.getDurationUs();
    }

    // Add to recordings list
    finalizeCurrentRecording();

    // Cleanup
    m_encoder.cleanup();
    m_recorder.cleanup();
    m_synchronizer.clear();

    m_recording = false;
    m_running = false;
    m_stopRequest = false;

    LOG_INFO("RecordingManager: Stopped recording");
}

void RecordingManager::pushFrame(const uint8_t* data, uint32_t width, uint32_t height)
{
    if (!m_recording)
    {
        return;
    }

    int64_t timestampUs = getTimestampUs();
    m_synchronizer.addVideoFrame(data, width, height, timestampUs);
}

void RecordingManager::pushAudio(const int16_t* samples, size_t sampleCount)
{
    if (!m_recording || !m_settings.includeAudio)
    {
        return;
    }

    int64_t timestampUs = getTimestampUs();
    m_synchronizer.addAudioChunk(samples, sampleCount, timestampUs, m_audioSampleRate, m_audioChannels);
}

void RecordingManager::encodingThread()
{
    // Wait a bit before starting to avoid processing very old frames
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

        // Calculate sync zone
        StreamSynchronizer::SyncZone syncZone = m_synchronizer.calculateSyncZone();

        if (syncZone.isValid())
        {
            // Get synchronized video frames
            auto videoFrames = m_synchronizer.getVideoFrames(syncZone);

            // Process video frames
            size_t framesProcessed = 0;
            size_t MAX_FRAMES_PER_ITERATION = hasBacklog ? 5 : 2;

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
                            m_recorder.muxPacket(packet);
                        }
                        processedAny = true;
                        framesProcessed++;
                    }
                }
            }

            // Get synchronized audio chunks
            auto audioChunks = m_synchronizer.getAudioChunks(syncZone);

            // Process audio chunks
            size_t chunksProcessed = 0;
            size_t MAX_CHUNKS_PER_ITERATION = hasBacklog ? 8 : 3;

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
                            m_recorder.muxPacket(packet);
                        }
                        processedAny = true;
                        chunksProcessed++;
                    }
                }
            }

            // Mark data as processed
            m_synchronizer.markVideoProcessed(syncZone.videoStartIdx, syncZone.videoEndIdx);
            m_synchronizer.markAudioProcessed(syncZone.audioStartIdx, syncZone.audioEndIdx);

            // Update status
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_currentFileSize = m_recorder.getFileSize();
                m_currentDurationUs = m_recorder.getDurationUs();
            }
        }

        // Small delay to avoid busy waiting
        if (!processedAny)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

uint64_t RecordingManager::getCurrentDurationUs() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_currentDurationUs;
}

uint64_t RecordingManager::getCurrentFileSize() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_currentFileSize;
}

std::string RecordingManager::getCurrentFilename() const
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
