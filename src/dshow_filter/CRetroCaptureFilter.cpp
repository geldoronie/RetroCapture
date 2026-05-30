#include "CRetroCaptureFilter.h"

#include "CRetroCaptureStream.h"

namespace retrocapture { namespace dshow_filter {

CUnknown *WINAPI CRetroCaptureFilter::CreateInstance(LPUNKNOWN pUnk,
                                                     HRESULT  *phr)
{
    auto *p = new CRetroCaptureFilter(pUnk, phr);
    if (p == nullptr && phr != nullptr)
    {
        *phr = E_OUTOFMEMORY;
    }
    return p;
}

CRetroCaptureFilter::CRetroCaptureFilter(LPUNKNOWN pUnk, HRESULT *phr)
    : CSource(NAME("RetroCapture VCam Filter"),
              pUnk,
              CLSID_RetroCaptureVCam)
{
    // CSource's ctor doesn't create any pins; we instantiate the
    // single output pin here. The stream registers itself with the
    // parent via its own ctor (CSourceStream -> CSource::AddPin).
    new CRetroCaptureStream(phr, this, L"Output");
}

}} // namespace retrocapture::dshow_filter
