#include "AudioPlaybackWASAPI.h"

#ifdef _WIN32

#include "../utils/Logger.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <chrono>
#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace
{
    constexpr REFERENCE_TIME kBufferDurationHns = 50 * 10000; // 50 ms in 100-ns units
}

AudioPlaybackWASAPI::AudioPlaybackWASAPI() = default;

AudioPlaybackWASAPI::~AudioPlaybackWASAPI()
{
    close();
}

bool AudioPlaybackWASAPI::open(uint32_t sampleRate, uint32_t channels)
{
    if (m_audioClient) close();
    if (sampleRate == 0 || channels == 0 || channels > 8) return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE = already initialised on this thread; treat as success.
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_ERROR("AudioPlaybackWASAPI: CoInitializeEx failed");
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                          nullptr,
                          CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void **>(&m_enumerator));
    if (FAILED(hr) || !m_enumerator)
    {
        LOG_ERROR("AudioPlaybackWASAPI: MMDeviceEnumerator failed");
        return false;
    }

    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr) || !m_device)
    {
        LOG_ERROR("AudioPlaybackWASAPI: GetDefaultAudioEndpoint failed");
        return false;
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void **>(&m_audioClient));
    if (FAILED(hr) || !m_audioClient)
    {
        LOG_ERROR("AudioPlaybackWASAPI: IAudioClient activate failed");
        return false;
    }

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = static_cast<WORD>(channels);
    fmt.nSamplesPerSec  = sampleRate;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = static_cast<WORD>((fmt.wBitsPerSample / 8) * fmt.nChannels);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kBufferDurationHns,
        0,
        &fmt,
        nullptr);
    if (FAILED(hr))
    {
        LOG_ERROR("AudioPlaybackWASAPI: IAudioClient::Initialize failed");
        return false;
    }

    m_eventHandle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_eventHandle)
    {
        LOG_ERROR("AudioPlaybackWASAPI: CreateEvent failed");
        return false;
    }
    m_audioClient->SetEventHandle(static_cast<HANDLE>(m_eventHandle));

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr))
    {
        LOG_ERROR("AudioPlaybackWASAPI: GetBufferSize failed");
        return false;
    }
    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void **>(&m_renderClient));
    if (FAILED(hr) || !m_renderClient)
    {
        LOG_ERROR("AudioPlaybackWASAPI: GetService(IAudioRenderClient) failed");
        return false;
    }
    hr = m_audioClient->GetService(__uuidof(IAudioClock),
                                    reinterpret_cast<void **>(&m_audioClock));
    if (FAILED(hr) || !m_audioClock)
    {
        LOG_ERROR("AudioPlaybackWASAPI: GetService(IAudioClock) failed");
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels   = channels;

    // Prime the buffer with silence so Start() doesn't drop into
    // underrun before the decoder catches up.
    BYTE *priming = nullptr;
    if (SUCCEEDED(m_renderClient->GetBuffer(m_bufferFrameCount, &priming)))
    {
        m_renderClient->ReleaseBuffer(m_bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    m_audioClient->Start();

    m_running.store(true);
    m_renderThread = std::thread(&AudioPlaybackWASAPI::renderThreadFn, this);
    LOG_INFO("AudioPlaybackWASAPI: opened " + std::to_string(sampleRate) +
             " Hz x " + std::to_string(channels) + " ch");
    return true;
}

void AudioPlaybackWASAPI::close()
{
    m_running.store(false);
    if (m_eventHandle)
    {
        SetEvent(static_cast<HANDLE>(m_eventHandle));
    }
    if (m_renderThread.joinable()) m_renderThread.join();

    if (m_audioClient) m_audioClient->Stop();

    if (m_audioClock)   { m_audioClock->Release();   m_audioClock   = nullptr; }
    if (m_renderClient) { m_renderClient->Release(); m_renderClient = nullptr; }
    if (m_audioClient)  { m_audioClient->Release();  m_audioClient  = nullptr; }
    if (m_device)       { m_device->Release();       m_device       = nullptr; }
    if (m_enumerator)   { m_enumerator->Release();   m_enumerator   = nullptr; }
    if (m_eventHandle)  { CloseHandle(static_cast<HANDLE>(m_eventHandle)); m_eventHandle = nullptr; }

    m_sampleRate = m_channels = m_bufferFrameCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_submittedFrames     = 0;
    m_firstSubmittedPtsUs = 0;
    m_anySubmitted        = false;
}

size_t AudioPlaybackWASAPI::submit(const float *interleaved,
                                   size_t sampleCount,
                                   int64_t firstPtsUs)
{
    if (!m_audioClient || !interleaved || sampleCount == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const size_t floats = sampleCount * m_channels;
        m_queue.insert(m_queue.end(), interleaved, interleaved + floats);
    }
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        if (!m_anySubmitted)
        {
            m_firstSubmittedPtsUs = firstPtsUs;
            m_anySubmitted        = true;
        }
        m_submittedFrames += sampleCount;
    }
    return sampleCount;
}

int64_t AudioPlaybackWASAPI::getClockUs() const
{
    if (!m_audioClock) return 0;
    std::lock_guard<std::mutex> lock(m_clockMutex);
    if (!m_anySubmitted) return 0;

    UINT64 position = 0, frequency = 0;
    HRESULT hr1 = m_audioClock->GetPosition(&position, nullptr);
    HRESULT hr2 = m_audioClock->GetFrequency(&frequency);
    if (FAILED(hr1) || FAILED(hr2) || frequency == 0) return m_firstSubmittedPtsUs;

    // position / frequency = seconds played since the stream started.
    const int64_t playedUs = static_cast<int64_t>((position * 1'000'000ULL) / frequency);
    return m_firstSubmittedPtsUs + playedUs;
}

void AudioPlaybackWASAPI::flush()
{
    if (!m_audioClient) return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_submittedFrames     = 0;
    m_firstSubmittedPtsUs = 0;
    m_anySubmitted        = false;
}

void AudioPlaybackWASAPI::renderThreadFn()
{
    // Bump priority so we don't underrun under heavy CPU load.
    DWORD taskIndex = 0;
    HANDLE mmHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);

    while (m_running.load())
    {
        DWORD wait = WaitForSingleObject(static_cast<HANDLE>(m_eventHandle), 100);
        if (!m_running.load()) break;
        if (wait != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(m_audioClient->GetCurrentPadding(&padding))) continue;
        const UINT32 framesAvail = m_bufferFrameCount - padding;
        if (framesAvail == 0) continue;

        BYTE *dst = nullptr;
        if (FAILED(m_renderClient->GetBuffer(framesAvail, &dst))) continue;

        size_t floatsNeeded = static_cast<size_t>(framesAvail) * m_channels;
        size_t floatsCopied = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            floatsCopied = std::min(floatsNeeded, m_queue.size());
            if (floatsCopied > 0)
            {
                std::memcpy(dst, m_queue.data(), floatsCopied * sizeof(float));
                m_queue.erase(m_queue.begin(), m_queue.begin() + floatsCopied);
            }
        }
        // Pad with silence if the queue ran short — keeps the hardware
        // happy without a glitch flag the user can hear.
        if (floatsCopied < floatsNeeded)
        {
            std::memset(dst + floatsCopied * sizeof(float), 0,
                        (floatsNeeded - floatsCopied) * sizeof(float));
        }
        m_renderClient->ReleaseBuffer(framesAvail, 0);
    }

    if (mmHandle) AvRevertMmThreadCharacteristics(mmHandle);
}

#endif // _WIN32
