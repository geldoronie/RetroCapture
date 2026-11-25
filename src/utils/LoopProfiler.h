#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Logger.h"

/**
 * Main-loop stage timer (render thread only). Each stage accumulates wall
 * time per iteration; every 10 s one log line gives, per stage, the mean
 * and the worst iteration, plus the loop rate and how many iterations
 * took longer than a 60 Hz frame. Used to find what keeps the loop from
 * keeping up (e.g. frame generation at 1080p60).
 */
class LoopProfiler
{
public:
    enum Stage
    {
        Capture = 0, // device read + upload / colour conversion
        FrameGen,    // frame generation (plan + synthesis)
        Render,      // whole render + distribute (includes the stages below)
        Swap,        // swapBuffers
        Count
    };

    static LoopProfiler &get()
    {
        static LoopProfiler p;
        return p;
    }

    void add(Stage s, double ms)
    {
        m_cur[s] += ms;
    }

    /** End of one loop iteration: fold it into the window, log every 10 s. */
    void endIteration(double iterMs)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_n == 0 && m_windowStart.time_since_epoch().count() == 0)
            m_windowStart = now;
        for (int i = 0; i < Count; ++i)
        {
            m_sum[i] += m_cur[i];
            if (m_cur[i] > m_max[i]) m_max[i] = m_cur[i];
            m_cur[i] = 0.0;
        }
        m_iterSum += iterMs;
        if (iterMs > m_iterMax) m_iterMax = iterMs;
        if (iterMs > 16.7) ++m_slow;
        ++m_n;
        const double sec = std::chrono::duration<double>(now - m_windowStart).count();
        if (sec < 10.0) return;
        static const char *names[Count] = {"capture", "framegen", "render", "swap"};
        std::string line = "Loop profile: " + std::to_string(m_n) + " iterations in " +
                           std::to_string(static_cast<int>(sec)) + " s";
        char buf[96];
        std::snprintf(buf, sizeof(buf), " (%.1f/s), iteration %.2f avg / %.2f max ms, %d over 16.7 ms;",
                      m_n / sec, m_iterSum / m_n, m_iterMax, m_slow);
        line += buf;
        for (int i = 0; i < Count; ++i)
        {
            std::snprintf(buf, sizeof(buf), " %s %.2f/%.2f", names[i], m_sum[i] / m_n, m_max[i]);
            line += buf;
        }
        line += " ms (avg/max)";
        LOG_INFO(line);
        *this = LoopProfiler();
        m_windowStart = now;
    }

    /** RAII timer for one stage. */
    struct Scope
    {
        Stage s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit Scope(Stage st) : s(st) {}
        ~Scope()
        {
            LoopProfiler::get().add(
                s, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
    };

private:
    std::array<double, Count> m_cur{}, m_sum{}, m_max{};
    double m_iterSum = 0.0, m_iterMax = 0.0;
    int m_n = 0, m_slow = 0;
    std::chrono::steady_clock::time_point m_windowStart{};
};
