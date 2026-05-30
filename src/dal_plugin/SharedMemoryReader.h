#pragma once

// Reader-side IPC for the macOS DAL plug-in. Mirrors
// `src/dshow_filter/SharedMemoryReader.h` (Windows) one-to-one
// in API and semantics; the implementations differ only in the
// underlying primitives (POSIX shm + sem vs Win32 named mapping
// + auto-reset event).
//
// Lives inside RetroCaptureVCam.plugin (loaded into every consumer
// process via /Library/CoreMediaIO/Plug-Ins/DAL/) and reads the
// frames `VirtualCameraOutputMac` writes from the RetroCapture
// host process.

#include "../output/VirtcamIpcLayout.h"

#include <semaphore.h>

#include <cstdint>
#include <string>
#include <vector>

namespace retrocapture { namespace dal_plugin {

class SharedMemoryReader
{
public:
    SharedMemoryReader();
    ~SharedMemoryReader();

    SharedMemoryReader(const SharedMemoryReader &)            = delete;
    SharedMemoryReader &operator=(const SharedMemoryReader &) = delete;

    /// Open the named shm + sem the host wrote with. Returns false
    /// if either side is missing — typically because the host
    /// process isn't running yet, or its virtcam toggle is off.
    /// The plug-in should retry periodically.
    bool open(std::string &outError);

    /// munmap + close (without unlinking — host owns the names).
    /// Idempotent.
    void close();

    bool isOpen() const { return m_sem != nullptr; }

    /// Block on the frame-ready semaphore for up to `timeoutMs`.
    /// Returns true if signalled within the timeout. A true return
    /// means *a* frame is ready; the actual data lives in the
    /// most-recently-completed slot (read via snapshotFrame()).
    /// Uses `sem_timedwait` where available; falls back to a tight
    /// poll on `sem_trywait` for macOS (which doesn't ship the
    /// portable variant — by design — so we busy-poll briefly).
    bool waitFrame(uint32_t timeoutMs);

    /// Take an atomic snapshot of the current write slot. Caller
    /// gets the FrameHeader by value and a copy of the payload in
    /// `outPayload`. Safe to call without waitFrame() first — the
    /// reader will simply re-deliver whatever the latest slot has
    /// (used for "frozen frame" filling on writer idle).
    bool snapshotFrame(virtcam_ipc::FrameHeader &outHeader,
                       std::vector<uint8_t>     &outPayload);

    std::string lastError() const { return m_lastError; }

private:
    int    m_shmFd   = -1;
    void  *m_mapView = nullptr;
    sem_t *m_sem     = nullptr;
    std::string m_lastError;
};

}} // namespace retrocapture::dal_plugin
