#pragma once

// Constants shared across the DAL plug-in TUs: the plug-in
// factory UUID (must match Info.plist's CFPlugInFactories key),
// the friendly name surfaced to CMIO consumers, the fixed output
// format we advertise.

#include <CoreFoundation/CoreFoundation.h>

namespace retrocapture { namespace dal_plugin {

// Plug-in factory UUID — also the value in Info.plist's
// CFPlugInFactories. Must NEVER change once shipped; CMIO clients
// may persist references to it.
//   5C8F2A3D-1B4E-4F9A-9C2D-DC85DA1FCA42
inline CFUUIDRef pluginFactoryUUID()
{
    return CFUUIDGetConstantUUIDWithBytes(
        nullptr,
        0x5C, 0x8F, 0x2A, 0x3D,
        0x1B, 0x4E,
        0x4F, 0x9A,
        0x9C, 0x2D,
        0xDC, 0x85, 0xDA, 0x1F, 0xCA, 0x42);
}

// Friendly name surfaced to consumers via
// kCMIODevicePropertyDeviceMaster, kCMIOObjectPropertyName, etc.
// NOT constexpr — CFSTR expands to a reinterpret_cast
// (__builtin___CFStringMakeConstantString) which isn't a constant
// expression. `const` at namespace scope (internal linkage, this
// header is only included by Plugin.mm) is the right shape: the
// CFString is a compile-time-constant section object, just not a
// C++ constant-expression.
const CFStringRef kPluginFriendlyName =
    CFSTR("RetroCapture Virtual Camera");

const CFStringRef kManufacturerName = CFSTR("RetroCapture");

// Device UID — opaque identifier consumers use to remember the
// device. Stays stable forever (same reasoning as the factory
// UUID).
const CFStringRef kDeviceUID =
    CFSTR("com.retrocapture.virtualcamera.device.v1");

const CFStringRef kModelUID =
    CFSTR("com.retrocapture.virtualcamera.model.v1");

// Fixed output geometry/format for Phase 1. Consumers can be
// taught to negotiate via kCMIOStreamPropertyFormatDescriptions
// later; today we expose ONE format and let the host-side sink
// resize whatever it has into it.
constexpr int   kStreamWidth      = 1280;
constexpr int   kStreamHeight     = 720;
constexpr int   kStreamFps        = 30;
// kCVPixelFormatType_422YpCbCr8 (== '2vuy' == 0x32767579), the
// camera-native 4:2:2 YUV format (UYVY byte order). Real webcams
// output this, and some consumers will only *activate* a capture
// session for a camera-native format. Byte order matters: '2vuy'
// is UYVY, fed from libswscale AV_PIX_FMT_UYVY422 on the host side
// (NOT YUYV). Spelled as the FourCC literal so this header needn't
// drag in <CoreVideo/CVPixelBuffer.h>.
//   History: 24RGB showed the device but no image; 32BGRA was
//   recognised (OBS showed the format) but the consumer still never
//   started the stream. Trying 2vuy as the camera-native format.
// 4:2:2 packs 2 bytes per pixel (one full Y per pixel, shared CbCr).
constexpr uint32_t kStreamCVPixelFormat = 0x32767579u; // '2vuy'
constexpr uint32_t kStreamBytesPerPixel = 2u;

}} // namespace retrocapture::dal_plugin
