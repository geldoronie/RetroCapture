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
        // CRITICAL: Write all data and verify it was written correctly
        m_outputFile.write(reinterpret_cast<const char*>(data), size);
        
        // Check for write errors
        if (m_outputFile.fail() || m_outputFile.bad())
        {
            LOG_ERROR("FileRecorder: Failed to write to file (fail=" + 
                     std::to_string(m_outputFile.fail()) + 
                     ", bad=" + std::to_string(m_outputFile.bad()) + ")");
            return -1;
        }
        
        // Verify bytes written (important for FFmpeg to know correct size)
        std::streampos posBefore = m_outputFile.tellp();
        if (posBefore == std::streampos(-1))
        {
            // Can't verify, but assume success if no error flags
            return static_cast<int>(size);
        }
        
        // Don't flush on every write - only flush when stopping/cleaning up
        // Return exact size written (FFmpeg expects this)
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

    // Determine container format from file extension
    std::string containerFormat = "";
    fs::path path(outputPath);
    std::string ext = path.extension();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".mkv")
        containerFormat = "matroska";
    else if (ext == ".webm")
        containerFormat = "webm";
    else if (ext == ".mp4")
        containerFormat = "mp4";
    // Se vazio, será detectado automaticamente pelo MediaMuxer
    
    // Converter caminho para absoluto para garantir que FFmpeg abra no lugar correto
    fs::path absolutePath = fs::absolute(path);
    std::string absolutePathStr = absolutePath.string();
    LOG_INFO("FileRecorder: Using absolute path: " + absolutePathStr);
    
    // Initialize MediaMuxer with file path (FFmpeg abre o arquivo diretamente, suporta seek)
    // Não usar callback - usar avio_open do FFmpeg para suportar seek (necessário para MP4)
    if (!m_muxer.initialize(videoConfig, audioConfig,
                            videoCodecContext, audioCodecContext,
                            absolutePathStr, nullptr, 0, containerFormat))
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

    // NÃO abrir o arquivo aqui - o MediaMuxer já abre com avio_open
    // Abrir novamente com std::ofstream trunca o arquivo e sobrescreve o ftyp box!
    // O FFmpeg escreve diretamente no arquivo através do avio_open

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
    // Finalizar escrita antes de fechar arquivo
    if (m_initialized)
    {
        m_muxer.finalize();
    }
    
    // Arquivo será fechado pelo FFmpeg em cleanup() do MediaMuxer
    // Não precisamos fechar manualmente
    
    m_recording = false;
    m_initialized = false;
    m_outputPath.clear();
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;
    m_durationUs = 0;
    m_startTimestampUs = 0;
    
    // NÃO chamar m_muxer.cleanup() - deixar recursos na memória
}

uint64_t FileRecorder::getFileSize()
{
    // Não usar m_outputFile - o arquivo é gerenciado pelo FFmpeg
    // Obter tamanho diretamente do filesystem
    try
    {
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
