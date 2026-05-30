#pragma once

// CSource subclass — minimal. Just constructs one CRetroCaptureStream
// in its ctor and lets the base class do the COM scaffolding. All
// the interesting work lives in the stream's FillBuffer.
//
// Instantiated by DirectShow via DllGetClassObject → CreateInstance
// when a consumer asks to bind to CLSID_RetroCaptureVCam. See
// DllEntry.cpp.

#include "CLSIDs.h"

#include <streams.h>

namespace retrocapture { namespace dshow_filter {

class CRetroCaptureFilter : public CSource
{
public:
    /// COM factory entry point referenced by the AMOVIESETUP_FILTER
    /// table in DllEntry.cpp. BaseClasses' DllGetClassObject calls
    /// this with `pUnk == nullptr` for the standard non-aggregated
    /// case.
    static CUnknown *WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT *phr);

private:
    CRetroCaptureFilter(LPUNKNOWN pUnk, HRESULT *phr);
};

}} // namespace retrocapture::dshow_filter
