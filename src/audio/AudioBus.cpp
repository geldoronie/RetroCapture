#include "AudioBus.h"

#include <algorithm>
#include <cstring>

AudioBus::AudioBus(uint32_t sampleRate, uint32_t channels)
    : m_sampleRate(sampleRate), m_channels(channels)
{
}

std::shared_ptr<AudioBus::Tap> AudioBus::createTap(size_t capacitySamples)
{
    auto tap = std::shared_ptr<Tap>(new Tap(capacitySamples));
    std::lock_guard<std::mutex> lock(m_tapsMutex);
    m_taps.push_back(tap);
    return tap;
}

void AudioBus::push(const int16_t *interleaved, size_t sampleCount)
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
        tap->push(interleaved, sampleCount);
    }
}

void AudioBus::Tap::push(const int16_t *src, size_t sampleCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.insert(m_buffer.end(), src, src + sampleCount);
    if (m_capacity > 0 && m_buffer.size() > m_capacity)
    {
        const size_t drop = m_buffer.size() - m_capacity;
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + drop);
    }
}

size_t AudioBus::Tap::pull(int16_t *dst, size_t maxSamples)
{
    if (!dst || maxSamples == 0)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t n = std::min(maxSamples, m_buffer.size());
    if (n == 0)
    {
        return 0;
    }
    // deque iterators don't guarantee contiguous storage, so element-by-
    // element copy. Sizes here are <= a few thousand samples per pull,
    // so this is not a hot-path concern.
    for (size_t i = 0; i < n; ++i)
    {
        dst[i] = m_buffer[i];
    }
    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + n);
    return n;
}

size_t AudioBus::Tap::available() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size();
}
