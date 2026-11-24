// Build & run: g++ -std=c++17 -O2 -o /tmp/mls tools/tests/monitor_latency_sim.cpp && /tmp/mls
// Offline simulation of MonitorLatencyController (#200): capture clock and
// playback clock differ by `skewPpm`; capture delivers 10 ms fragments with
// jitter; the playback device consumes at its own rate. Reports the backlog
// (latency) over 10 simulated minutes, with and without the controller, plus a
// 500 ms stall in the middle.
#include "../../src/audio/MonitorLatencyController.h"
#include <cstdio>
#include <deque>
#include <random>

static void run(double skewPpm, bool controlled, double seconds)
{
    const uint32_t rate = 48000, ch = 2;
    MonitorLatencyController ctl;
    ctl.configure(rate, ch, 40, 200);
    std::deque<int16_t> queue; // what sits ahead of the device
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-0.003, 0.003);

    const double dt = 0.001; // 1 ms simulation step
    double capAcc = 0, playAcc = 0, nextFrag = 0.010;
    std::vector<int16_t> frag, out;
    double maxMs = 0, minMs = 1e9, sumMs = 0; long samples = 0;
    double phase = 0;
    for (double t = 0; t < seconds; t += dt)
    {
        const bool stalled = (t > seconds / 2 && t < seconds / 2 + 0.5);
        capAcc += rate * (1.0 + skewPpm * 1e-6) * dt;
        if (t >= nextFrag + jitter(rng) && !stalled)
        {
            const size_t frames = static_cast<size_t>(capAcc);
            capAcc -= frames;
            frag.resize(frames * ch);
            for (size_t i = 0; i < frames; ++i) { int16_t v = int16_t(8000 * std::sin(phase)); phase += 0.13; frag[2*i] = frag[2*i+1] = v; }
            out.clear();
            if (controlled)
            {
                size_t drop = ctl.update(queue.size() / ch);
                for (size_t i = 0; i < drop * ch && !queue.empty(); ++i) queue.pop_front();
                ctl.process(frag.data(), frames, out);
            }
            else out = frag;
            queue.insert(queue.end(), out.begin(), out.end());
            nextFrag += 0.010;
        }
        else if (stalled) { nextFrag = t + 0.010; } // capture keeps accumulating: burst after the stall
        playAcc += rate * dt;
        size_t take = static_cast<size_t>(playAcc);
        playAcc -= take;
        for (size_t i = 0; i < take * ch && !queue.empty(); ++i) queue.pop_front();
        if (t > 5)
        {
            const double ms = 1000.0 * (queue.size() / ch) / rate;
            maxMs = std::max(maxMs, ms); minMs = std::min(minMs, ms); sumMs += ms; ++samples;
        }
    }
    std::printf("skew %+6.0f ppm %-12s latency mean %6.1f ms  min %6.1f  max %7.1f  snaps %llu\n",
                skewPpm, controlled ? "controlled" : "raw", sumMs / samples, minMs, maxMs,
                (unsigned long long)ctl.snapCount());
}

int main()
{
    for (double skew : {1000.0, -1000.0, 200.0, 0.0})
    {
        run(skew, false, 600);
        run(skew, true, 600);
    }
}
