#include "PipeSourcePublisher.h"

#include "../utils/Logger.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

PipeSourcePublisher::~PipeSourcePublisher()
{
    stop();
}

bool PipeSourcePublisher::start(const std::string &fifoPath,
                                std::shared_ptr<AudioBus::Tap> tap)
{
    if (m_running.load())
    {
        LOG_WARN("PipeSourcePublisher already running");
        return true;
    }
    if (!tap)
    {
        LOG_ERROR("PipeSourcePublisher::start — null tap");
        return false;
    }

    // O_NONBLOCK on the write end so the writer thread never blocks on
    // a slow downstream PulseAudio consumer; we drop samples instead
    // (drop-oldest is also what AudioBus::Tap does once at capacity).
    m_fd = ::open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
    if (m_fd < 0)
    {
        LOG_ERROR("PipeSourcePublisher: open(" + fifoPath + ") failed: " +
                  std::string(std::strerror(errno)));
        return false;
    }

    m_tap     = std::move(tap);
    m_running = true;
    m_thread  = std::thread(&PipeSourcePublisher::writerLoop, this);
    return true;
}

void PipeSourcePublisher::stop()
{
    if (!m_running.exchange(false))
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        return;
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_tap.reset();
}

void PipeSourcePublisher::writerLoop()
{
    // ~10 ms chunks at 44.1 kHz stereo S16LE. Large enough to amortize
    // syscall overhead, small enough to stay below the FIFO's PIPE_BUF
    // so partial writes don't fragment frames the reader has to
    // reassemble. PIPE_BUF on Linux is 4096 bytes; an interleaved S16LE
    // stereo block of 882 samples = 3528 bytes fits cleanly.
    constexpr size_t kChunkSamples = 882 * 2;
    std::vector<int16_t> chunk(kChunkSamples);

    while (m_running.load())
    {
        const size_t got = m_tap->pull(chunk.data(), chunk.size());
        if (got == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const char *p          = reinterpret_cast<const char *>(chunk.data());
        size_t      remaining  = got * sizeof(int16_t);
        bool        droppedAny = false;

        while (remaining > 0 && m_running.load())
        {
            const ssize_t w = ::write(m_fd, p, remaining);
            if (w > 0)
            {
                p         += w;
                remaining -= static_cast<size_t>(w);
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                // FIFO full — pipe-source isn't draining fast enough.
                // Drop the unwritten remainder of this chunk rather than
                // backing up the in-process bus. Log only the first
                // occurrence per session at WARN level; subsequent ones
                // are silent so a chronic mismatch doesn't flood logs.
                droppedAny = true;
                break;
            }
            if (w < 0 && errno == EINTR)
            {
                continue;
            }
            if (w < 0 && errno == EPIPE)
            {
                // Reader side gone — module-pipe-source unloaded. Exit
                // the loop; stop() will close the fd.
                LOG_INFO("PipeSourcePublisher: reader closed FIFO, exiting");
                m_running = false;
                return;
            }
            // Anything else is unexpected.
            LOG_ERROR("PipeSourcePublisher: write failed: " +
                      std::string(std::strerror(errno)));
            m_running = false;
            return;
        }

        if (droppedAny)
        {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
            {
                LOG_WARN("PipeSourcePublisher: pipe-source FIFO full, "
                         "dropping samples (consumer behind)");
            }
        }
    }
}
