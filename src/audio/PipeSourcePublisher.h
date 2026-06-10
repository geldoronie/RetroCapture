#pragma once

#include "AudioBus.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

/**
 * Drains an AudioBus tap into a PulseAudio module-pipe-source FIFO.
 *
 * The module itself (the one that publishes the `RetroCapture` virtual
 * source to the rest of the OS audio graph) is loaded/unloaded by the
 * caller, which already owns the PulseAudio context. Keeping the PA
 * context out of this class avoids cross-thread pa_context_* calls —
 * the writer thread here only does FIFO IO.
 */
class PipeSourcePublisher
{
public:
    PipeSourcePublisher() = default;
    ~PipeSourcePublisher();

    PipeSourcePublisher(const PipeSourcePublisher &)            = delete;
    PipeSourcePublisher &operator=(const PipeSourcePublisher &) = delete;

    // Opens `fifoPath` for writing (non-blocking) and spawns the writer
    // thread. Caller must have already loaded module-pipe-source so the
    // read end of the FIFO is attached, otherwise open() returns ENXIO
    // and start() fails. Returns false on any setup error.
    bool start(const std::string &fifoPath, std::shared_ptr<AudioBus::Tap> tap);

    // Signals the writer thread to exit, joins it and closes the fd.
    // Safe to call from any thread; idempotent.
    void stop();

    bool isRunning() const { return m_running.load(); }

private:
    void writerLoop();

    int                            m_fd = -1;
    std::atomic<bool>              m_running{false};
    std::thread                    m_thread;
    std::shared_ptr<AudioBus::Tap> m_tap;
};
