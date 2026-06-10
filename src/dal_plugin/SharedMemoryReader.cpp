#include "SharedMemoryReader.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace retrocapture { namespace dal_plugin {

namespace {

using virtcam_ipc::FrameHeader;
using virtcam_ipc::SharedHeader;
using virtcam_ipc::kFrameMagic;
using virtcam_ipc::kMappingSize;
using virtcam_ipc::kSemName;
using virtcam_ipc::kSharedMagic;
using virtcam_ipc::kShmName;
using virtcam_ipc::kSlotCount;
using virtcam_ipc::kSlotMaxBytes;
using virtcam_ipc::slotOffset;

} // namespace

SharedMemoryReader::SharedMemoryReader() = default;

SharedMemoryReader::~SharedMemoryReader()
{
    close();
}

bool SharedMemoryReader::open(std::string &outError)
{
    if (isOpen())
    {
        return true;
    }

    // O_RDONLY — the plug-in never writes back. mmap with PROT_READ
    // matches.
    m_shmFd = ::shm_open(kShmName, O_RDONLY, 0);
    if (m_shmFd < 0)
    {
        m_lastError = std::string("shm_open failed: ") +
                      std::strerror(errno);
        outError    = m_lastError;
        close();
        return false;
    }

    void *p = ::mmap(nullptr, kMappingSize, PROT_READ,
                     MAP_SHARED, m_shmFd, 0);
    if (p == MAP_FAILED)
    {
        m_lastError = std::string("mmap failed: ") + std::strerror(errno);
        outError    = m_lastError;
        close();
        return false;
    }
    m_mapView = p;

    sem_t *sem = ::sem_open(kSemName, 0);
    if (sem == SEM_FAILED)
    {
        m_lastError = std::string("sem_open failed: ") +
                      std::strerror(errno);
        outError    = m_lastError;
        close();
        return false;
    }
    m_sem = sem;

    // Cheap header validation before we trust any of the slots.
    auto *hdr = static_cast<const SharedHeader *>(m_mapView);
    if (hdr->magic != kSharedMagic)
    {
        m_lastError = "SharedHeader magic mismatch — protocol drift";
        outError    = m_lastError;
        close();
        return false;
    }
    return true;
}

void SharedMemoryReader::close()
{
    if (m_sem != nullptr)
    {
        ::sem_close(m_sem);
        m_sem = nullptr;
    }
    if (m_mapView != nullptr)
    {
        ::munmap(m_mapView, kMappingSize);
        m_mapView = nullptr;
    }
    if (m_shmFd >= 0)
    {
        ::close(m_shmFd);
        m_shmFd = -1;
    }
}

bool SharedMemoryReader::waitFrame(uint32_t timeoutMs)
{
    if (!isOpen())
    {
        return false;
    }
    // POSIX has sem_timedwait but macOS deliberately omits it.
    // The recommended workaround is a poll loop on sem_trywait
    // with a short sleep between attempts. 5 ms granularity gives
    // 200 tries/sec — plenty for a 30 fps target without burning
    // CPU.
    constexpr uint32_t kPollIntervalMs = 5;
    uint32_t elapsed = 0;
    while (elapsed < timeoutMs)
    {
        if (::sem_trywait(m_sem) == 0)
        {
            return true;
        }
        if (errno != EAGAIN)
        {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
        elapsed += kPollIntervalMs;
    }
    return false;
}

bool SharedMemoryReader::snapshotFrame(FrameHeader              &outHeader,
                                       std::vector<uint8_t>     &outPayload)
{
    if (!isOpen())
    {
        return false;
    }

    auto *base    = static_cast<const uint8_t *>(m_mapView);
    auto *sharedH = reinterpret_cast<const SharedHeader *>(base);
    if (sharedH->magic != kSharedMagic)
    {
        m_lastError = "snapshot: SharedHeader magic mismatch";
        return false;
    }

    // Acquire-load on writeSlot — pairs with the writer's
    // release-store. Reinterpret as atomic<uint32_t> via
    // std::atomic_ref-equivalent (we don't have C++20 here, so
    // we use the pointer-cast trick that works for any plain
    // uint32_t since std::atomic<uint32_t> is lock-free + has the
    // same layout on x86 / arm64).
    const uint32_t slot =
        reinterpret_cast<const std::atomic<uint32_t> *>(&sharedH->writeSlot)
            ->load(std::memory_order_acquire);
    if (slot >= kSlotCount)
    {
        m_lastError = "snapshot: writeSlot out of range";
        return false;
    }

    const uint8_t *slotPtr = base + slotOffset(slot);
    auto          *slotFH  = reinterpret_cast<const FrameHeader *>(slotPtr);
    if (slotFH->magic != kFrameMagic)
    {
        // Writer hasn't filled this slot yet (boot race) — not
        // an error per se.
        return false;
    }
    if (slotFH->payloadBytes == 0 || slotFH->payloadBytes > kSlotMaxBytes)
    {
        m_lastError = "snapshot: payloadBytes out of range";
        return false;
    }

    outHeader = *slotFH;
    outPayload.assign(slotPtr + sizeof(FrameHeader),
                      slotPtr + sizeof(FrameHeader) + slotFH->payloadBytes);
    return true;
}

}} // namespace retrocapture::dal_plugin
