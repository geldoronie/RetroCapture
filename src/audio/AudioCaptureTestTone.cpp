#include "AudioCaptureTestTone.h"

#include "../utils/Logger.h"
#include "../utils/MediaClock.h"

#include <chrono>
#include <cmath>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 1000.0;
constexpr int64_t kBeepUs = 100000;  // beep length: first 100 ms of each second
constexpr double kAmplitude = 0.5 * 32767.0;
} // namespace

AudioCaptureTestTone::AudioCaptureTestTone(int skewPpm, int fragmentMs)
    : m_skewPpm(skewPpm), m_fragmentMs(fragmentMs > 0 ? fragmentMs : 10) {}

AudioCaptureTestTone::~AudioCaptureTestTone()
{
    close();
}

bool AudioCaptureTestTone::open(const std::string & /*deviceName*/)
{
    if (m_open)
        return true;
    m_bus = std::make_unique<AudioBus>(m_sampleRate, m_channels);
    // ~2 s of slack, same as the real backends' local tap.
    m_localTap = m_bus->createTap(static_cast<size_t>(m_sampleRate) * m_channels * 2);
    m_running = true;
    m_open = true;
    m_thread = std::thread(&AudioCaptureTestTone::generatorLoop, this);
    LOG_INFO("AudioCaptureTestTone opened: " + std::to_string(m_sampleRate) + " Hz x " +
             std::to_string(m_channels) + " ch, device clock skew " +
             std::to_string(m_skewPpm) + " ppm, " + std::to_string(m_fragmentMs) +
             " ms fragments");
    return true;
}

void AudioCaptureTestTone::close()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
    m_open = false;
    m_localTap.reset();
    m_bus.reset();
}

std::vector<AudioDeviceInfo> AudioCaptureTestTone::listDevices()
{
    AudioDeviceInfo info;
    info.id = "test";
    info.name = "Test Tone (A/V sync)";
    info.description = "synthetic";
    return {info};
}

size_t AudioCaptureTestTone::getSamples(std::vector<float> &samples)
{
    samples.clear();
    if (!m_localTap)
        return 0;
    const size_t avail = m_localTap->available();
    if (avail == 0)
        return 0;
    std::vector<int16_t> raw(avail);
    const size_t n = m_localTap->pull(raw.data(), avail);
    samples.resize(n);
    for (size_t i = 0; i < n; ++i)
        samples[i] = static_cast<float>(raw[i]) / 32768.0f;
    return n;
}

size_t AudioCaptureTestTone::getSamples(int16_t *buffer, size_t maxSamples)
{
    if (!m_open || !buffer || maxSamples == 0 || !m_localTap)
        return 0;
    return m_localTap->pull(buffer, maxSamples);
}

void AudioCaptureTestTone::generatorLoop()
{
    // The device's real sample rate, as seen by the system clock.
    const double deviceRate = m_sampleRate * (1.0 + m_skewPpm * 1e-6);
    const int64_t startUs = MediaClock::nowUs();
    uint64_t produced = 0; // frames (per-channel samples) produced so far
    std::vector<int16_t> chunk;

    while (m_running)
    {
        const int64_t nowUs = MediaClock::nowUs();
        const uint64_t target =
            static_cast<uint64_t>(static_cast<double>(nowUs - startUs) * deviceRate / 1e6);
        // Deliver whole fragments only, like a real capture backend.
        const uint64_t fragmentFrames =
            static_cast<uint64_t>(deviceRate * m_fragmentMs / 1000.0);
        if (target >= produced + fragmentFrames)
        {
            const uint64_t frames = target - produced;
            chunk.resize(static_cast<size_t>(frames) * m_channels);
            for (uint64_t i = 0; i < frames; ++i)
            {
                // Real time at which this sample was "captured".
                const uint64_t n = produced + i;
                const double tUs = static_cast<double>(startUs) + static_cast<double>(n) * 1e6 / deviceRate;
                const int64_t inSecondUs = static_cast<int64_t>(tUs) % 1000000LL;
                int16_t v = 0;
                if (inSecondUs < kBeepUs)
                {
                    const double phase = 2.0 * kPi * kToneHz * (static_cast<double>(inSecondUs) / 1e6);
                    v = static_cast<int16_t>(kAmplitude * std::sin(phase));
                }
                for (uint32_t c = 0; c < m_channels; ++c)
                    chunk[static_cast<size_t>(i) * m_channels + c] = v;
            }
            m_bus->push(chunk.data(), chunk.size());
            produced = target;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
