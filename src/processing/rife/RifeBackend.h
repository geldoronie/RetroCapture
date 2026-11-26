#pragma once

#include "../../renderer/glad_loader.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RifeEngine;

/**
 * GL side of the RIFE frame-generation backend (#248). Owns a worker thread
 * that runs RifeEngine (ncnn + Vulkan) so inference never runs on the render
 * loop. GL thread API: submitFrame() / request() / fetch(); everything else
 * is thread-safe.
 *
 * Interop is a host round trip: each real frame is read back with
 * glReadPixels (RGBA8) and uploaded to Vulkan by the worker; each result
 * comes back with glTexSubImage2D. Zero-copy GL<->Vulkan sharing
 * (GL_EXT_memory_object + VK_KHR_external_memory) would need the GL and the
 * Vulkan device to be the same GPU and driver, and ncnn's allocators to
 * import foreign memory; the round trip works everywhere (also with a
 * software GL and a hardware Vulkan device) and its cost is measured
 * (Stats::readbackMs / uploadMs).
 *
 * Start-up (Vulkan probe + model load + shader compilation) runs on the
 * worker too; until state() is Ready the caller keeps using another method.
 * Built without ENABLE_RIFE, the class is a stub that reports why.
 */
class RifeBackend
{
public:
    enum class State
    {
        Idle,        // not started
        Loading,     // probing Vulkan / loading the model
        Ready,
        Unavailable, // see reason()
    };
    struct Stats
    {
        double readbackMs = 0.0; // glReadPixels of a real frame (GL thread)
        double uploadMs = 0.0;   // glTexSubImage2D of a result (GL thread)
        double inferMs = 0.0;    // one phase on the worker (Vulkan, incl. transfer)
        double frameUploadMs = 0.0; // host -> Vulkan + pre-process, per real frame
        double latencyMs = 0.0;  // real frame submitted -> first phase ready (EMA)
        uint64_t delivered = 0;  // results handed out
        uint64_t late = 0;       // fetch() gave up waiting (caller fell back)
    };

    RifeBackend();
    ~RifeBackend();
    RifeBackend(const RifeBackend &) = delete;
    RifeBackend &operator=(const RifeBackend &) = delete;

    /** Compiled with ENABLE_RIFE (ncnn + Vulkan available at build time). */
    static bool compiledIn();

    /** Start the worker (probe + model load) if not started yet. */
    void start();
    State state() const;
    std::string reason() const; // why Unavailable
    std::string device() const; // Vulkan device name once Ready
    /** Block until Ready / Unavailable (bench). */
    bool waitReady(int timeoutMs);

    /**
     * GL thread: a new real frame is in fbo (RGBA8 colour attachment, w x h).
     * Read it back and queue it; it pairs with the previous one.
     */
    void submitFrame(GLuint fbo, uint32_t w, uint32_t h);
    /** Schedule phase t of the current pair (no-op if already requested). */
    void request(float t);
    /**
     * GL thread: the frame at phase t of the current pair, written to tex
     * (RGBA8, w x h). Waits up to waitMs (< 0: no limit) for the worker;
     * false = not ready in time / no pair / not running.
     */
    bool fetch(float t, int waitMs, GLuint tex);
    /** Forget the frames (new geometry, source switched). Keeps the model. */
    void reset();
    /** Stop the worker and release Vulkan. Safe to call more than once. */
    void shutdown();

    Stats stats() const;

private:
    struct Frame
    {
        uint64_t id = 0;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
        int64_t submittedUs = 0;
    };
    struct Result
    {
        uint64_t pairB = 0; // id of B of the pair it belongs to
        float t = 0.0f;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        bool failed = false;
    };

    void workerMain();
    bool hasResultLocked(float t) const;
    void notePairPhasesLocked();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;     // worker wake-up
    std::condition_variable m_doneCv; // result / state changes
    std::thread m_worker;
    bool m_started = false;
    bool m_stop = false;
    State m_state = State::Idle;
    std::string m_reason;
    std::string m_device;

    // Frames waiting for upload (worker), the current pair, its phases.
    std::deque<std::shared_ptr<Frame>> m_uploads;
    uint64_t m_nextId = 1;
    uint64_t m_pairA = 0, m_pairB = 0;
    uint32_t m_w = 0, m_h = 0;
    int64_t m_pairSubmittedUs = 0;
    bool m_pairFirstDone = false;
    std::vector<float> m_requests; // phases of the current pair, in order
    std::vector<Result> m_results; // results of the current pair
    std::vector<std::vector<uint8_t>> m_pool; // recycled result buffers
    // Phases asked for in the last two pairs: a stable set (fixed-multiple
    // modes) is requested as soon as the next pair exists.
    std::vector<float> m_phasesCur, m_phasesPrev;
    bool m_resetPending = false;

    Stats m_stats;
    std::unique_ptr<RifeEngine> m_engine; // worker thread only
};
