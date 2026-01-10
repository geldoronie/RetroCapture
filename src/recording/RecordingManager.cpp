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

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

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

std::string RecordingManager::generateFilename(const RecordingSettings &settings)
{
    // Get current time
    std::time_t now = std::time(nullptr);
    std::tm *tm = std::localtime(&now);

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

bool RecordingManager::startRecording(const RecordingSettings &settings)
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
    std::tm *tm = std::gmtime(&time);
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

    // Inicializar encoder para gravação em arquivo (não streaming)
    if (!m_encoder.initialize(videoConfig, audioConfig, false))
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
        for (const auto &packet : packets)
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

    // Update file size after cleanup (file is now finalized)
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

    // Add to recordings list (after cleanup, file is finalized)
    // This will also generate thumbnail now that file is complete
    finalizeCurrentRecording();

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

void RecordingManager::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
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

void RecordingManager::pushAudio(const int16_t *samples, size_t sampleCount)
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

            for (const auto &frame : videoFrames)
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
                        for (const auto &packet : packets)
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

            for (const auto &chunk : audioChunks)
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
                        for (const auto &packet : packets)
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
            for (const auto &frame : videoFrames)
            {
                if (frame.processed || !frame.data)
                    continue; // Skip if already processed or invalid

                // Find and mark this specific frame in the buffer
                // We need to find it by timestamp since order may have changed
                m_synchronizer.markVideoFrameProcessedByTimestamp(frame.captureTimestampUs);
            }

            // Mark audio chunks as processed
            if (syncZone.audioEndIdx > syncZone.audioStartIdx)
            {
                for (const auto &chunk : audioChunks)
                {
                    if (chunk.processed || !chunk.samples)
                        continue;
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
            for (const auto &item : json["recordings"])
            {
                m_recordings.push_back(RecordingMetadata::fromJSON(item));
            }
        }

        return true;
    }
    catch (const std::exception &e)
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
        for (const auto &recording : m_recordings)
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
    catch (const std::exception &e)
    {
        LOG_ERROR("RecordingManager: Exception saving metadata: " + std::string(e.what()));
        return false;
    }
}

void RecordingManager::finalizeCurrentRecording()
{
    // Generate thumbnail from video file (file is already finalized at this point)
    if (fs::exists(m_currentMetadata.filepath))
    {
        // Generate thumbnail path: same name as video but with .jpg extension
        fs::path videoPath(m_currentMetadata.filepath);
        fs::path thumbnailPath = videoPath.parent_path() / (videoPath.stem() + ".jpg");

        if (generateThumbnail(m_currentMetadata.filepath, thumbnailPath.string()))
        {
            m_currentMetadata.thumbnailPath = thumbnailPath.string();
            LOG_INFO("RecordingManager: Thumbnail generated: " + m_currentMetadata.thumbnailPath);
        }
        else
        {
            LOG_WARN("RecordingManager: Failed to generate thumbnail for: " + m_currentMetadata.filepath);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_recordingsMutex);
        m_recordings.push_back(m_currentMetadata);
    }

    saveRecordingsMetadata();
}

bool RecordingManager::generateThumbnail(const std::string &videoPath, const std::string &thumbnailPath)
{
    AVFormatContext *formatCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    const AVCodec *codec = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *yuvFrame = nullptr;
    SwsContext *swsCtx = nullptr;
    int videoStreamIndex = -1;
    bool success = false;

    // Open video file
    if (avformat_open_input(&formatCtx, videoPath.c_str(), nullptr, nullptr) < 0)
    {
        LOG_ERROR("RecordingManager: Could not open video file: " + videoPath);
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(formatCtx, nullptr) < 0)
    {
        LOG_ERROR("RecordingManager: Could not find stream info");
        avformat_close_input(&formatCtx);
        return false;
    }

    // Find video stream
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++)
    {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1)
    {
        LOG_ERROR("RecordingManager: No video stream found");
        avformat_close_input(&formatCtx);
        return false;
    }

    // Get codec parameters
    AVCodecParameters *codecpar = formatCtx->streams[videoStreamIndex]->codecpar;

    // Find decoder
    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        LOG_ERROR("RecordingManager: Codec not found");
        avformat_close_input(&formatCtx);
        return false;
    }

    // Allocate codec context
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        LOG_ERROR("RecordingManager: Could not allocate codec context");
        avformat_close_input(&formatCtx);
        return false;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codecCtx, codecpar) < 0)
    {
        LOG_ERROR("RecordingManager: Could not copy codec parameters");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return false;
    }

    // Open codec
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        LOG_ERROR("RecordingManager: Could not open codec");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return false;
    }

    // Allocate frame
    frame = av_frame_alloc();
    if (!frame)
    {
        LOG_ERROR("RecordingManager: Could not allocate frame");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return false;
    }

    // Read first frame
    AVPacket *packet = av_packet_alloc();
    if (!packet)
    {
        LOG_ERROR("RecordingManager: Could not allocate packet");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        av_frame_free(&frame);
        return false;
    }

    // Seek to beginning
    av_seek_frame(formatCtx, videoStreamIndex, 0, AVSEEK_FLAG_FRAME);

    // Read until we get a video frame
    while (av_read_frame(formatCtx, packet) >= 0)
    {
        if (packet->stream_index == videoStreamIndex)
        {
            // Send packet to decoder
            if (avcodec_send_packet(codecCtx, packet) >= 0)
            {
                // Receive frame from decoder
                if (avcodec_receive_frame(codecCtx, frame) >= 0)
                {
                    int width = codecCtx->width;
                    int height = codecCtx->height;

                    // Use MJPEG encoder to save as JPEG
                    const AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
                    if (jpegCodec)
                    {
                        AVCodecContext *jpegCtx = avcodec_alloc_context3(jpegCodec);
                        if (jpegCtx)
                        {
                            jpegCtx->width = width;
                            jpegCtx->height = height;
                            jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
                            jpegCtx->time_base = {1, 25};

                            if (avcodec_open2(jpegCtx, jpegCodec, nullptr) >= 0)
                            {
                                // Allocate YUV frame
                                yuvFrame = av_frame_alloc();
                                if (yuvFrame)
                                {
                                    yuvFrame->format = AV_PIX_FMT_YUVJ420P;
                                    yuvFrame->width = width;
                                    yuvFrame->height = height;
                                    if (av_frame_get_buffer(yuvFrame, 32) >= 0)
                                    {
                                        // Convert decoded frame to YUVJ420P
                                        swsCtx = sws_getContext(
                                            width, height, codecCtx->pix_fmt,
                                            width, height, AV_PIX_FMT_YUVJ420P,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);

                                        if (swsCtx)
                                        {
                                            sws_scale(swsCtx, frame->data, frame->linesize,
                                                      0, height, yuvFrame->data, yuvFrame->linesize);

                                            yuvFrame->pts = 0;

                                            // Encode frame
                                            if (avcodec_send_frame(jpegCtx, yuvFrame) >= 0)
                                            {
                                                AVPacket *jpegPacket = av_packet_alloc();
                                                if (jpegPacket)
                                                {
                                                    if (avcodec_receive_packet(jpegCtx, jpegPacket) >= 0)
                                                    {
                                                        // Write to file
                                                        std::ofstream outFile(thumbnailPath, std::ios::binary);
                                                        if (outFile.is_open())
                                                        {
                                                            outFile.write(reinterpret_cast<const char *>(jpegPacket->data), jpegPacket->size);
                                                            outFile.close();
                                                            success = true;
                                                        }
                                                    }
                                                    av_packet_free(&jpegPacket);
                                                }
                                            }
                                            sws_freeContext(swsCtx);
                                        }
                                    }
                                    av_frame_free(&yuvFrame);
                                }
                            }
                            avcodec_free_context(&jpegCtx);
                        }
                    }

                    break; // Got frame, exit loop
                }
            }
        }
        av_packet_unref(packet);
    }

    // Cleanup
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);

    return success;
}

std::vector<RecordingMetadata> RecordingManager::listRecordings()
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);
    return m_recordings;
}

bool RecordingManager::deleteRecording(const std::string &recordingId)
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                           [&recordingId](const RecordingMetadata &m)
                           { return m.id == recordingId; });

    if (it == m_recordings.end())
    {
        LOG_INFO("RecordingManager: Recording not found: " + recordingId);
        return false;
    }

    std::string filepath = it->filepath; // Copiar antes de remover da lista

    // Delete file (if it exists) - fazer ANTES de remover da lista para evitar problemas
    // If file doesn't exist, just log and continue - we'll remove the record anyway
    try
    {
        if (fs::exists(filepath))
        {
            fs::remove(filepath);
            LOG_INFO("RecordingManager: Deleted file: " + filepath);
        }
        else
        {
            LOG_INFO("RecordingManager: File not found (already deleted?): " + filepath + " - removing from list");
        }
    }
    catch (const std::exception &e)
    {
        // Log error but continue - we'll still remove the record from metadata
        LOG_WARN("RecordingManager: Failed to delete file (continuing anyway): " + std::string(e.what()));
    }
    catch (...)
    {
        // Catch any other exception
        LOG_WARN("RecordingManager: Unknown error deleting file (continuing anyway)");
    }

    // Remove from list (always, even if file deletion failed or file doesn't exist)
    m_recordings.erase(it);

    // Save metadata - fazer sem lock adicional (já temos o lock)
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
        for (const auto &recording : m_recordings)
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

        LOG_INFO("RecordingManager: Metadata saved successfully");
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("RecordingManager: Exception saving metadata: " + std::string(e.what()));
        return false;
    }
    catch (...)
    {
        LOG_ERROR("RecordingManager: Unknown exception saving metadata");
        return false;
    }
}

std::string RecordingManager::getRecordingPath(const std::string &recordingId)
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                           [&recordingId](const RecordingMetadata &m)
                           { return m.id == recordingId; });

    if (it != m_recordings.end())
    {
        return it->filepath;
    }

    return "";
}

bool RecordingManager::renameRecording(const std::string &recordingId, const std::string &newName)
{
    std::lock_guard<std::mutex> lock(m_recordingsMutex);

    auto it = std::find_if(m_recordings.begin(), m_recordings.end(),
                           [&recordingId](const RecordingMetadata &m)
                           { return m.id == recordingId; });

    if (it == m_recordings.end())
    {
        return false;
    }

    // Get old file path
    std::string oldPath = it->filepath;
    fs::path oldFilePath(oldPath);

    // Create new file path with new name
    fs::path newFilePath = oldFilePath.parent_path() / newName;

    // If new name doesn't have extension, preserve old extension
    if (newFilePath.extension().empty() && !oldFilePath.extension().empty())
    {
        newFilePath.replace_extension(oldFilePath.extension());
    }

    // Rename file
    try
    {
        if (fs::exists(oldPath))
        {
            fs::rename(oldPath, newFilePath);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("RecordingManager: Failed to rename file: " + std::string(e.what()));
        return false;
    }

    // Update metadata
    it->filename = fs_helper::get_filename_string(newFilePath);
    it->filepath = fs::absolute(newFilePath).string();

    // Save metadata
    return saveRecordingsMetadata();
}

void RecordingManager::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_audioSampleRate = sampleRate;
    m_audioChannels = channels;
}
