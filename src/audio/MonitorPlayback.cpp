#include "MonitorPlayback.h"

#include "../utils/Logger.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <chrono>
#include <vector>

MonitorPlayback::~MonitorPlayback()
{
    stop();
}

bool MonitorPlayback::start(std::shared_ptr<AudioBus::Tap> tap,
                            uint32_t sampleRate, uint32_t channels)
{
    if (m_running.load())
    {
        return true;
    }
    if (!tap || sampleRate == 0 || channels == 0 || channels > 8)
    {
        LOG_ERROR("MonitorPlayback::start — invalid args");
        return false;
    }

    pa_sample_spec spec;
    spec.format   = PA_SAMPLE_S16LE;
    spec.rate     = sampleRate;
    spec.channels = static_cast<uint8_t>(channels);

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength   = pa_usec_to_bytes(50 * 1000, &spec);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);
    attr.fragsize  = static_cast<uint32_t>(-1);

    int err = 0;
    m_stream = pa_simple_new(nullptr,
                             "RetroCapture",
                             PA_STREAM_PLAYBACK,
                             nullptr,
                             "Monitor",
                             &spec,
                             nullptr,
                             &attr,
                             &err);
    if (!m_stream)
    {
        LOG_ERROR(std::string("MonitorPlayback: pa_simple_new failed — ") +
                  pa_strerror(err));
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels   = channels;
    m_tap        = std::move(tap);
    m_running    = true;
    m_thread     = std::thread(&MonitorPlayback::writerLoop, this);
    LOG_INFO("MonitorPlayback: opened " + std::to_string(sampleRate) +
             " Hz x " + std::to_string(channels) + " ch");
    return true;
}

void MonitorPlayback::stop()
{
    m_running = false;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    if (m_stream)
    {
        int err = 0;
        pa_simple_flush(m_stream, &err);
        pa_simple_free(m_stream);
        m_stream = nullptr;
    }
    m_tap.reset();
}

void MonitorPlayback::writerLoop()
{
    // ~10 ms per pull.
    const size_t chunkSamples = static_cast<size_t>(m_sampleRate / 100) * m_channels;
    std::vector<int16_t> chunk(chunkSamples);
    std::vector<int16_t> drain;
    std::vector<int16_t> out;
    // #200 — keep ~20 ms queued in the tap on top of the sink's 50 ms.
    m_latency.configure(m_sampleRate, m_channels, 20, 200);
    auto lastReport = std::chrono::steady_clock::now();

    while (m_running.load())
    {
        if (m_resyncPending.exchange(false))
        {
            // Toss everything currently queued in the tap so we
            // restart from the newest samples the producer is
            // pushing right now.
            const size_t available = m_tap->available();
            if (available > 0)
            {
                drain.resize(available);
                m_tap->pull(drain.data(), available);
            }
            // Discard what PulseAudio already has buffered for
            // playback. After this the stream is empty and the next
            // pa_simple_write below will refill from "now".
            int err = 0;
            pa_simple_flush(m_stream, &err);
            m_latency.reset();
            LOG_INFO("MonitorPlayback: resync (dropped " +
                     std::to_string(available / (m_channels ? m_channels : 1)) +
                     " queued frames)");
        }

        // #200 — latency control on the tap backlog.
        const size_t backlogFrames = m_tap->available() / m_channels;
        const size_t snap = m_latency.update(backlogFrames);
        if (snap > 0)
        {
            m_tap->discard(snap * m_channels);
            LOG_INFO("MonitorPlayback: latency snap — dropped " +
                     std::to_string(snap * 1000 / m_sampleRate) + " ms of backlog");
        }

        const size_t got = m_tap->pull(chunk.data(), chunk.size());
        if (got == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        out.clear();
        m_latency.process(chunk.data(), got / m_channels, out);

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(60))
        {
            lastReport = now;
            LOG_INFO("MonitorPlayback: backlog " + std::to_string(backlogFrames * 1000 / m_sampleRate) +
                     " ms, rate adjust " + std::to_string(static_cast<int>(m_latency.rateAdjustPpm())) + " ppm");
        }

        if (out.empty())
        {
            continue;
        }
        int err = 0;
        if (pa_simple_write(m_stream, out.data(),
                            out.size() * sizeof(int16_t), &err) < 0)
        {
            LOG_ERROR(std::string("MonitorPlayback: pa_simple_write failed — ") +
                      pa_strerror(err));
            m_running = false;
            return;
        }
    }
}
