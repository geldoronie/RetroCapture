#pragma once

// GUIDs for the RetroCapture virtual camera DirectShow filter.
//
// CLSID_RetroCaptureVCam is the COM class id consumers like OBS
// see when they enumerate `CLSID_VideoInputDeviceCategory`. It's
// fixed per-build (NOT regenerated per install) so multiple
// RetroCapture installs share the same device entry — that's the
// intended behaviour: one virtual camera per host, regardless of
// how many copies of the application a user has lying around.
//
// Picked once on the original Phase 2 design (docs/VIRTCAM_WINDOWS.md
// shows the placeholder); locked in here. Don't regenerate without
// an installer-side migration story — changing it strands old
// registry entries pointing at the previous CLSID.

#include <guiddef.h>

namespace retrocapture { namespace dshow_filter {

// {C4F2E1A0-7B3D-4F8E-9C1B-BC850000FCAB}
constexpr GUID CLSID_RetroCaptureVCam = {
    0xC4F2E1A0,
    0x7B3D,
    0x4F8E,
    { 0x9C, 0x1B, 0xBC, 0x85, 0x00, 0x00, 0xFC, 0xAB }
};

// Friendly name surfaced to the consumer — what OBS / Chrome show
// in their device picker.
constexpr wchar_t kFilterFriendlyName[] = L"RetroCapture Virtual Camera";

}} // namespace retrocapture::dshow_filter
