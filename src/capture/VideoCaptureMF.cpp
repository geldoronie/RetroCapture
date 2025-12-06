#include "VideoCaptureMF.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cstring>
#include <comdef.h>
#include <propkey.h>
#include <propvarutil.h>

#ifdef _WIN32

// Helper macro for COM error checking
#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) { \
        _com_error err(hr); \
        LOG_ERROR(msg + std::string(": ") + err.ErrorMessage()); \
        return false; \
    }

// Helper to release COM objects
template<typename T>
void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

VideoCaptureMF::VideoCaptureMF()
    : m_mediaSource(nullptr)
    , m_sourceReader(nullptr)
    , m_mediaType(nullptr)
    , m_width(0)
    , m_height(0)
    , m_fps(30)
    , m_pixelFormat(MFVideoFormat_RGB24)
    , m_isOpen(false)
    , m_streaming(false)
    , m_dummyMode(false)
    , m_hasFrame(false)
{
    // Initialize Media Foundation
    if (!initializeMediaFoundation())
    {
        LOG_ERROR("Falha ao inicializar Media Foundation");
    }
}

VideoCaptureMF::~VideoCaptureMF()
{
    close();
    shutdownMediaFoundation();
}

bool VideoCaptureMF::initializeMediaFoundation()
{
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        LOG_ERROR("Falha ao inicializar Media Foundation");
        return false;
    }
    return true;
}

void VideoCaptureMF::shutdownMediaFoundation()
{
    MFShutdown();
}

bool VideoCaptureMF::open(const std::string &device)
{
    if (m_isOpen)
    {
        LOG_WARN("Dispositivo já aberto, fechando primeiro");
        close();
    }

    m_deviceId = device;

    // Em modo dummy, apenas marcar como aberto
    if (m_dummyMode)
    {
        m_isOpen = true;
        LOG_INFO("Modo dummy ativado para Windows");
        return true;
    }

    if (!createMediaSource(device))
    {
        LOG_ERROR("Falha ao criar Media Source para dispositivo: " + device);
        return false;
    }

    if (!configureSourceReader())
    {
        LOG_ERROR("Falha ao configurar Source Reader");
        SafeRelease(&m_mediaSource);
        return false;
    }

    m_isOpen = true;
    LOG_INFO("Dispositivo aberto: " + device);
    return true;
}

void VideoCaptureMF::close()
{
    if (!m_isOpen)
    {
        return;
    }

    stopCapture();

    SafeRelease(&m_sourceReader);
    SafeRelease(&m_mediaSource);
    SafeRelease(&m_mediaType);

    m_isOpen = false;
    m_hasFrame = false;
    m_frameBuffer.clear();

    if (!m_dummyMode)
    {
        m_dummyFrameBuffer.clear();
    }

    LOG_INFO("Dispositivo fechado");
}

bool VideoCaptureMF::isOpen() const
{
    return m_isOpen || m_dummyMode;
}

bool VideoCaptureMF::createMediaSource(const std::string &deviceId)
{
    HRESULT hr = S_OK;
    IMFAttributes *attributes = nullptr;
    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;

    // Create attributes for video capture devices
    hr = MFCreateAttributes(&attributes, 1);
    CHECK_HR(hr, "Falha ao criar atributos");

    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    CHECK_HR(hr, "Falha ao definir tipo de dispositivo");

    // Enumerate video capture devices
    hr = MFEnumDeviceSources(attributes, &devices, &deviceCount);
    CHECK_HR(hr, "Falha ao enumerar dispositivos");

    if (deviceCount == 0)
    {
        LOG_ERROR("Nenhum dispositivo de captura encontrado");
        SafeRelease(&attributes);
        return false;
    }

    // If deviceId is empty or "default", use first device
    // Otherwise, try to find device by ID or name
    UINT32 selectedIndex = 0;
    if (!deviceId.empty() && deviceId != "default")
    {
        bool found = false;
        for (UINT32 i = 0; i < deviceCount; i++)
        {
            WCHAR *friendlyName = nullptr;
            UINT32 nameLength = 0;
            hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength);
            
            if (SUCCEEDED(hr))
            {
                // Convert WCHAR to string for comparison
                char nameBuffer[512];
                WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
                std::string name(nameBuffer);

                // Check if device ID or name matches
                if (name == deviceId || std::to_string(i) == deviceId)
                {
                    selectedIndex = i;
                    found = true;
                    CoTaskMemFree(friendlyName);
                    break;
                }
                CoTaskMemFree(friendlyName);
            }
        }

        if (!found)
        {
            LOG_WARN("Dispositivo não encontrado: " + deviceId + ", usando primeiro dispositivo disponível");
        }
    }

    // Activate the selected device
    hr = devices[selectedIndex]->ActivateObject(IID_PPV_ARGS(&m_mediaSource));
    CHECK_HR(hr, "Falha ao ativar dispositivo");

    // Cleanup
    for (UINT32 i = 0; i < deviceCount; i++)
    {
        SafeRelease(&devices[i]);
    }
    CoTaskMemFree(devices);
    SafeRelease(&attributes);

    return true;
}

bool VideoCaptureMF::configureSourceReader()
{
    if (!m_mediaSource)
    {
        LOG_ERROR("Media Source não está disponível");
        return false;
    }

    HRESULT hr = S_OK;

    // Create source reader
    IMFAttributes *attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 2);
    CHECK_HR(hr, "Falha ao criar atributos do Source Reader");

    // Enable async mode
    hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    CHECK_HR(hr, "Falha ao configurar hardware transforms");

    hr = MFCreateSourceReaderFromMediaSource(m_mediaSource, attributes, &m_sourceReader);
    CHECK_HR(hr, "Falha ao criar Source Reader");

    SafeRelease(&attributes);

    // Set media type (RGB24 by default)
    if (m_width > 0 && m_height > 0)
    {
        if (!setFormat(m_width, m_height, 0))
        {
            LOG_WARN("Falha ao configurar formato, usando formato padrão do dispositivo");
        }
    }

    return true;
}

bool VideoCaptureMF::setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
    if (m_dummyMode)
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = getPixelFormatGUID(pixelFormat);

        // Create dummy buffer (RGB24: 3 bytes per pixel)
        size_t frameSize = width * height * 3;
        m_dummyFrameBuffer.resize(frameSize, 0);

        LOG_INFO("Formato dummy definido: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    if (!m_sourceReader)
    {
        LOG_ERROR("Source Reader não está disponível");
        return false;
    }

    HRESULT hr = S_OK;
    IMFMediaType *mediaType = nullptr;

    // Create media type
    hr = MFCreateMediaType(&mediaType);
    CHECK_HR(hr, "Falha ao criar Media Type");

    // Set major type: video
    hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    CHECK_HR(hr, "Falha ao definir major type");

    // Set subtype: RGB24
    GUID formatGuid = getPixelFormatGUID(pixelFormat);
    hr = mediaType->SetGUID(MF_MT_SUBTYPE, formatGuid);
    CHECK_HR(hr, "Falha ao definir subtype");

    // Set frame size
    hr = MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, width, height);
    CHECK_HR(hr, "Falha ao definir tamanho do frame");

    // Set frame rate
    if (m_fps > 0)
    {
        hr = MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, m_fps, 1);
        CHECK_HR(hr, "Falha ao definir frame rate");
    }

    // Set the media type on the source reader
    hr = m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType);
    if (FAILED(hr))
    {
        LOG_WARN("Falha ao definir Media Type no Source Reader, tentando formato nativo");
        SafeRelease(&mediaType);
        
        // Try to get native format
        hr = m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mediaType);
        if (SUCCEEDED(hr))
        {
            // Get actual format
            UINT32 actualWidth = 0, actualHeight = 0;
            hr = MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &actualWidth, &actualHeight);
            if (SUCCEEDED(hr))
            {
                m_width = actualWidth;
                m_height = actualHeight;
                LOG_INFO("Usando formato nativo do dispositivo: " + std::to_string(m_width) + "x" + std::to_string(m_height));
            }
        }
    }
    else
    {
        m_width = width;
        m_height = height;
        m_pixelFormat = formatGuid;
        LOG_INFO("Formato definido: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    }

    SafeRelease(&mediaType);
    return true;
}

bool VideoCaptureMF::setFramerate(uint32_t fps)
{
    if (m_dummyMode)
    {
        m_fps = fps;
        LOG_INFO("Framerate dummy configurado: " + std::to_string(fps) + "fps");
        return true;
    }

    m_fps = fps;

    // Reconfigure format with new framerate
    if (m_width > 0 && m_height > 0)
    {
        return setFormat(m_width, m_height, getPixelFormatFromGUID(m_pixelFormat));
    }

    return true;
}

bool VideoCaptureMF::startCapture()
{
    if (m_dummyMode)
    {
        if (m_streaming)
        {
            return true;
        }

        if (m_dummyFrameBuffer.empty() && m_width > 0 && m_height > 0)
        {
            size_t frameSize = m_width * m_height * 3; // RGB24: 3 bytes per pixel
            m_dummyFrameBuffer.resize(frameSize, 0);
        }

        m_streaming = true;
        LOG_INFO("Captura dummy iniciada: " + std::to_string(m_width) + "x" + std::to_string(m_height));
        return true;
    }

    if (!m_sourceReader)
    {
        LOG_ERROR("Source Reader não está disponível");
        return false;
    }

    if (m_streaming)
    {
        return true;
    }

    m_streaming = true;
    m_hasFrame = false;
    LOG_INFO("Captura iniciada");
    return true;
}

void VideoCaptureMF::stopCapture()
{
    if (!m_streaming)
    {
        return;
    }

    if (m_dummyMode)
    {
        m_streaming = false;
        LOG_INFO("Captura dummy parada");
        return;
    }

    m_streaming = false;
    m_hasFrame = false;
    LOG_INFO("Captura parada");
}

bool VideoCaptureMF::captureFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming || m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        return true;
    }

    if (!m_sourceReader || !m_streaming)
    {
        return false;
    }

    return readSample(frame);
}

bool VideoCaptureMF::captureLatestFrame(Frame &frame)
{
    if (m_dummyMode)
    {
        if (!m_streaming || m_dummyFrameBuffer.empty())
        {
            return false;
        }
        generateDummyFrame(frame);
        return true;
    }

    if (!m_sourceReader || !m_streaming)
    {
        return false;
    }

    // Read all available frames and keep the latest
    Frame tempFrame;
    bool gotFrame = false;

    while (readSample(tempFrame))
    {
        frame = tempFrame;
        gotFrame = true;
    }

    return gotFrame;
}

bool VideoCaptureMF::readSample(Frame &frame)
{
    if (!m_sourceReader)
    {
        return false;
    }

    HRESULT hr = S_OK;
    IMFSample *sample = nullptr;
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;

    // Read sample
    hr = m_sourceReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0,
        nullptr,
        &streamFlags,
        &timestamp,
        &sample);

    if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_STREAMTICK))
    {
        return false;
    }

    if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
    {
        return false;
    }

    if (!sample)
    {
        return false;
    }

    // Convert sample to buffer
    IMFMediaBuffer *buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr))
    {
        SafeRelease(&sample);
        return false;
    }

    // Lock buffer
    BYTE *data = nullptr;
    DWORD dataLength = 0;
    hr = buffer->Lock(&data, nullptr, &dataLength);
    if (FAILED(hr))
    {
        SafeRelease(&buffer);
        SafeRelease(&sample);
        return false;
    }

    // Copy frame data
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_frameBuffer.resize(dataLength);
        std::memcpy(m_frameBuffer.data(), data, dataLength);
    }

    // Fill frame structure
    frame.data = m_frameBuffer.data();
    frame.size = dataLength;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = getPixelFormatFromGUID(m_pixelFormat);

    // Unlock and release
    buffer->Unlock();
    SafeRelease(&buffer);
    SafeRelease(&sample);

    m_hasFrame = true;
    m_latestFrame = frame;

    return true;
}

void VideoCaptureMF::generateDummyFrame(Frame &frame)
{
    if (m_dummyFrameBuffer.empty() || m_width == 0 || m_height == 0)
    {
        return;
    }

    frame.data = m_dummyFrameBuffer.data();
    frame.size = m_dummyFrameBuffer.size();
    frame.width = m_width;
    frame.height = m_height;
    frame.format = getPixelFormatFromGUID(m_pixelFormat);
}

uint32_t VideoCaptureMF::getPixelFormat() const
{
    return getPixelFormatFromGUID(m_pixelFormat);
}

GUID VideoCaptureMF::getPixelFormatGUID(uint32_t pixelFormat)
{
    // Map pixel format codes to Media Foundation GUIDs
    // Default to RGB24 if unknown
    switch (pixelFormat)
    {
    case 0: // Default
    default:
        return MFVideoFormat_RGB24;
    }
}

uint32_t VideoCaptureMF::getPixelFormatFromGUID(const GUID &guid)
{
    // Map Media Foundation GUIDs to format codes
    if (guid == MFVideoFormat_RGB24)
    {
        return 0; // RGB24
    }
    return 0; // Default
}

bool VideoCaptureMF::setControl(const std::string &controlName, int32_t value)
{
    if (m_dummyMode)
    {
        return true; // Silently succeed in dummy mode
    }

    return setControlMF(controlName, value);
}

bool VideoCaptureMF::getControl(const std::string &controlName, int32_t &value)
{
    if (m_dummyMode)
    {
        return false; // No controls in dummy mode
    }

    return getControlMF(controlName, value);
}

bool VideoCaptureMF::getControlMin(const std::string &controlName, int32_t &minValue)
{
    // Media Foundation doesn't directly expose min/max for all controls
    // This would require IAMCameraControl or IAMVideoProcAmp from DirectShow
    // For now, return false
    return false;
}

bool VideoCaptureMF::getControlMax(const std::string &controlName, int32_t &maxValue)
{
    // Media Foundation doesn't directly expose min/max for all controls
    return false;
}

bool VideoCaptureMF::getControlDefault(const std::string &controlName, int32_t &defaultValue)
{
    // Media Foundation doesn't directly expose default values
    return false;
}

std::string VideoCaptureMF::getControlNameFromMF(const std::string &controlName)
{
    // Map control names to Media Foundation property keys
    // This is a simplified mapping - actual implementation would need
    // to use IAMCameraControl/IAMVideoProcAmp from DirectShow
    if (controlName == "brightness")
    {
        return "Brightness";
    }
    else if (controlName == "contrast")
    {
        return "Contrast";
    }
    else if (controlName == "saturation")
    {
        return "Saturation";
    }
    // Add more mappings as needed
    return controlName;
}

bool VideoCaptureMF::setControlMF(const std::string &controlName, int32_t value)
{
    // Media Foundation doesn't directly support camera controls
    // This would require using DirectShow interfaces (IAMCameraControl, IAMVideoProcAmp)
    // For now, return false - this is a limitation of the Media Foundation approach
    LOG_WARN("Controles de hardware não suportados via Media Foundation: " + controlName);
    return false;
}

bool VideoCaptureMF::getControlMF(const std::string &controlName, int32_t &value)
{
    // Media Foundation doesn't directly support camera controls
    LOG_WARN("Controles de hardware não suportados via Media Foundation: " + controlName);
    return false;
}

std::vector<DeviceInfo> VideoCaptureMF::listDevices()
{
    std::vector<DeviceInfo> devices;

    HRESULT hr = S_OK;
    IMFAttributes *attributes = nullptr;
    IMFActivate **deviceList = nullptr;
    UINT32 deviceCount = 0;

    // Create attributes
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr))
    {
        return devices;
    }

    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr))
    {
        SafeRelease(&attributes);
        return devices;
    }

    // Enumerate devices
    hr = MFEnumDeviceSources(attributes, &deviceList, &deviceCount);
    if (FAILED(hr) || deviceCount == 0)
    {
        SafeRelease(&attributes);
        return devices;
    }

    // Get device information
    for (UINT32 i = 0; i < deviceCount; i++)
    {
        DeviceInfo info;
        info.id = std::to_string(i);

        // Get friendly name
        WCHAR *friendlyName = nullptr;
        UINT32 nameLength = 0;
        hr = deviceList[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength);

        if (SUCCEEDED(hr))
        {
            char nameBuffer[512];
            WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
            info.name = std::string(nameBuffer);
            CoTaskMemFree(friendlyName);
        }
        else
        {
            info.name = "Dispositivo " + std::to_string(i);
        }

        info.available = true;
        devices.push_back(info);
    }

    // Cleanup
    for (UINT32 i = 0; i < deviceCount; i++)
    {
        SafeRelease(&deviceList[i]);
    }
    CoTaskMemFree(deviceList);
    SafeRelease(&attributes);

    return devices;
}

void VideoCaptureMF::setDummyMode(bool enabled)
{
    m_dummyMode = enabled;
}

bool VideoCaptureMF::isDummyMode() const
{
    return m_dummyMode;
}

#endif // _WIN32

