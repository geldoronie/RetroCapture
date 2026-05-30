// COM entry points + AMOVIESETUP tables for RetroCaptureVCam.dll.
//
// strmbase (in third_party/baseclasses/) provides the bulk of the
// boilerplate:
//   - DllMain → DllEntryPoint (via dllentry.cpp)
//   - DllGetClassObject + DllCanUnloadNow (via dllentry.cpp)
//   - AMovieDllRegisterServer2 (via dllsetup.cpp) — does the HKCR
//     CLSID writes AND the CLSID_VideoInputDeviceCategory entry.
//
// We supply:
//   - The g_Templates[] array + g_cTemplates so the strmbase
//     scaffolding knows what filter to expose.
//   - DllRegisterServer / DllUnregisterServer thin wrappers that
//     route to AMovieDllRegisterServer2(TRUE/FALSE).
//   - DllMain → forward to DllEntryPoint (BaseClasses convention).
//
// The .def file exports DllMain / DllGetClassObject / DllCanUnloadNow /
// DllRegisterServer / DllUnregisterServer so regsvr32 finds them.

#include "CLSIDs.h"
#include "CRetroCaptureFilter.h"

#include <streams.h>

using namespace retrocapture::dshow_filter;

// --------------------------------------------------------------------
// AMOVIESETUP tables
// --------------------------------------------------------------------

// Pin advertisement: we accept video, RGB24 only.
const AMOVIESETUP_MEDIATYPE sudOpPinTypes = {
    &MEDIATYPE_Video,
    &MEDIASUBTYPE_RGB24
};

const AMOVIESETUP_PIN sudOutputPin = {
    const_cast<LPWSTR>(L"Output"),  // pin name
    FALSE,                          // rendered?
    TRUE,                           // is output?
    FALSE,                          // zero instances allowed?
    FALSE,                          // many instances?
    &CLSID_NULL,                    // connects to which filter (none)
    nullptr,                        // connects to which pin (none)
    1,                              // # of media types
    &sudOpPinTypes
};

const AMOVIESETUP_FILTER sudFilter = {
    &CLSID_RetroCaptureVCam,
    kFilterFriendlyName,
    MERIT_DO_NOT_USE,               // graph builder: don't auto-pick us
    1,                              // # of pins
    &sudOutputPin
};

// --------------------------------------------------------------------
// g_Templates — referenced by strmbase via extern declaration in
// dllentry.cpp / dllsetup.cpp. The array's size also has to be
// reflected in g_cTemplates (also extern).
// --------------------------------------------------------------------

CFactoryTemplate g_Templates[] = {
    {
        kFilterFriendlyName,
        &CLSID_RetroCaptureVCam,
        CRetroCaptureFilter::CreateInstance,
        nullptr,                    // init function
        &sudFilter
    }
};

int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// --------------------------------------------------------------------
// DllRegisterServer / DllUnregisterServer
//
// AMovieDllRegisterServer2 walks g_Templates and writes the standard
// HKCR\CLSID keys; it also calls IFilterMapper2 to register us under
// CLSID_VideoInputDeviceCategory (because sudFilter.dwMerit is
// MERIT_DO_NOT_USE — graph builder ignores, but the category entry
// still lands so OBS et al. see the camera in their device list).
// --------------------------------------------------------------------

STDAPI DllRegisterServer()
{
    return AMovieDllRegisterServer2(TRUE);
}

STDAPI DllUnregisterServer()
{
    return AMovieDllRegisterServer2(FALSE);
}

// --------------------------------------------------------------------
// DllMain — strmbase ships DllEntryPoint with the real init logic
// (debug subsystem, BaseClasses bookkeeping). All we do is forward.
// --------------------------------------------------------------------

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved)
{
    return DllEntryPoint(hModule, dwReason, lpReserved);
}
