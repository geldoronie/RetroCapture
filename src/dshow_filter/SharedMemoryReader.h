#pragma once

// Filter-side reader for the virtcam IPC channel.
//
// Lives inside the DirectShow source filter DLL (which loads
// into every consumer process — OBS, Chrome, Zoom, ...) and
// reads the frames that `VirtualCameraOutputWin` writes from
// the RetroCapture host process.
//
// See `docs/VIRTCAM_WINDOWS.md` for the full architecture and
// `src/output/VirtcamIpcLayout.h` for the on-wire format.

#include "../output/VirtcamIpcLayout.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace retrocapture { namespace dshow_filter {

class SharedMemoryReader
{
public:
    SharedMemoryReader();
    ~SharedMemoryReader();

    SharedMemoryReader(const SharedMemoryReader &)            = delete;
    SharedMemoryReader &operator=(const SharedMemoryReader &) = delete;

    /// Open the named mapping + event the host wrote with. Returns
    /// false if either side is missing — typically because the
    /// RetroCapture process isn't running yet, or its virtcam
    /// toggle is off. The filter should retry periodically.
    bool open(std::string &outError);

    /// CloseHandle + UnmapViewOfFile. Idempotent.
    void close();

    bool isOpen() const { return m_event != nullptr; }

    /// Block on the auto-reset frame-ready event for up to
    /// `timeoutMs`. Returns true if the event was signalled within
    /// the timeout, false on timeout / WAIT_FAILED. A true return
    /// means *a* frame is ready; the actual data lives in the
    /// most-recently-completed slot (read via snapshotFrame()).
    bool waitFrame(DWORD timeoutMs);

    /// Take an atomic snapshot of the current write slot. Caller
    /// gets the FrameHeader by value and a copy of the payload in
    /// `outPayload`. Safe to call without waitFrame() first — the
    /// reader will simply re-deliver whatever the latest slot has
    /// (used for "frozen frame" filling on writer idle).
    ///
    /// Returns false on:
    /// - reader not open
    /// - SharedHeader magic mismatch (corrupt / wrong version)
    /// - FrameHeader magic mismatch in the indicated slot (writer
    ///   hasn't laid down a valid frame yet)
    /// - payloadBytes exceeds the slot capacity (corrupt header)
    bool snapshotFrame(virtcam_ipc::FrameHeader &outHeader,
                       std::vector<uint8_t>     &outPayload);

    /// Last failure string from open() / snapshot — useful for
    /// the filter's status surface.
    std::string lastError() const { return m_lastError; }

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_event   = nullptr;
    void  *m_mapView = nullptr;
    std::string m_lastError;
};

}} // namespace retrocapture::dshow_filter
