#include "FileRecorder.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include <ctime>
#include <algorithm>

FileRecorder::FileRecorder()
{
}

FileRecorder::~FileRecorder()
{
    cleanup();
}

bool FileRecorder::ensureOutputDirectory(const std::string& path)
{
    try
    {
        fs::path filePath(path);
        fs::path dirPath = filePath.parent_path();
        
        if (!dirPath.empty() && !fs::exists(dirPath))
        {
            if (!fs::create_directories(dirPath))
            {
                LOG_ERROR("FileRecorder: Failed to create output directory: " + dirPath.string());
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("FileRecorder: Exception creating directory: " + std::string(e.what()));
        return false;
    }
}

int FileRecorder::writeToFile(const uint8_t* data, size_t size)
{
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (!m_outputFile.is_open())
    {
        return -1;
    }

    try
    {
        m_outputFile.write(reinterpret_cast<const char*>(data), size);
        if (m_outputFile.fail())
        {
            LOG_ERROR("FileRecorder: Failed to write to file");
            return -1;
        }
        m_outputFile.flush();
        return static_cast<int>(size);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("FileRecorder: Exception writing to file: " + std::string(e.what()));
        return -1;
    }
}

bool FileRecorder::initialize(const MediaEncoder::VideoConfig& videoConfig,
                               const MediaEncoder::AudioConfig& audioConfig,
                               void* videoCodecContext,
                               void* audioCodecContext,
                               const std::string& outputPath)
{
    if (m_initialized)
    {
        cleanup();
    }

    if (!videoCodecContext || !audioCodecContext)
    {
        LOG_ERROR("FileRecorder: Codec contexts must be provided");
        return false;
    }

    m_outputPath = outputPath;
    m_videoCodecContext = videoCodecContext;
    m_audioCodecContext = audioCodecContext;

    // Ensure output directory exists
    if (!ensureOutputDirectory(outputPath))
    {
        return false;
    }

    // Create write callback
    auto writeCallback = [this](const uint8_t* data, size_t size) -> int
    {
        return this->writeToFile(data, size);
    };

    // Initialize MediaMuxer with file callback
    // Use larger buffer for file I/O (512KB)
    if (!m_muxer.initialize(videoConfig, audioConfig,
                            videoCodecContext, audioCodecContext,
                            writeCallback, 512 * 1024))
    {
        LOG_ERROR("FileRecorder: Failed to initialize MediaMuxer");
        return false;
    }

    m_initialized = true;
    return true;
}

bool FileRecorder::startRecording()
{
    if (!m_initialized)
    {
        LOG_ERROR("FileRecorder: Not initialized");
        return false;
    }

    if (m_recording)
    {
        LOG_WARN("FileRecorder: Already recording");
        return true;
    }

    // Open output file
    {
        std::lock_guard<std::mutex> lock(m_fileMutex);
        m_outputFile.open(m_outputPath, std::ios::binary | std::ios::out);
        if (!m_outputFile.is_open())
        {
            LOG_ERROR("FileRecorder: Failed to open output file: " + m_outputPath);
            return false;
        }
    }

    // Get start timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    m_startTimestampUs = static_cast<int64_t>(ts.tv_sec) * 1000000LL + 
                         static_cast<int64_t>(ts.tv_nsec) / 1000LL;
    m_durationUs = 0;

    m_recording = true;
    LOG_INFO("FileRecorder: Started recording to: " + m_outputPath);
    return true;
}

void FileRecorder::stopRecording()
{
    if (!m_recording)
    {
        return;
    }

    // Flush pending packets
    flush();

    // Close file
    {
        std::lock_guard<std::mutex> lock(m_fileMutex);
        if (m_outputFile.is_open())
        {
            m_outputFile.close();
        }
    }

    m_recording = false;
    LOG_INFO("FileRecorder: Stopped recording. Duration: " + 
             std::to_string(m_durationUs / 1000000) + " seconds");
}

bool FileRecorder::muxPacket(const MediaEncoder::EncodedPacket& packet)
{
    if (!m_recording || !m_initialized)
    {
        return false;
    }

    // Update duration based on packet timestamp
    if (packet.captureTimestampUs > 0 && m_startTimestampUs > 0)
    {
        m_durationUs = packet.captureTimestampUs - m_startTimestampUs;
    }

    return m_muxer.muxPacket(packet);
}

void FileRecorder::flush()
{
    if (m_initialized)
    {
        m_muxer.flush();
    }
}

void FileRecorder::cleanup()
{
    stopRecording();

    if (m_initialized)
    {
        m_muxer.cleanup();
        m_initialized = false;
    }

    m_outputPath.clear();
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;
    m_durationUs = 0;
    m_startTimestampUs = 0;
}

uint64_t FileRecorder::getFileSize() const
{
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (!m_outputFile.is_open())
    {
        return 0;
    }

    try
    {
        // Get current position (file size)
        std::streampos pos = m_outputFile.tellp();
        if (pos == std::streampos(-1))
        {
            // Try to get file size from filesystem
            if (fs::exists(m_outputPath))
            {
                return static_cast<uint64_t>(fs::file_size(m_outputPath));
            }
            return 0;
        }
        return static_cast<uint64_t>(pos);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("FileRecorder: Exception getting file size: " + std::string(e.what()));
        return 0;
    }
}
