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
    
    // During cleanup, av_write_trailer() may call this after file is closed
    // Return success to avoid FFmpeg errors, but don't try to write
    if (!m_outputFile.is_open())
    {
        // File closed - return success to avoid FFmpeg errors during cleanup
        return static_cast<int>(size);
    }

    try
    {
        m_outputFile.write(reinterpret_cast<const char*>(data), size);
        if (m_outputFile.fail())
        {
            LOG_ERROR("FileRecorder: Failed to write to file");
            return -1;
        }
        // Don't flush on every write - only flush when stopping/cleaning up
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

    // Flush muxer first to ensure all data is written (file must be open)
    if (m_initialized)
    {
        m_muxer.flush();
    }

    // Set flag to prevent new packets
    // IMPORTANT: Keep file open - cleanup() needs it for av_write_trailer()
    m_recording = false;

    // Flush file buffer but keep file open for cleanup()
    {
        std::lock_guard<std::mutex> lock(m_fileMutex);
        if (m_outputFile.is_open())
        {
            m_outputFile.flush(); // Ensure all data is written to disk
            // Don't close here - cleanup() will close after av_write_trailer()
        }
    }

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
        std::lock_guard<std::mutex> lock(m_fileMutex);
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
    LOG_INFO("FileRecorder: Starting cleanup");
    
    // Stop recording first (flushes but keeps file open for av_write_trailer)
    if (m_recording)
    {
        LOG_INFO("FileRecorder: Flushing muxer before cleanup");
        // Flush muxer while file is still open
        if (m_initialized)
        {
            m_muxer.flush();
        }
        m_recording = false;
    }

    // Cleanup muxer (av_write_trailer may write to file, so file must be open)
    // After cleanup, close the file
    if (m_initialized)
    {
        LOG_INFO("FileRecorder: Ensuring file is open for av_write_trailer");
        {
            std::lock_guard<std::mutex> lock(m_fileMutex);
            // Ensure file is open for av_write_trailer
            if (!m_outputFile.is_open() && !m_outputPath.empty())
            {
                LOG_INFO("FileRecorder: Reopening file for cleanup");
                m_outputFile.open(m_outputPath, std::ios::binary | std::ios::out | std::ios::app);
            }
        }
        
        LOG_INFO("FileRecorder: Calling muxer.cleanup() (will call av_write_trailer)");
        // Cleanup muxer (this calls av_write_trailer which may write to file)
        m_muxer.cleanup();
        m_initialized = false;
        
        LOG_INFO("FileRecorder: Closing file after muxer cleanup");
        // Now close the file
        {
            std::lock_guard<std::mutex> lock(m_fileMutex);
            if (m_outputFile.is_open())
            {
                m_outputFile.flush();
                m_outputFile.close();
                LOG_INFO("FileRecorder: File closed successfully");
            }
        }
    }

    // Clear state
    m_outputPath.clear();
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;
    m_durationUs = 0;
    m_startTimestampUs = 0;
    
    LOG_INFO("FileRecorder: Cleanup completed");
}

uint64_t FileRecorder::getFileSize()
{
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (!m_outputFile.is_open())
    {
        return 0;
    }

    try
    {
        // Try to get file size from filesystem (more reliable)
        if (fs::exists(m_outputPath))
        {
            return static_cast<uint64_t>(fs::file_size(m_outputPath));
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("FileRecorder: Exception getting file size: " + std::string(e.what()));
        return 0;
    }
}
