#include "AudioBus.h"

#include "../utils/MediaClock.h"

#include <algorithm>
#include <cstring>

AudioBus::AudioBus(uint32_t sampleRate, uint32_t channels)
    : m_sampleRate(sampleRate), m_channels(channels)
{
}

uint32_t AudioBus::getSampleRate() const
{
    std::lock_guard<std::mutex> lock(m_formatMutex);
    return m_sampleRate;
}

uint32_t AudioBus::getChannels() const
{
    std::lock_guard<std::mutex> lock(m_formatMutex);
    return m_channels;
}

void AudioBus::setFormat(uint32_t sampleRate, uint32_t channels)
{
    {
        std::lock_guard<std::mutex> lock(m_formatMutex);
        m_sampleRate = sampleRate;
        m_channels = channels;
    }
    std::lock_guard<std::mutex> lock(m_tapsMutex);
    for (auto &w : m_taps)
    {
        if (auto sp = w.lock())
            sp->setFormat(sampleRate, channels);
    }
}

std::shared_ptr<AudioBus::Tap> AudioBus::createTap(size_t capacitySamples)
{
    uint32_t rate, ch;
    {
        std::lock_guard<std::mutex> lock(m_formatMutex);
        rate = m_sampleRate;
        ch = m_channels;
    }
    auto tap = std::shared_ptr<Tap>(new Tap(capacitySamples, rate, ch));
    std::lock_guard<std::mutex> lock(m_tapsMutex);
    m_taps.push_back(tap);
    return tap;
}

void AudioBus::push(const int16_t *interleaved, size_t sampleCount)
{
    if (!interleaved || sampleCount == 0)
        return;
    uint32_t rate, ch;
    {
        std::lock_guard<std::mutex> lock(m_formatMutex);
        rate = m_sampleRate;
        ch = m_channels;
    }
    int64_t durationUs = 0;
    if (rate > 0 && ch > 0)
        durationUs = static_cast<int64_t>(sampleCount / ch) * 1000000LL / rate;
    push(interleaved, sampleCount, MediaClock::nowUs() - durationUs);
}

void AudioBus::push(const int16_t *interleaved, size_t sampleCount, int64_t firstSampleTsUs)
{
    if (!interleaved || sampleCount == 0)
    {
        return;
    }

    std::vector<std::shared_ptr<Tap>> live;
    {
        std::lock_guard<std::mutex> lock(m_tapsMutex);
        live.reserve(m_taps.size());
        auto it = m_taps.begin();
        while (it != m_taps.end())
        {
            if (auto sp = it->lock())
            {
                live.push_back(std::move(sp));
                ++it;
            }
            else
            {
                it = m_taps.erase(it);
            }
        }
    }

    for (auto &tap : live)
    {
        tap->push(interleaved, sampleCount, firstSampleTsUs);
    }
}

void AudioBus::Tap::setFormat(uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampleRate = sampleRate;
    m_channels = channels;
}

void AudioBus::Tap::push(const int16_t *src, size_t sampleCount, int64_t firstSampleTsUs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t abs = m_frontAbs + m_buffer.size();
    m_anchors.push_back(Anchor{abs, firstSampleTsUs});
    m_buffer.insert(m_buffer.end(), src, src + sampleCount);
    if (m_capacity > 0 && m_buffer.size() > m_capacity)
    {
        popFrontLocked(m_buffer.size() - m_capacity);
    }
}

void AudioBus::Tap::popFrontLocked(size_t n)
{
    n = std::min(n, m_buffer.size());
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(n));
    m_frontAbs += n;
    // Keep the last anchor at or before the new front: it still dates the
    // front sample. Anything older is no longer needed.
    while (m_anchors.size() > 1 && m_anchors[1].absIndex <= m_frontAbs)
    {
        m_anchors.pop_front();
    }
    if (m_buffer.empty())
    {
        m_anchors.clear();
    }
}

int64_t AudioBus::Tap::timestampOfLocked(uint64_t abs) const
{
    // Last anchor at or before `abs` (anchors are few: one per push still
    // in the queue, so a reverse linear scan is cheap).
    for (auto it = m_anchors.rbegin(); it != m_anchors.rend(); ++it)
    {
        if (it->absIndex <= abs)
        {
            const uint64_t offsetSamples = abs - it->absIndex;
            if (m_sampleRate == 0 || m_channels == 0)
                return it->tsUs;
            return it->tsUs + static_cast<int64_t>(offsetSamples / m_channels) * 1000000LL /
                                  static_cast<int64_t>(m_sampleRate);
        }
    }
    return MediaClock::nowUs();
}

size_t AudioBus::Tap::pull(int16_t *dst, size_t maxSamples)
{
    int64_t ignored = 0;
    return pull(dst, maxSamples, ignored);
}

size_t AudioBus::Tap::pull(int16_t *dst, size_t maxSamples, int64_t &firstSampleTsUs)
{
    if (!dst || maxSamples == 0)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = std::min(maxSamples, m_buffer.size());
    // Never split an interleaved frame across pulls.
    if (m_channels > 1)
        n -= n % m_channels;
    if (n == 0)
    {
        return 0;
    }
    firstSampleTsUs = timestampOfLocked(m_frontAbs);
    std::copy(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(n), dst);
    popFrontLocked(n);
    return n;
}

size_t AudioBus::Tap::discard(size_t maxSamples)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = std::min(maxSamples, m_buffer.size());
    if (m_channels > 1)
        n -= n % m_channels;
    popFrontLocked(n);
    return n;
}

size_t AudioBus::Tap::available() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
}
