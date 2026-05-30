#include "CRetroCaptureStream.h"

#include "../output/VirtcamIpcLayout.h"

#include <algorithm>
#include <cstring>

namespace retrocapture { namespace dshow_filter {

namespace {

// Resolutions we advertise via GetMediaType. Order matters: index 0
// is the consumer's default pick when it doesn't explicitly
// enumerate, so the most "I just want a webcam" choice goes first.
struct Resolution
{
    int width;
    int height;
};
constexpr Resolution kOfferedResolutions[] = {
    { 1280,  720 },
    {  640,  480 },
    { 1920, 1080 },
};
constexpr int kOfferedCount = static_cast<int>(
    sizeof(kOfferedResolutions) / sizeof(kOfferedResolutions[0]));

// 30 fps everywhere — we don't currently advertise per-fps
// variants. Could expose 15 / 24 / 60 later by multiplying out
// the GetMediaType enumeration.
constexpr int kFps = 30;

// 100ns units. DirectShow's REFERENCE_TIME is 1 unit = 100ns,
// so one second = 10'000'000.
constexpr REFERENCE_TIME kTicksPerSecond  = 10000000;
constexpr REFERENCE_TIME kFrameDuration   = kTicksPerSecond / kFps;

// We only deliver RGB24 today. RGBA on the wire is converted to
// RGB24 by the writer-side libswscale path, and YUYV is too —
// the simpler the DLL the better, since we can't easily ship
// swscale here. If the writer is set to RGB24 it's a passthrough
// memcpy; otherwise, today, we fall back to the frozen frame.
constexpr int kBytesPerPixelRgb24 = 3;

inline int rgb24Bytes(int w, int h)
{
    return w * h * kBytesPerPixelRgb24;
}

// Strict-aliasing-safe BITMAPINFOHEADER setup. Width is positive,
// height is positive (the convention for sample BMPs is to set
// negative height for top-down RGB, but most DirectShow consumers
// expect positive). RGB24 in DirectShow is BGR byte order — the
// writer's RGB24 path already produces BGR.
void fillBitmapInfoHeader(BITMAPINFOHEADER &bih, int width, int height)
{
    std::memset(&bih, 0, sizeof(bih));
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = width;
    bih.biHeight      = height;
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = rgb24Bytes(width, height);
}

// Black slate — every pixel zeroed. Tiny + cache-friendly. Used
// only when the writer hasn't laid a frame down yet.
void writeBlackRgb24(uint8_t *dst, int width, int height)
{
    std::memset(dst, 0, rgb24Bytes(width, height));
}

} // namespace

CRetroCaptureStream::CRetroCaptureStream(HRESULT             *phr,
                                         CRetroCaptureFilter *pParent,
                                         LPCWSTR              pPinName)
    : CSourceStream(NAME("RetroCapture VCam Output"),
                    phr,
                    reinterpret_cast<CSource *>(pParent),
                    pPinName)
{
}

CRetroCaptureStream::~CRetroCaptureStream() = default;

// --------------------------------------------------------------------
// Media-type negotiation
// --------------------------------------------------------------------

HRESULT CRetroCaptureStream::GetMediaType(int iPosition, CMediaType *pmt)
{
    if (pmt == nullptr)
    {
        return E_POINTER;
    }
    if (iPosition < 0)
    {
        return E_INVALIDARG;
    }
    if (iPosition >= kOfferedCount)
    {
        return VFW_S_NO_MORE_ITEMS;
    }

    const Resolution &res = kOfferedResolutions[iPosition];

    auto *pvi = reinterpret_cast<VIDEOINFOHEADER *>(
        pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    if (pvi == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    std::memset(pvi, 0, sizeof(VIDEOINFOHEADER));

    fillBitmapInfoHeader(pvi->bmiHeader, res.width, res.height);
    pvi->AvgTimePerFrame = kFrameDuration;
    // dwBitRate is informational; consumers rarely read it but
    // omitting it makes some Filter Graph editors complain.
    pvi->dwBitRate       = pvi->bmiHeader.biSizeImage * 8 * kFps;
    pvi->bmiHeader.biSizeImage = rgb24Bytes(res.width, res.height);

    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetSubtype(&MEDIASUBTYPE_RGB24);
    pmt->SetFormatType(&FORMAT_VideoInfo);
    pmt->SetTemporalCompression(FALSE);
    pmt->SetSampleSize(pvi->bmiHeader.biSizeImage);

    return S_OK;
}

HRESULT CRetroCaptureStream::CheckMediaType(const CMediaType *pMediaType)
{
    if (pMediaType == nullptr)
    {
        return E_POINTER;
    }
    if (*pMediaType->Type() != MEDIATYPE_Video)
    {
        return E_INVALIDARG;
    }
    if (*pMediaType->Subtype() != MEDIASUBTYPE_RGB24)
    {
        return E_INVALIDARG;
    }
    if (*pMediaType->FormatType() != FORMAT_VideoInfo)
    {
        return E_INVALIDARG;
    }

    auto *pvi = reinterpret_cast<const VIDEOINFOHEADER *>(pMediaType->Format());
    if (pvi == nullptr)
    {
        return E_INVALIDARG;
    }

    const LONG w = pvi->bmiHeader.biWidth;
    const LONG h = pvi->bmiHeader.biHeight;
    for (int i = 0; i < kOfferedCount; ++i)
    {
        if (kOfferedResolutions[i].width  == w &&
            kOfferedResolutions[i].height == h)
        {
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

HRESULT CRetroCaptureStream::SetMediaType(const CMediaType *pmt)
{
    const HRESULT hr = CSourceStream::SetMediaType(pmt);
    if (FAILED(hr))
    {
        return hr;
    }
    auto *pvi          = reinterpret_cast<const VIDEOINFOHEADER *>(pmt->Format());
    m_width            = static_cast<int>(pvi->bmiHeader.biWidth);
    m_height           = static_cast<int>(pvi->bmiHeader.biHeight);
    m_fps              = kFps;
    m_avgTimePerFrame  = pvi->AvgTimePerFrame
                            ? pvi->AvgTimePerFrame
                            : kFrameDuration;
    m_nextSampleTime   = 0;
    m_frozenFrame.clear();
    return S_OK;
}

HRESULT CRetroCaptureStream::DecideBufferSize(IMemAllocator        *pAlloc,
                                              ALLOCATOR_PROPERTIES *pRequest)
{
    if (pAlloc == nullptr || pRequest == nullptr)
    {
        return E_POINTER;
    }

    // One buffer is enough for a live source — the consumer
    // doesn't need to queue frames. Bumping to 2 would let us
    // double-buffer at the cost of latency; not worth it for
    // a webcam-style stream.
    pRequest->cBuffers = std::max<long>(pRequest->cBuffers, 1);
    pRequest->cbBuffer = std::max<long>(pRequest->cbBuffer,
                                        rgb24Bytes(m_width, m_height));

    ALLOCATOR_PROPERTIES actual = {};
    const HRESULT hr = pAlloc->SetProperties(pRequest, &actual);
    if (FAILED(hr))
    {
        return hr;
    }
    if (actual.cbBuffer < pRequest->cbBuffer)
    {
        return E_FAIL;
    }
    return S_OK;
}

// --------------------------------------------------------------------
// FillBuffer — the hot path
// --------------------------------------------------------------------

void CRetroCaptureStream::ensureReaderOpen()
{
    if (m_reader.isOpen())
    {
        return;
    }
    if (m_reconnectCountdown > 0)
    {
        --m_reconnectCountdown;
        return;
    }
    std::string err;
    if (!m_reader.open(err))
    {
        // ~1 second cooldown at 30 fps. Avoids hammering
        // OpenFileMappingW when the host isn't running.
        m_reconnectCountdown = kFps;
    }
}

void CRetroCaptureStream::fillBlankFrame(uint8_t *dst)
{
    if (!m_frozenFrame.empty() &&
        m_frozenWidth  == m_width &&
        m_frozenHeight == m_height)
    {
        std::memcpy(dst, m_frozenFrame.data(), m_frozenFrame.size());
        return;
    }
    writeBlackRgb24(dst, m_width, m_height);
}

HRESULT CRetroCaptureStream::FillBuffer(IMediaSample *pSample)
{
    if (pSample == nullptr)
    {
        return E_POINTER;
    }

    BYTE *pBuf = nullptr;
    HRESULT hr = pSample->GetPointer(&pBuf);
    if (FAILED(hr))
    {
        return hr;
    }
    const long capacity = pSample->GetSize();
    const int  expected = rgb24Bytes(m_width, m_height);
    if (capacity < expected)
    {
        return VFW_E_BUFFER_OVERFLOW;
    }

    ensureReaderOpen();

    bool delivered = false;
    if (m_reader.isOpen())
    {
        // Wait up to one frame duration in ms (minus a small
        // margin so we don't oversleep) for a fresh frame. If
        // the host is writing at our advertised fps the event
        // will fire well within this window.
        const DWORD timeoutMs = std::max<DWORD>(1,
            static_cast<DWORD>(m_avgTimePerFrame / 10000) - 1);
        m_reader.waitFrame(timeoutMs);

        virtcam_ipc::FrameHeader fh{};
        std::vector<uint8_t>     payload;
        if (m_reader.snapshotFrame(fh, payload) &&
            fh.pixelFormat == virtcam_ipc::kPixelFormatRGB24 &&
            static_cast<int>(fh.width)  == m_width &&
            static_cast<int>(fh.height) == m_height &&
            payload.size()              == static_cast<size_t>(expected))
        {
            std::memcpy(pBuf, payload.data(), expected);
            m_frozenFrame  = std::move(payload);
            m_frozenWidth  = m_width;
            m_frozenHeight = m_height;
            delivered      = true;
        }
        else if (!m_reader.isOpen())
        {
            // Host vanished mid-snapshot — schedule retry.
            m_reconnectCountdown = kFps;
        }
    }

    if (!delivered)
    {
        fillBlankFrame(pBuf);
    }

    // Stream-clock pacing: we set timestamps relative to the
    // stream start (T=0 on the first frame, +frame_duration each
    // subsequent). Consumers using IReferenceClock will pace
    // playback off this; consumers that don't will get the
    // frames as fast as we can produce them anyway.
    REFERENCE_TIME tStart = m_nextSampleTime;
    REFERENCE_TIME tEnd   = tStart + m_avgTimePerFrame;
    m_nextSampleTime      = tEnd;
    pSample->SetTime(&tStart, &tEnd);

    pSample->SetActualDataLength(expected);
    pSample->SetSyncPoint(TRUE);
    pSample->SetDiscontinuity(FALSE);

    return S_OK;
}

}} // namespace retrocapture::dshow_filter
