#include "SharedMemoryReader.h"

#include <cstring>

namespace retrocapture { namespace dshow_filter {

namespace {

// Reuse the layout constants verbatim — pulled into this TU's
// scope to keep the call sites legible (no `virtcam_ipc::` prefix
// noise on every line).
using virtcam_ipc::kEventName;
using virtcam_ipc::kFrameMagic;
using virtcam_ipc::kMapName;
using virtcam_ipc::kMappingSize;
using virtcam_ipc::kSharedMagic;
using virtcam_ipc::kSlotCount;
using virtcam_ipc::kSlotMaxBytes;
using virtcam_ipc::slotOffset;
using virtcam_ipc::FrameHeader;
using virtcam_ipc::SharedHeader;

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

    // OpenFileMappingW with READ-only access — the filter never
    // writes back. FILE_MAP_READ matches FILE_MAP_ALL_ACCESS that
    // the host uses on the writer side; the kernel handles the
    // intersection.
    m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMapName);
    if (m_mapping == nullptr)
    {
        m_lastError = "OpenFileMappingW failed — host probably not "
                      "writing (RetroCapture stopped or virtcam off)";
        outError    = m_lastError;
        close();
        return false;
    }

    m_mapView = MapViewOfFile(m_mapping, FILE_MAP_READ,
                              /*offsetHigh=*/0, /*offsetLow=*/0,
                              kMappingSize);
    if (m_mapView == nullptr)
    {
        m_lastError = "MapViewOfFile failed";
        outError    = m_lastError;
        close();
        return false;
    }

    // SYNCHRONIZE so we can WaitForSingleObject on it. The host
    // creates it with EVENT_ALL_ACCESS so the rights intersect.
    m_event = OpenEventW(SYNCHRONIZE, FALSE, kEventName);
    if (m_event == nullptr)
    {
        m_lastError = "OpenEventW failed";
        outError    = m_lastError;
        close();
        return false;
    }

    // Cheap sanity check on the header before we start trusting
    // the rest of the mapping. Wrong magic = host wrote an
    // incompatible layout (likely a `_v2` instance pretending to
    // be `_v1`).
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
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    if (m_mapView != nullptr)
    {
        UnmapViewOfFile(m_mapView);
        m_mapView = nullptr;
    }
    if (m_mapping != nullptr)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}

bool SharedMemoryReader::waitFrame(DWORD timeoutMs)
{
    if (!isOpen())
    {
        return false;
    }
    return WaitForSingleObject(m_event, timeoutMs) == WAIT_OBJECT_0;
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

    // Read writeSlot via an InterlockedCompareExchange against
    // itself — gives us an acquire-load equivalent (matches the
    // writer's InterlockedExchange release-store flip). Plain
    // volatile read would also work on x86 but the interlocked
    // version is portable across architectures.
    const LONG slotRead = InterlockedCompareExchange(
        const_cast<volatile LONG *>(
            reinterpret_cast<const volatile LONG *>(&sharedH->writeSlot)),
        0, 0);

    const uint32_t slot = static_cast<uint32_t>(slotRead);
    if (slot >= kSlotCount)
    {
        m_lastError = "snapshot: writeSlot out of range";
        return false;
    }

    const uint8_t      *slotPtr   = base + slotOffset(slot);
    auto               *slotFH    = reinterpret_cast<const FrameHeader *>(slotPtr);

    if (slotFH->magic != kFrameMagic)
    {
        // Not an error per se — writer hasn't laid down a frame yet
        // (boot race). Don't smear m_lastError so the filter's
        // status surface stays clean.
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

}} // namespace retrocapture::dshow_filter
