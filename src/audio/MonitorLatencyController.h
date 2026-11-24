#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

/**
 * Keeps the local monitor's latency at a fixed target (#200).
 *
 * The capture device and the playback device run on independent clocks
 * (ppm apart), so a monitor that simply forwards every captured sample
 * accumulates the difference as ever-growing latency (or starves). This
 * controller sits between the two:
 *
 *  - update(backlogFrames) is called once per chunk with how many frames
 *    are queued ahead of the device (whatever queue grows on that
 *    platform: the bus tap on Linux, the output queue on macOS/Windows).
 *    It smooths the error against the target and turns it into a playback
 *    rate within +-0.5 % (inaudible). If the backlog is grossly over the
 *    target (after a stall), it returns how many frames to discard right
 *    away so latency snaps back instead of slowly bleeding off.
 *  - process() resamples a chunk at that rate (linear interpolation, phase
 *    continuous across chunks) — fewer output frames when there's too much
 *    backlog, more when there's too little.
 *
 * Pure C++ (no FFmpeg) so the Linux, macOS and Windows monitors share it.
 */
class MonitorLatencyController
{
public:
    void configure(uint32_t sampleRate, uint32_t channels, uint32_t targetMs = 40, uint32_t snapMs = 200)
    {
        m_rate = sampleRate ? sampleRate : 48000;
        m_channels = std::max<uint32_t>(1, std::min<uint32_t>(channels, kMaxChannels));
        m_targetFrames = static_cast<double>(m_rate) * targetMs / 1000.0;
        m_snapFrames = static_cast<double>(m_rate) * snapMs / 1000.0;
        m_configured = true;
        reset();
    }

    void reset()
    {
        m_errEma = 0.0;
        m_step = 1.0;
        m_pos = 0.0;
        m_havePrev = false;
        for (uint32_t c = 0; c < kMaxChannels; ++c)
            m_prev[c] = 0.0f;
    }

    // Returns the number of frames to discard from the backlog right now
    // (0 = none). Updates the playback rate otherwise.
    size_t update(size_t backlogFrames)
    {
        const double backlog = static_cast<double>(backlogFrames);
        m_lastBacklogFrames = backlogFrames;
        if (backlog > m_targetFrames + m_snapFrames)
        {
            ++m_snaps;
            m_errEma = 0.0;
            m_step = 1.0;
            return static_cast<size_t>(backlog - m_targetFrames);
        }
        const double err = backlog - m_targetFrames;
        m_errEma += 0.02 * (err - m_errEma);
        // Bleed the error off over ~2 s, capped at +-0.5 %.
        double adj = m_errEma / (static_cast<double>(m_rate) * 2.0);
        adj = std::max(-0.005, std::min(0.005, adj));
        m_step = 1.0 + adj; // input frames consumed per output frame
        return 0;
    }

    // Resample `frames` interleaved frames into `out` (appended).
    template <typename T>
    void process(const T *in, size_t frames, std::vector<T> &out)
    {
        if (!in || frames == 0)
            return;
        const uint32_t ch = m_channels;
        if (!m_havePrev)
        {
            for (uint32_t c = 0; c < ch; ++c)
                m_prev[c] = static_cast<float>(in[c]);
            m_havePrev = true;
        }
        // Virtual input x[0] = previous chunk's last frame, x[k] = in[k-1].
        const double n = static_cast<double>(frames);
        double t = m_pos;
        out.reserve(out.size() + static_cast<size_t>(n / m_step) + 2 * ch);
        while (t < n)
        {
            const size_t i = static_cast<size_t>(t);
            const float f = static_cast<float>(t - static_cast<double>(i));
            for (uint32_t c = 0; c < ch; ++c)
            {
                const float a = (i == 0) ? m_prev[c] : static_cast<float>(in[(i - 1) * ch + c]);
                const float b = static_cast<float>(in[i * ch + c]);
                out.push_back(toSample<T>(a + (b - a) * f));
            }
            t += m_step;
        }
        m_pos = t - n;
        for (uint32_t c = 0; c < ch; ++c)
            m_prev[c] = static_cast<float>(in[(frames - 1) * ch + c]);
    }

    double rateAdjustPpm() const { return (m_step - 1.0) * 1e6; }
    size_t lastBacklogFrames() const { return m_lastBacklogFrames; }
    uint64_t snapCount() const { return m_snaps; }
    uint32_t sampleRate() const { return m_rate; }
    bool configured() const { return m_configured; }

private:
    static constexpr uint32_t kMaxChannels = 8;

    template <typename T>
    static T toSample(float v)
    {
        if (std::is_floating_point<T>::value)
            return static_cast<T>(v);
        const float lo = static_cast<float>(std::numeric_limits<T>::min());
        const float hi = static_cast<float>(std::numeric_limits<T>::max());
        return static_cast<T>(std::lround(std::max(lo, std::min(hi, v))));
    }

    uint32_t m_rate = 48000;
    uint32_t m_channels = 2;
    double m_targetFrames = 0.0;
    double m_snapFrames = 0.0;
    double m_errEma = 0.0;
    double m_step = 1.0;
    double m_pos = 0.0;
    bool m_havePrev = false;
    float m_prev[kMaxChannels] = {};
    size_t m_lastBacklogFrames = 0;
    uint64_t m_snaps = 0;
    bool m_configured = false;
};
