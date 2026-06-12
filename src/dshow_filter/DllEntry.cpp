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
// AMovieDllRegisterServer2 writes the HKCR\CLSID\InprocServer32 keys
// (so the filter can be CoCreateInstance'd) and registers it with
// IFilterMapper2 — but under the DEFAULT category, which resolves to
// CLSID_LegacyAmFilterCategory (it passes category = 0, see
// third_party/baseclasses/dllsetup.cpp). It does NOT touch
// CLSID_VideoInputDeviceCategory, so OBS/Zoom/Teams — which enumerate
// the video-input category — never list the camera (#133). The filter
// merit has nothing to do with the category; the old comment here that
// claimed AMovieDllRegisterServer2 handled the category was wrong.
//
// So after the standard registration we ALSO register the filter under
// CLSID_VideoInputDeviceCategory ourselves — that's what makes it show
// up as a webcam. DllUnregisterServer removes that entry first, then
// undoes the standard registration.
// --------------------------------------------------------------------

namespace
{
    // Register/unregister the filter under the video-input (camera)
    // category via IFilterMapper2 so capture apps enumerate it.
    HRESULT RegisterCameraCategory(BOOL bRegister)
    {
        // regsvr32 / AMovieDllRegisterServer2 run with COM up, but init
        // defensively (refcounted) in case we're invoked standalone.
        const bool comInited = SUCCEEDED(CoInitialize(nullptr));

        IFilterMapper2 *mapper = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                      reinterpret_cast<void **>(&mapper));
        if (SUCCEEDED(hr) && mapper)
        {
            if (bRegister)
            {
                REGPINTYPES rpt = {&MEDIATYPE_Video, &MEDIASUBTYPE_RGB24};
                REGFILTERPINS rfp = {};
                rfp.strName      = const_cast<LPWSTR>(L"Output");
                rfp.bRendered    = FALSE;
                rfp.bOutput      = TRUE;
                rfp.bZero        = FALSE;
                rfp.bMany        = FALSE;
                rfp.clsConnectsToFilter = &CLSID_NULL;
                rfp.strConnectsToPin    = nullptr;
                rfp.nMediaTypes  = 1;
                rfp.lpMediaType  = &rpt;

                REGFILTER2 rf2 = {};
                rf2.dwVersion = 1;
                rf2.dwMerit   = MERIT_DO_NOT_USE;
                rf2.cPins     = 1;
                rf2.rgPins    = &rfp;

                hr = mapper->RegisterFilter(CLSID_RetroCaptureVCam,
                                            kFilterFriendlyName,
                                            nullptr, // create the moniker
                                            &CLSID_VideoInputDeviceCategory,
                                            nullptr, // instance = friendly name
                                            &rf2);
            }
            else
            {
                hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory,
                                              nullptr, CLSID_RetroCaptureVCam);
                if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                    hr = S_OK; // not registered → fine on uninstall
            }
            mapper->Release();
        }

        if (comInited)
            CoUninitialize();
        return hr;
    }
}

STDAPI DllRegisterServer()
{
    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr))
        return hr;
    return RegisterCameraCategory(TRUE);
}

STDAPI DllUnregisterServer()
{
    // Remove the camera-category entry first, then the CLSID keys.
    HRESULT hrCat = RegisterCameraCategory(FALSE);
    HRESULT hr    = AMovieDllRegisterServer2(FALSE);
    return FAILED(hr) ? hr : hrCat;
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
