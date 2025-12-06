#include "AudioCaptureWASAPI.h"
#include "../utils/Logger.h"
#include <comdef.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32

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

// Helper macro for COM error checking
#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) { \
        _com_error err(hr); \
        LOG_ERROR(msg + std::string(": ") + err.ErrorMessage()); \
        return false; \
    }

AudioCaptureWASAPI::AudioCaptureWASAPI()
    : m_deviceEnumerator(nullptr)
    , m_device(nullptr)
    , m_audioClient(nullptr)
    , m_captureClient(nullptr)
    , m_endpointVolume(nullptr)
    , m_sampleRate(44100)
    , m_channels(2)
    , m_bytesPerSample(2) // 16-bit
    , m_waveFormat(nullptr)
    , m_isOpen(false)
    , m_isCapturing(false)
    , m_captureThreadRunning(false)
{
    if (!initializeCOM())
    {
        LOG_ERROR("Falha ao inicializar COM");
    }
}

AudioCaptureWASAPI::~AudioCaptureWASAPI()
{
    close();
    shutdownCOM();
}

bool AudioCaptureWASAPI::initializeCOM()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_ERROR("Falha ao inicializar COM");
        return false;
    }
    return true;
}

void AudioCaptureWASAPI::shutdownCOM()
{
    CoUninitialize();
}

bool AudioCaptureWASAPI::createDeviceEnumerator()
{
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void **)&m_deviceEnumerator);

    CHECK_HR(hr, "Falha ao criar Device Enumerator");
    return true;
}

bool AudioCaptureWASAPI::selectDevice(const std::string &deviceName)
{
    if (!m_deviceEnumerator)
    {
        if (!createDeviceEnumerator())
        {
            return false;
        }
    }

    HRESULT hr = S_OK;

    // If deviceName is empty or "default", use default capture device
    if (deviceName.empty() || deviceName == "default")
    {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
            eCapture,
            eConsole,
            &m_device);
        CHECK_HR(hr, "Falha ao obter dispositivo padrão");
        return true;
    }

    // Enumerate devices and find matching one
    IMMDeviceCollection *deviceCollection = nullptr;
    hr = m_deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &deviceCollection);
    CHECK_HR(hr, "Falha ao enumerar dispositivos");

    UINT deviceCount = 0;
    hr = deviceCollection->GetCount(&deviceCount);
    CHECK_HR(hr, "Falha ao obter contagem de dispositivos");

    bool found = false;
    for (UINT i = 0; i < deviceCount; i++)
    {
        IMMDevice *device = nullptr;
        hr = deviceCollection->Item(i, &device);
        if (FAILED(hr))
        {
            continue;
        }

        // Get device ID
        LPWSTR deviceId = nullptr;
        hr = device->GetId(&deviceId);
        if (SUCCEEDED(hr))
        {
            char idBuffer[512];
            WideCharToMultiByte(CP_UTF8, 0, deviceId, -1, idBuffer, sizeof(idBuffer), nullptr, nullptr);
            std::string id(idBuffer);
            CoTaskMemFree(deviceId);

            // Get friendly name
            IPropertyStore *properties = nullptr;
            hr = device->OpenPropertyStore(STGM_READ, &properties);
            if (SUCCEEDED(hr))
            {
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);
                hr = properties->GetValue(PKEY_Device_FriendlyName, &friendlyName);
                if (SUCCEEDED(hr))
                {
                    char nameBuffer[512];
                    WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
                    std::string name(nameBuffer);

                    // Check if device matches
                    if (id == deviceName || name == deviceName || std::to_string(i) == deviceName)
                    {
                        m_device = device;
                        m_deviceId = id;
                        found = true;
                        PropVariantClear(&friendlyName);
                        SafeRelease(&properties);
                        break;
                    }
                    PropVariantClear(&friendlyName);
                }
                SafeRelease(&properties);
            }
        }

        if (!found)
        {
            SafeRelease(&device);
        }
    }

    SafeRelease(&deviceCollection);

    if (!found)
    {
        LOG_WARN("Dispositivo não encontrado: " + deviceName + ", usando dispositivo padrão");
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &m_device);
        CHECK_HR(hr, "Falha ao obter dispositivo padrão");
    }

    return true;
}

bool AudioCaptureWASAPI::initializeAudioClient()
{
    if (!m_device)
    {
        LOG_ERROR("Dispositivo não está disponível");
        return false;
    }

    HRESULT hr = S_OK;

    // Activate audio client
    hr = m_device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        (void **)&m_audioClient);
    CHECK_HR(hr, "Falha ao ativar Audio Client");

    // Get mix format
    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    CHECK_HR(hr, "Falha ao obter formato de mix");

    // Update format info
    m_sampleRate = m_waveFormat->nSamplesPerSec;
    m_channels = m_waveFormat->nChannels;
    m_bytesPerSample = m_waveFormat->wBitsPerSample / 8;

    // Initialize audio client
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        0, // Buffer duration (0 = default)
        0, // Period (0 = default)
        m_waveFormat,
        nullptr);
    CHECK_HR(hr, "Falha ao inicializar Audio Client");

    // Get capture client
    hr = m_audioClient->GetService(
        __uuidof(IAudioCaptureClient),
        (void **)&m_captureClient);
    CHECK_HR(hr, "Falha ao obter Capture Client");

    LOG_INFO("Audio Client inicializado: " + std::to_string(m_sampleRate) + "Hz, " +
             std::to_string(m_channels) + " canais, " +
             std::to_string(m_bytesPerSample * 8) + " bits");

    return true;
}

bool AudioCaptureWASAPI::open(const std::string &deviceName)
{
    if (m_isOpen)
    {
        LOG_WARN("AudioCapture já está aberto");
        return true;
    }

    if (!createDeviceEnumerator())
    {
        return false;
    }

    if (!selectDevice(deviceName))
    {
        return false;
    }

    if (!initializeAudioClient())
    {
        SafeRelease(&m_device);
        return false;
    }

    m_isOpen = true;
    LOG_INFO("AudioCapture aberto");
    return true;
}

void AudioCaptureWASAPI::close()
{
    if (!m_isOpen)
    {
        return;
    }

    stopCapture();

    SafeRelease(&m_captureClient);
    SafeRelease(&m_audioClient);
    SafeRelease(&m_endpointVolume);
    SafeRelease(&m_device);
    SafeRelease(&m_deviceEnumerator);

    if (m_waveFormat)
    {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_audioBuffer.clear();
    }

    m_isOpen = false;
    LOG_INFO("AudioCapture fechado");
}

bool AudioCaptureWASAPI::isOpen() const
{
    return m_isOpen;
}

bool AudioCaptureWASAPI::startCapture()
{
    if (!m_isOpen)
    {
        LOG_ERROR("AudioCapture não está aberto");
        return false;
    }

    if (m_isCapturing)
    {
        return true;
    }

    if (!m_audioClient)
    {
        LOG_ERROR("Audio Client não está disponível");
        return false;
    }

    HRESULT hr = m_audioClient->Start();
    CHECK_HR(hr, "Falha ao iniciar captura");

    m_isCapturing = true;

    if (!startCaptureThread())
    {
        m_audioClient->Stop();
        m_isCapturing = false;
        return false;
    }

    LOG_INFO("AudioCapture iniciado");
    return true;
}

void AudioCaptureWASAPI::stopCapture()
{
    if (!m_isCapturing)
    {
        return;
    }

    stopCaptureThread();

    if (m_audioClient)
    {
        m_audioClient->Stop();
    }

    m_isCapturing = false;
    LOG_INFO("AudioCapture parado");
}

bool AudioCaptureWASAPI::startCaptureThread()
{
    if (m_captureThreadRunning)
    {
        return true;
    }

    m_captureThreadRunning = true;
    m_captureThread = std::thread(&AudioCaptureWASAPI::captureThreadFunction, this);
    return true;
}

void AudioCaptureWASAPI::stopCaptureThread()
{
    if (!m_captureThreadRunning)
    {
        return;
    }

    m_captureThreadRunning = false;
    if (m_captureThread.joinable())
    {
        m_captureThread.join();
    }
}

void AudioCaptureWASAPI::captureThreadFunction()
{
    while (m_captureThreadRunning && m_isCapturing)
    {
        if (!m_captureClient)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

        if (FAILED(hr))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        while (packetLength > 0)
        {
            BYTE *data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr))
            {
                if (framesAvailable > 0)
                {
                    processAudioData(data, framesAvailable);
                }

                m_captureClient->ReleaseBuffer(framesAvailable);
            }

            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr))
            {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AudioCaptureWASAPI::processAudioData(BYTE *data, UINT32 framesAvailable)
{
    if (!data || framesAvailable == 0)
    {
        return;
    }

    size_t samples = framesAvailable * m_channels;
    const int16_t *sampleData = reinterpret_cast<const int16_t *>(data);

    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        size_t oldSize = m_audioBuffer.size();
        m_audioBuffer.resize(oldSize + samples);
        std::memcpy(m_audioBuffer.data() + oldSize, sampleData, samples * sizeof(int16_t));
    }

    if (m_audioCallback)
    {
        m_audioCallback(sampleData, samples);
    }
}

size_t AudioCaptureWASAPI::getSamples(std::vector<float> &samples)
{
    if (!m_isOpen)
    {
        samples.clear();
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_audioBuffer.empty())
    {
        samples.clear();
        return 0;
    }

    size_t numSamples = m_audioBuffer.size();
    samples.resize(numSamples);

    // Convert from int16_t (-32768 to 32767) to float (-1.0 to 1.0)
    for (size_t i = 0; i < numSamples; ++i)
    {
        samples[i] = static_cast<float>(m_audioBuffer[i]) / 32768.0f;
    }

    m_audioBuffer.clear();
    return numSamples;
}

size_t AudioCaptureWASAPI::getSamples(int16_t *buffer, size_t maxSamples)
{
    if (!m_isOpen || !buffer || maxSamples == 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_audioBuffer.empty())
    {
        return 0;
    }

    size_t samplesToCopy = std::min(maxSamples, m_audioBuffer.size());
    if (samplesToCopy > 0)
    {
        std::memcpy(buffer, m_audioBuffer.data(), samplesToCopy * sizeof(int16_t));
        m_audioBuffer.erase(m_audioBuffer.begin(),
                           m_audioBuffer.begin() + samplesToCopy);
    }

    return samplesToCopy;
}

uint32_t AudioCaptureWASAPI::getSampleRate() const
{
    return m_sampleRate;
}

uint32_t AudioCaptureWASAPI::getChannels() const
{
    return m_channels;
}

uint32_t AudioCaptureWASAPI::getBytesPerSample() const
{
    return m_bytesPerSample;
}

std::vector<AudioDeviceInfo> AudioCaptureWASAPI::listDevices()
{
    std::vector<AudioDeviceInfo> devices;

    if (!createDeviceEnumerator())
    {
        return devices;
    }

    IMMDeviceCollection *deviceCollection = nullptr;
    HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (FAILED(hr))
    {
        return devices;
    }

    UINT deviceCount = 0;
    hr = deviceCollection->GetCount(&deviceCount);
    if (FAILED(hr))
    {
        SafeRelease(&deviceCollection);
        return devices;
    }

    for (UINT i = 0; i < deviceCount; i++)
    {
        IMMDevice *device = nullptr;
        hr = deviceCollection->Item(i, &device);
        if (FAILED(hr))
        {
            continue;
        }

        AudioDeviceInfo info;

        // Get device ID
        LPWSTR deviceId = nullptr;
        hr = device->GetId(&deviceId);
        if (SUCCEEDED(hr))
        {
            char idBuffer[512];
            WideCharToMultiByte(CP_UTF8, 0, deviceId, -1, idBuffer, sizeof(idBuffer), nullptr, nullptr);
            info.id = std::string(idBuffer);
            CoTaskMemFree(deviceId);
        }
        else
        {
            info.id = std::to_string(i);
        }

        // Get friendly name
        IPropertyStore *properties = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(hr))
        {
            PROPVARIANT friendlyName;
            PropVariantInit(&friendlyName);
            hr = properties->GetValue(PKEY_Device_FriendlyName, &friendlyName);
            if (SUCCEEDED(hr))
            {
                char nameBuffer[512];
                WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, nameBuffer, sizeof(nameBuffer), nullptr, nullptr);
                info.name = std::string(nameBuffer);
                PropVariantClear(&friendlyName);
            }
            else
            {
                info.name = "Dispositivo " + std::to_string(i);
            }
            SafeRelease(&properties);
        }
        else
        {
            info.name = "Dispositivo " + std::to_string(i);
        }

        info.available = true;
        devices.push_back(info);
        SafeRelease(&device);
    }

    SafeRelease(&deviceCollection);
    return devices;
}

void AudioCaptureWASAPI::setDeviceStateCallback(std::function<void(const std::string &, bool)> callback)
{
    m_deviceStateCallback = callback;
}

void AudioCaptureWASAPI::convertToFloat(const int16_t *input, float *output, size_t samples)
{
    for (size_t i = 0; i < samples; ++i)
    {
        output[i] = static_cast<float>(input[i]) / 32768.0f;
    }
}

#endif // _WIN32

