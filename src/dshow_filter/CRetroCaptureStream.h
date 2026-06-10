#pragma once

// CSourceStream subclass that produces frames for the consumer.
// One per filter (single output pin). FillBuffer is the hot path:
// it waits on the writer's frame-ready event, snapshots the shared
// memory slot, and copies into the IMediaSample buffer.

#include "SharedMemoryReader.h"

// strmbase brings the CSource / CSourceStream definitions. The
// `<streams.h>` umbrella is the canonical entry point.
#include <streams.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace retrocapture { namespace dshow_filter {

class CRetroCaptureFilter;

class CRetroCaptureStream : public CSourceStream
{
public:
    CRetroCaptureStream(HRESULT             *phr,
                        CRetroCaptureFilter *pParent,
                        LPCWSTR              pPinName);
    ~CRetroCaptureStream();

    // -- CSourceStream overrides ------------------------------------

    // Enumerate the media types we offer. Called by the consumer
    // during connection negotiation; we expose RGB24 at three
    // common resolutions (640×480, 1280×720, 1920×1080).
    HRESULT GetMediaType(int iPosition, CMediaType *pmt) override;

    // Accept-or-reject incoming type. We accept any of the
    // resolutions we offered in GetMediaType + only RGB24.
    HRESULT CheckMediaType(const CMediaType *pMediaType) override;

    // After CheckMediaType succeeds, the consumer commits the type
    // here. We cache width/height/fps from it for FillBuffer.
    HRESULT SetMediaType(const CMediaType *pmt) override;

    // Negotiate buffer geometry with the allocator: how many
    // buffers + how big each one.
    HRESULT DecideBufferSize(IMemAllocator        *pAlloc,
                             ALLOCATOR_PROPERTIES *pRequest) override;

    // The hot path. Called once per output frame; we have to fill
    // pSample and return S_OK. Returning S_FALSE means EOS to most
    // consumers — never do that for a live source.
    HRESULT FillBuffer(IMediaSample *pSample) override;

private:
    // Lazy connect to the host's shared memory + event. Cheap to
    // call every FillBuffer because of the isOpen() short-circuit.
    void ensureReaderOpen();

    // Fill `dst` with a generic "no signal" pattern (slate of
    // black). Used when the host isn't running or hasn't laid
    // down a first frame yet. Sized to (m_width * m_height * 3).
    void fillBlankFrame(uint8_t *dst);

    SharedMemoryReader m_reader;

    // Negotiated format. Defaults match the first GetMediaType
    // entry so a consumer that skips negotiation gets something
    // sane.
    int            m_width             = 1280;
    int            m_height            = 720;
    int            m_fps               = 30;
    REFERENCE_TIME m_avgTimePerFrame   = 10'000'000 / 30;  // 100ns units

    // Stream-clock-relative timestamp of the next frame we emit.
    REFERENCE_TIME m_nextSampleTime    = 0;

    // Last good frame we successfully delivered, cached so we can
    // re-emit it when the writer goes idle without making the
    // consumer think the camera disconnected. RGB24 only.
    std::vector<uint8_t> m_frozenFrame;
    int                  m_frozenWidth  = 0;
    int                  m_frozenHeight = 0;

    // Reconnect throttle — if open() fails we don't retry every
    // FillBuffer call (that'd be ~30 spurious OpenFileMappingW per
    // second). Counter ticked down to 0 by FillBuffer, then a new
    // attempt is allowed.
    int m_reconnectCountdown = 0;
};

}} // namespace retrocapture::dshow_filter
