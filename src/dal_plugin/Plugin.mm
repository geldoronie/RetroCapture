// RetroCaptureVCam.plugin — CoreMediaIO DAL plug-in entry point
// and main plumbing.
//
// Loaded into every CMIO consumer process (OBS, Chrome legacy
// path, Discord, Zoom, ...) when those scan
// /Library/CoreMediaIO/Plug-Ins/DAL/*.plugin at startup.
//
// Object hierarchy (per CMIO conventions):
//   kCMIOObjectSystemObject
//     └── PlugIn       (this binary, one)
//           └── Device       (the virtual camera, one)
//                 └── Stream (the output stream, one)
//
// Frame flow:
//   1. Consumer calls DeviceStartStream → we spin up a frame
//      thread.
//   2. Thread loops: waitFrame(timeoutMs) → snapshotFrame() →
//      CVPixelBufferCreateWithBytes → CMIOSampleBufferCreate →
//      CMSimpleQueueEnqueue → notify the queueAlteredProc the
//      consumer registered.
//   3. DeviceStopStream → kill the thread.
//
// Known limitations of this first cut (iterate on a real Mac):
//   - Single fixed format (RGB24 @ 1280x720 30fps). Consumer
//     can't pick. The host always renders into this geometry.
//   - Property table covers the bare minimum CMIO requires
//     (Name, Manufacturer, DeviceUID, ModelUID, IsAlive, Streams
//     list, Format). Anything else returns kCMIOHardwareUnknownPropertyError
//     — most consumers tolerate that.
//   - No timestamp re-mapping. The frame thread paces on the
//     event semaphore from the host; if the host stops writing,
//     the queue stops filling and the consumer's image freezes
//     (acceptable for v1; can re-emit the frozen frame later).

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMediaIO/CMIOHardwarePlugIn.h>
#import <CoreMediaIO/CMIOSampleBuffer.h>   // CMIOSampleBufferCreateForImageBuffer + kCMIOSampleBufferNoDiscontinuities
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreVideo/CVPixelBuffer.h>

#include "PluginConstants.h"
#include "SharedMemoryReader.h"

#include <mach/mach_time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace retrocapture::dal_plugin;
using retrocapture::virtcam_ipc::FrameHeader;

namespace {

// ---------------------------------------------------------------------
// Diagnostic logging
//
// The plug-in runs INSIDE the consumer process (OBS, etc.), so we
// can't just printf to a terminal we control. Append to a fixed
// world-readable file the user can `cat` back to us. Each consumer
// that loads the plug-in writes its pid so concurrent loaders don't
// confuse the trace. This is debugging scaffolding for the first
// real bring-up; once the camera works end-to-end we can switch to
// os_log or strip it.
// ---------------------------------------------------------------------
void vclog(const char *fmt, ...)
{
    FILE *f = std::fopen("/tmp/RetroCaptureVCam.log", "a");
    if (f == nullptr) return;
    std::fprintf(f, "[pid %d] ", getpid());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// One global instance — DAL only ever instantiates one plug-in
// per loaded bundle. Keeps the C-callback world simple (no per-
// instance dispatch tables).
struct PluginState
{
    // Refcount for IUnknown semantics.
    std::atomic<unsigned long> refCount{1};

    // CMIO IDs assigned by InitializeWithObjectID + the synthetic
    // children we expose. The PlugIn ID comes from CMIO; the
    // Device/Stream IDs we mint ourselves using
    // CMIOObjectCreate (allowed for plug-in-owned objects).
    CMIOObjectID pluginID = 0;
    CMIODeviceID deviceID = 0;
    CMIOStreamID streamID = 0;

    // The queue the consumer registers in StreamCopyBufferQueue.
    // We enqueue CMSampleBufferRef into it whenever a new frame
    // arrives. The consumer reads via CMSimpleQueueDequeue.
    CMSimpleQueueRef                  bufferQueue          = nullptr;
    CMIODeviceStreamQueueAlteredProc  queueAlteredProc     = nullptr;
    void                             *queueAlteredRefCon  = nullptr;

    // Cached format description for the stream — built lazily on
    // first frame. CMVideoFormatDescriptionCreate reads the
    // pixel format + dims; we recreate only on geometry change.
    CMFormatDescriptionRef formatDescription = nullptr;

    // CMIO stream clock — CMIO needs a clock per stream to pace
    // playback. Created in InitializeWithObjectID, handed to the
    // consumer via kCMIOStreamPropertyClock, advanced once per
    // delivered frame via CMIOStreamClockPostTimingEvent.
    CFTypeRef streamClock = nullptr;

    // Frame thread state. Spun up by DeviceStartStream, torn
    // down by DeviceStopStream.
    std::atomic<bool> streamActive{false};
    std::thread       frameThread;
    SharedMemoryReader reader;

    // Monotonic frame counter for CMSampleBuffer timestamps.
    uint64_t frameSequence = 0;
};

PluginState g_plugin;

// ---------------------------------------------------------------------
// Frame thread
// ---------------------------------------------------------------------

CMFormatDescriptionRef ensureFormatDescription()
{
    if (g_plugin.formatDescription != nullptr)
    {
        return g_plugin.formatDescription;
    }
    CMVideoFormatDescriptionCreate(
        kCFAllocatorDefault,
        kStreamCVPixelFormat,
        kStreamWidth, kStreamHeight,
        nullptr,
        &g_plugin.formatDescription);
    return g_plugin.formatDescription;
}

// Build a CVPixelBuffer from raw bytes the host just delivered.
// kCVPixelBufferBytesPerRowAlignmentKey isn't worth fiddling
// with for a fixed 1280x720 RGB24 — caller already passes a
// tightly-packed buffer.
CVPixelBufferRef makePixelBuffer(const uint8_t *bytes, size_t /*bytesLen*/)
{
    CVPixelBufferRef pb = nullptr;
    const size_t bytesPerRow = static_cast<size_t>(kStreamWidth) * 3;
    CVPixelBufferCreateWithBytes(
        kCFAllocatorDefault,
        kStreamWidth, kStreamHeight,
        kStreamCVPixelFormat,
        const_cast<uint8_t *>(bytes),
        bytesPerRow,
        /*releaseCallback=*/nullptr,
        /*releaseRefCon=*/nullptr,
        /*pixelBufferAttributes=*/nullptr,
        &pb);
    return pb;
}

// Wrap a CVPixelBuffer in a CMSampleBuffer with sane timing
// metadata, then enqueue into the consumer's queue. Returns
// false if the queue is full (consumer is behind — we drop).
bool enqueueFrame(CVPixelBufferRef pb)
{
    if (g_plugin.bufferQueue == nullptr) return false;

    CMFormatDescriptionRef fmt = ensureFormatDescription();
    if (fmt == nullptr) return false;

    // 30 fps → 1/30 s per frame, in CMTime ticks of 1/1000000.
    const CMTime duration = CMTimeMake(1, kStreamFps);
    const CMTime pts      = CMTimeMake(
        static_cast<int64_t>(g_plugin.frameSequence), kStreamFps);
    CMSampleTimingInfo timing = { duration, pts, kCMTimeInvalid };

    CMSampleBufferRef sb = nullptr;
    OSStatus s = CMIOSampleBufferCreateForImageBuffer(
        kCFAllocatorDefault,
        pb,
        fmt,
        &timing,
        g_plugin.frameSequence,
        kCMIOSampleBufferNoDiscontinuities,
        &sb);
    if (s != noErr || sb == nullptr)
    {
        return false;
    }

    OSStatus enqRc = CMSimpleQueueEnqueue(g_plugin.bufferQueue, sb);
    if (enqRc != noErr)
    {
        CFRelease(sb);
        return false;
    }

    // Advance the stream clock so the consumer's IReferenceClock-
    // equivalent paces correctly. resynchronize on the very first
    // frame to anchor the timeline to "now".
    if (g_plugin.streamClock != nullptr)
    {
        CMIOStreamClockPostTimingEvent(
            pts,
            mach_absolute_time(),
            /*resynchronize=*/(g_plugin.frameSequence == 0),
            g_plugin.streamClock);
    }

    // Notify the consumer there's something new in the queue.
    // The proc is registered when the consumer called
    // StreamCopyBufferQueue.
    if (g_plugin.queueAlteredProc != nullptr)
    {
        g_plugin.queueAlteredProc(
            g_plugin.streamID, sb, g_plugin.queueAlteredRefCon);
    }
    return true;
}

void frameThreadProc()
{
    std::string err;
    const bool opened = g_plugin.reader.open(err);
    vclog("frameThread start: reader.open=%d (%s)",
          opened ? 1 : 0, opened ? "ok" : err.c_str());
    bool loggedFirstFrame = false;

    while (g_plugin.streamActive.load())
    {
        if (!g_plugin.reader.isOpen())
        {
            // Reconnect attempt every ~1s.
            (void)g_plugin.reader.open(err);
            if (!g_plugin.reader.isOpen())
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        // Wait up to one frame duration in ms for the host's
        // sem_post. If timeout, fall through with a re-emit of
        // the last good frame (TODO once we keep a frozen copy).
        const uint32_t timeoutMs = 1000 / kStreamFps + 1;
        if (!g_plugin.reader.waitFrame(timeoutMs))
        {
            continue;
        }

        FrameHeader              fh{};
        std::vector<uint8_t>     payload;
        if (!g_plugin.reader.snapshotFrame(fh, payload))
        {
            continue;
        }

        // We only support RGB24 today. If the writer is in another
        // format, skip — the consumer's screen will freeze on the
        // last delivered frame, which is acceptable for v1.
        if (fh.pixelFormat != retrocapture::virtcam_ipc::kPixelFormatRGB24 ||
            static_cast<int>(fh.width)  != kStreamWidth ||
            static_cast<int>(fh.height) != kStreamHeight)
        {
            if (!loggedFirstFrame)
            {
                vclog("frameThread: got frame but mismatch "
                      "(fmt=%u %ux%u; want fmt=%u %dx%d) — skipping",
                      fh.pixelFormat, fh.width, fh.height,
                      retrocapture::virtcam_ipc::kPixelFormatRGB24,
                      kStreamWidth, kStreamHeight);
                loggedFirstFrame = true;
            }
            continue;
        }

        CVPixelBufferRef pb = makePixelBuffer(payload.data(), payload.size());
        if (pb == nullptr)
        {
            continue;
        }
        const bool ok = enqueueFrame(pb);
        CFRelease(pb);
        if (ok)
        {
            if (!loggedFirstFrame)
            {
                vclog("frameThread: first frame enqueued OK (%dx%d)",
                      kStreamWidth, kStreamHeight);
                loggedFirstFrame = true;
            }
            ++g_plugin.frameSequence;
        }
    }

    g_plugin.reader.close();
}

// ---------------------------------------------------------------------
// Property handlers
// ---------------------------------------------------------------------

// Helpers: copy data of various types into the caller's buffer
// per CMIO's GetPropertyData contract.
OSStatus copyString(CFStringRef value, UInt32 cap, UInt32 *used, void *out)
{
    if (cap < sizeof(CFStringRef)) return kCMIOHardwareBadPropertySizeError;
    *static_cast<CFStringRef *>(out) =
        static_cast<CFStringRef>(CFRetain(value));
    if (used != nullptr) *used = sizeof(CFStringRef);
    return noErr;
}

OSStatus copyUInt32(UInt32 value, UInt32 cap, UInt32 *used, void *out)
{
    if (cap < sizeof(UInt32)) return kCMIOHardwareBadPropertySizeError;
    *static_cast<UInt32 *>(out) = value;
    if (used != nullptr) *used = sizeof(UInt32);
    return noErr;
}

OSStatus copyObjectIDs(const CMIOObjectID *ids, UInt32 count,
                       UInt32 cap, UInt32 *used, void *out)
{
    const UInt32 needed = count * sizeof(CMIOObjectID);
    if (cap < needed) return kCMIOHardwareBadPropertySizeError;
    std::memcpy(out, ids, needed);
    if (used != nullptr) *used = needed;
    return noErr;
}

OSStatus copyInt32(int32_t value, UInt32 cap, UInt32 *used, void *out)
{
    if (cap < sizeof(int32_t)) return kCMIOHardwareBadPropertySizeError;
    *static_cast<int32_t *>(out) = value;
    if (used != nullptr) *used = sizeof(int32_t);
    return noErr;
}

OSStatus copyFloat64(Float64 value, UInt32 cap, UInt32 *used, void *out)
{
    if (cap < sizeof(Float64)) return kCMIOHardwareBadPropertySizeError;
    *static_cast<Float64 *>(out) = value;
    if (used != nullptr) *used = sizeof(Float64);
    return noErr;
}

// Hand the caller a +1 CFTypeRef (CFString / CMFormatDescription /
// clock / CFArray). Caller releases per CMIO's "Copy" contract.
OSStatus copyCFType(CFTypeRef value, UInt32 cap, UInt32 *used, void *out)
{
    if (value == nullptr) return kCMIOHardwareUnknownPropertyError;
    if (cap < sizeof(CFTypeRef)) return kCMIOHardwareBadPropertySizeError;
    *static_cast<CFTypeRef *>(out) = CFRetain(value);
    if (used != nullptr) *used = sizeof(CFTypeRef);
    return noErr;
}

OSStatus copyAudioValueRange(Float64 lo, Float64 hi,
                             UInt32 cap, UInt32 *used, void *out)
{
    if (cap < sizeof(AudioValueRange)) return kCMIOHardwareBadPropertySizeError;
    AudioValueRange r = { lo, hi };
    std::memcpy(out, &r, sizeof(r));
    if (used != nullptr) *used = sizeof(AudioValueRange);
    return noErr;
}

// A single-element CFArray holding the stream's format description.
// Built lazily, cached for the process lifetime (the format never
// changes in this first cut).
CFArrayRef formatDescriptionsArray(CMFormatDescriptionRef fmt)
{
    static CFArrayRef s_arr = nullptr;
    if (s_arr == nullptr && fmt != nullptr)
    {
        const void *vals[1] = { fmt };
        s_arr = CFArrayCreate(kCFAllocatorDefault, vals, 1,
                              &kCFTypeArrayCallBacks);
    }
    return s_arr;
}

// 30 fps as a Float64 — used for the frame-rate properties.
constexpr Float64 kFps64 = static_cast<Float64>(kStreamFps);

OSStatus getDataSize(CMIOObjectID objectID,
                     const CMIOObjectPropertyAddress *addr,
                     UInt32 *dataSize)
{
    if (dataSize == nullptr || addr == nullptr) return kCMIOHardwareIllegalOperationError;

    const bool isPlugin = (objectID == g_plugin.pluginID);
    const bool isDevice = (objectID == g_plugin.deviceID);
    const bool isStream = (objectID == g_plugin.streamID);

    switch (addr->mSelector)
    {
        case kCMIOObjectPropertyName:
        case kCMIOObjectPropertyManufacturer:
        case kCMIODevicePropertyDeviceUID:
        case kCMIODevicePropertyModelUID:
            *dataSize = sizeof(CFStringRef);
            return noErr;

        case kCMIOObjectPropertyClass:
            *dataSize = sizeof(CMIOClassID);
            return noErr;

        case kCMIOObjectPropertyOwner:
            *dataSize = sizeof(CMIOObjectID);
            return noErr;

        case kCMIOObjectPropertyOwnedObjects:
            // plugin owns 1 device; device owns 1 stream; stream owns 0.
            *dataSize = (isStream ? 0u : 1u) * sizeof(CMIOObjectID);
            return noErr;

        case kCMIODevicePropertyStreams:
            *dataSize = (isDevice ? 1u : 0u) * sizeof(CMIOObjectID);
            return noErr;

        case kCMIODevicePropertyDeviceIsAlive:
        case kCMIODevicePropertyDeviceIsRunning:
        case kCMIODevicePropertyDeviceIsRunningSomewhere:
        case kCMIODevicePropertyExcludeNonDALAccess:
        case kCMIODevicePropertyCanProcessAVCCommand:
        case kCMIODevicePropertyCanProcessRS422Command:
        case kCMIODevicePropertyLatency:
        case kCMIOStreamPropertyDirection:
            *dataSize = sizeof(UInt32);
            return noErr;

        case kCMIODevicePropertyHogMode:
        case kCMIODevicePropertyDeviceMaster:
            *dataSize = sizeof(pid_t);
            return noErr;

        case kCMIOStreamPropertyFrameRate:
        case kCMIOStreamPropertyMinimumFrameRate:
            *dataSize = sizeof(Float64);
            return noErr;

        case kCMIOStreamPropertyFrameRates:
        case kCMIOStreamPropertyFrameRateRanges:
            *dataSize = sizeof(AudioValueRange);
            return noErr;

        case kCMIOStreamPropertyFormatDescription:
        case kCMIOStreamPropertyFormatDescriptions:
        case kCMIOStreamPropertyClock:
            *dataSize = sizeof(CFTypeRef);
            return noErr;

        default:
            (void)isPlugin;
            return kCMIOHardwareUnknownPropertyError;
    }
}

OSStatus getData(CMIOObjectID objectID,
                 const CMIOObjectPropertyAddress *addr,
                 UInt32 cap, UInt32 *used, void *out)
{
    if (addr == nullptr || out == nullptr)
    {
        return kCMIOHardwareIllegalOperationError;
    }

    const bool isPlugin = (objectID == g_plugin.pluginID);
    const bool isDevice = (objectID == g_plugin.deviceID);
    const bool isStream = (objectID == g_plugin.streamID);
    const UInt32 running = g_plugin.streamActive.load() ? 1u : 0u;

    switch (addr->mSelector)
    {
        // ---- base CMIOObject properties (all objects) ----------------
        case kCMIOObjectPropertyName:
            return copyCFType(kPluginFriendlyName, cap, used, out);

        case kCMIOObjectPropertyManufacturer:
            return copyCFType(kManufacturerName, cap, used, out);

        case kCMIOObjectPropertyClass:
            if (isDevice) return copyUInt32(kCMIODeviceClassID, cap, used, out);
            if (isStream) return copyUInt32(kCMIOStreamClassID, cap, used, out);
            if (isPlugin) return copyUInt32(kCMIOPlugInClassID, cap, used, out);
            return kCMIOHardwareUnknownPropertyError;

        case kCMIOObjectPropertyOwner:
            // device owned by system object; stream owned by device;
            // plugin owned by system object.
            if (isDevice || isPlugin)
            {
                const CMIOObjectID sys = kCMIOObjectSystemObject;
                return copyObjectIDs(&sys, 1, cap, used, out);
            }
            if (isStream)
                return copyObjectIDs(&g_plugin.deviceID, 1, cap, used, out);
            return kCMIOHardwareUnknownPropertyError;

        case kCMIOObjectPropertyOwnedObjects:
            if (isPlugin)
                return copyObjectIDs(&g_plugin.deviceID, 1, cap, used, out);
            if (isDevice)
                return copyObjectIDs(&g_plugin.streamID, 1, cap, used, out);
            // stream owns nothing.
            if (used != nullptr) *used = 0;
            return noErr;

        // ---- device properties ---------------------------------------
        case kCMIODevicePropertyDeviceUID:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyCFType(kDeviceUID, cap, used, out);

        case kCMIODevicePropertyModelUID:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyCFType(kModelUID, cap, used, out);

        case kCMIODevicePropertyDeviceIsAlive:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(1, cap, used, out);

        case kCMIODevicePropertyDeviceIsRunning:
        case kCMIODevicePropertyDeviceIsRunningSomewhere:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(running, cap, used, out);

        case kCMIODevicePropertyExcludeNonDALAccess:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(0, cap, used, out);

        case kCMIODevicePropertyCanProcessAVCCommand:
        case kCMIODevicePropertyCanProcessRS422Command:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(0, cap, used, out);

        case kCMIODevicePropertyLatency:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(0, cap, used, out);

        case kCMIODevicePropertyHogMode:
        case kCMIODevicePropertyDeviceMaster:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            // -1 == not hogged / no master.
            return copyInt32(-1, cap, used, out);

        case kCMIODevicePropertyStreams:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyObjectIDs(&g_plugin.streamID, 1, cap, used, out);

        // ---- stream properties ---------------------------------------
        case kCMIOStreamPropertyDirection:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            // 0 = output (device produces data the consumer reads).
            // If the device shows up but isn't usable as a camera,
            // this is the first thing to try flipping to 1.
            return copyUInt32(0, cap, used, out);

        case kCMIOStreamPropertyFormatDescription:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            return copyCFType(ensureFormatDescription(), cap, used, out);

        case kCMIOStreamPropertyFormatDescriptions:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            return copyCFType(
                formatDescriptionsArray(ensureFormatDescription()),
                cap, used, out);

        case kCMIOStreamPropertyFrameRate:
        case kCMIOStreamPropertyMinimumFrameRate:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            return copyFloat64(kFps64, cap, used, out);

        case kCMIOStreamPropertyFrameRates:
        case kCMIOStreamPropertyFrameRateRanges:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            return copyAudioValueRange(kFps64, kFps64, cap, used, out);

        case kCMIOStreamPropertyClock:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            return copyCFType(g_plugin.streamClock, cap, used, out);

        default:
            // Log only misses on OUR device/stream — a miss there can
            // be why a consumer rejects the device. System-object
            // misses are normal and would just spam.
            if (isDevice || isStream)
            {
                const uint32_t s = addr->mSelector;
                vclog("getData MISS on %s: selector '%c%c%c%c'",
                      isDevice ? "device" : "stream",
                      (char)((s >> 24) & 0xff), (char)((s >> 16) & 0xff),
                      (char)((s >> 8) & 0xff), (char)(s & 0xff));
            }
            return kCMIOHardwareUnknownPropertyError;
    }
}

// ---------------------------------------------------------------------
// CMIO plug-in interface callbacks
// ---------------------------------------------------------------------

HRESULT cbQueryInterface(void *self, REFIID iid, LPVOID *ppv)
{
    if (ppv == nullptr) return E_POINTER;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(nullptr, iid);
    const Boolean match =
        CFEqual(requested, kCMIOHardwarePlugInInterfaceID) ||
        CFEqual(requested, IUnknownUUID);
    CFRelease(requested);
    if (match)
    {
        ++g_plugin.refCount;
        *ppv = self;
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG cbAddRef(void * /*self*/)
{
    return static_cast<ULONG>(++g_plugin.refCount);
}

ULONG cbRelease(void *self)
{
    const auto n = --g_plugin.refCount;
    if (n == 0)
    {
        // The plug-in instance was allocated by
        // CMIOHardwarePluginRefCreateInstance; the interface
        // table itself is static so we just zero our pointer
        // back at the caller. CMIO usually keeps the plug-in
        // alive for the lifetime of the process anyway.
        (void)self;
    }
    return static_cast<ULONG>(n);
}

OSStatus cbInitialize(CMIOHardwarePlugInRef /*self*/)
{
    vclog("cbInitialize called");
    return noErr;
}

OSStatus cbInitializeWithObjectID(CMIOHardwarePlugInRef self,
                                  CMIOObjectID objectID)
{
    g_plugin.pluginID = objectID;
    vclog("cbInitializeWithObjectID: pluginID=%u", (unsigned)objectID);

    // Mint the Device as a child of the system object, then the
    // Stream as a child of the Device. CMIOObjectCreate registers
    // the object with the CMIO runtime and assigns its id; we then
    // publish them so consumers enumerating the system object see
    // the device and, drilling in, its stream.
    OSStatus err = CMIOObjectCreate(self, kCMIOObjectSystemObject,
                                    kCMIODeviceClassID, &g_plugin.deviceID);
    vclog("  CMIOObjectCreate(device) err=%d id=%u",
          (int)err, (unsigned)g_plugin.deviceID);
    if (err != noErr)
    {
        return err;
    }
    err = CMIOObjectCreate(self, g_plugin.deviceID,
                           kCMIOStreamClassID, &g_plugin.streamID);
    vclog("  CMIOObjectCreate(stream) err=%d id=%u",
          (int)err, (unsigned)g_plugin.streamID);
    if (err != noErr)
    {
        return err;
    }

    // Stream clock — paces playback for the consumer. minimum time
    // between getTime calls = one frame; light rate smoothing.
    OSStatus clkErr = CMIOStreamClockCreate(
        kCFAllocatorDefault,
        CFSTR("RetroCaptureVCamClock"),
        /*sourceIdentifier=*/&g_plugin,            // any unique ptr
        CMTimeMake(1, kStreamFps),
        /*numberOfEventsForRateSmoothing=*/10,
        /*numberOfAveragesForRateSmoothing=*/4,
        &g_plugin.streamClock);
    vclog("  CMIOStreamClockCreate err=%d clock=%p",
          (int)clkErr, (void *)g_plugin.streamClock);

    // Publish: device under the system object, stream under the
    // device. This is what makes them visible to consumers.
    OSStatus pubD = CMIOObjectsPublishedAndDied(self, kCMIOObjectSystemObject,
                                1, &g_plugin.deviceID, 0, nullptr);
    OSStatus pubS = CMIOObjectsPublishedAndDied(self, g_plugin.deviceID,
                                1, &g_plugin.streamID, 0, nullptr);
    vclog("  published device(err=%d) stream(err=%d)", (int)pubD, (int)pubS);
    return noErr;
}

OSStatus cbTeardown(CMIOHardwarePlugInRef /*self*/)
{
    if (g_plugin.streamActive.load())
    {
        g_plugin.streamActive.store(false);
        if (g_plugin.frameThread.joinable())
        {
            g_plugin.frameThread.join();
        }
    }
    if (g_plugin.formatDescription != nullptr)
    {
        CFRelease(g_plugin.formatDescription);
        g_plugin.formatDescription = nullptr;
    }
    if (g_plugin.streamClock != nullptr)
    {
        CMIOStreamClockInvalidate(g_plugin.streamClock);
        CFRelease(g_plugin.streamClock);
        g_plugin.streamClock = nullptr;
    }
    return noErr;
}

void cbObjectShow(CMIOHardwarePlugInRef /*self*/, CMIOObjectID /*objectID*/)
{
}

Boolean cbObjectHasProperty(CMIOHardwarePlugInRef /*self*/,
                            CMIOObjectID objectID,
                            const CMIOObjectPropertyAddress *addr)
{
    UInt32 dummy = 0;
    return getDataSize(objectID, addr, &dummy) == noErr;
}

OSStatus cbObjectIsPropertySettable(CMIOHardwarePlugInRef /*self*/,
                                    CMIOObjectID /*objectID*/,
                                    const CMIOObjectPropertyAddress * /*addr*/,
                                    Boolean *isSettable)
{
    if (isSettable != nullptr) *isSettable = false;
    return noErr;
}

OSStatus cbObjectGetPropertyDataSize(CMIOHardwarePlugInRef /*self*/,
                                     CMIOObjectID objectID,
                                     const CMIOObjectPropertyAddress *addr,
                                     UInt32 /*qSize*/, const void * /*q*/,
                                     UInt32 *dataSize)
{
    return getDataSize(objectID, addr, dataSize);
}

OSStatus cbObjectGetPropertyData(CMIOHardwarePlugInRef /*self*/,
                                 CMIOObjectID objectID,
                                 const CMIOObjectPropertyAddress *addr,
                                 UInt32 /*qSize*/, const void * /*q*/,
                                 UInt32 dataSize, UInt32 *dataUsed, void *data)
{
    return getData(objectID, addr, dataSize, dataUsed, data);
}

OSStatus cbObjectSetPropertyData(CMIOHardwarePlugInRef /*self*/,
                                 CMIOObjectID /*objectID*/,
                                 const CMIOObjectPropertyAddress * /*addr*/,
                                 UInt32 /*qSize*/, const void * /*q*/,
                                 UInt32 /*dataSize*/, const void * /*data*/)
{
    return kCMIOHardwareIllegalOperationError;
}

OSStatus cbDeviceSuspend(CMIOHardwarePlugInRef /*self*/, CMIODeviceID /*d*/) { return noErr; }
OSStatus cbDeviceResume (CMIOHardwarePlugInRef /*self*/, CMIODeviceID /*d*/) { return noErr; }

OSStatus cbDeviceStartStream(CMIOHardwarePlugInRef /*self*/,
                             CMIODeviceID /*d*/, CMIOStreamID /*s*/)
{
    vclog("cbDeviceStartStream");
    if (g_plugin.streamActive.exchange(true))
    {
        return noErr;
    }
    g_plugin.frameSequence = 0;
    g_plugin.frameThread   = std::thread(frameThreadProc);
    return noErr;
}

OSStatus cbDeviceStopStream(CMIOHardwarePlugInRef /*self*/,
                            CMIODeviceID /*d*/, CMIOStreamID /*s*/)
{
    g_plugin.streamActive.store(false);
    if (g_plugin.frameThread.joinable())
    {
        g_plugin.frameThread.join();
    }
    return noErr;
}

OSStatus cbDeviceProcessAVCCommand(CMIOHardwarePlugInRef /*self*/,
                                   CMIODeviceID /*d*/,
                                   CMIODeviceAVCCommand * /*cmd*/)
{
    return kCMIOHardwareIllegalOperationError;
}

OSStatus cbDeviceProcessRS422Command(CMIOHardwarePlugInRef /*self*/,
                                     CMIODeviceID /*d*/,
                                     CMIODeviceRS422Command * /*cmd*/)
{
    return kCMIOHardwareIllegalOperationError;
}

OSStatus cbStreamCopyBufferQueue(CMIOHardwarePlugInRef /*self*/,
                                 CMIOStreamID /*s*/,
                                 CMIODeviceStreamQueueAlteredProc alteredProc,
                                 void *refCon,
                                 CMSimpleQueueRef *queue)
{
    vclog("cbStreamCopyBufferQueue");
    if (g_plugin.bufferQueue == nullptr)
    {
        // 30 entries = 1s of headroom at 30fps. If the consumer
        // stalls longer than that we drop the oldest frames.
        CMSimpleQueueCreate(kCFAllocatorDefault, 30, &g_plugin.bufferQueue);
    }
    g_plugin.queueAlteredProc    = alteredProc;
    g_plugin.queueAlteredRefCon  = refCon;
    if (queue != nullptr)
    {
        // Hand the consumer a +1 reference. CFRetain returns a
        // CFTypeRef (const void*); assigning the already-typed
        // member avoids the cast-away-qualifiers error a
        // static_cast on the CFRetain result would trip.
        CFRetain(g_plugin.bufferQueue);
        *queue = g_plugin.bufferQueue;
    }
    return noErr;
}

OSStatus cbStreamDeckPlay   (CMIOHardwarePlugInRef /*self*/, CMIOStreamID /*s*/) { return noErr; }
OSStatus cbStreamDeckStop   (CMIOHardwarePlugInRef /*self*/, CMIOStreamID /*s*/) { return noErr; }
OSStatus cbStreamDeckJog    (CMIOHardwarePlugInRef /*self*/, CMIOStreamID /*s*/, SInt32 /*sp*/) { return noErr; }
OSStatus cbStreamDeckCueTo  (CMIOHardwarePlugInRef /*self*/, CMIOStreamID /*s*/, Float64 /*f*/, Boolean /*p*/) { return noErr; }

// Static interface table — what CMIO holds onto after we hand it
// out from PluginCreateInstance. All callbacks above are
// referenced here in the order CMIO expects.
CMIOHardwarePlugInInterface s_interface = {
    nullptr,
    cbQueryInterface,
    cbAddRef,
    cbRelease,
    cbInitialize,
    cbInitializeWithObjectID,
    cbTeardown,
    cbObjectShow,
    cbObjectHasProperty,
    cbObjectIsPropertySettable,
    cbObjectGetPropertyDataSize,
    cbObjectGetPropertyData,
    cbObjectSetPropertyData,
    cbDeviceSuspend,
    cbDeviceResume,
    cbDeviceStartStream,
    cbDeviceStopStream,
    cbDeviceProcessAVCCommand,
    cbDeviceProcessRS422Command,
    cbStreamCopyBufferQueue,
    cbStreamDeckPlay,
    cbStreamDeckStop,
    cbStreamDeckJog,
    cbStreamDeckCueTo,
};

// CMIO actually wants a pointer-to-pointer (the COM-style
// `**self` indirection). Wrap once.
CMIOHardwarePlugInInterface *s_interfacePtr = &s_interface;

} // namespace

// =====================================================================
// PluginCreateInstance — the C entry point referenced by Info.plist's
// CFPlugInFactories. CMIO's DAL loader calls this with the requested
// type UUID; we return the interface ref or NULL.
// =====================================================================

extern "C" __attribute__((visibility("default")))
void *PluginCreateInstance(CFAllocatorRef /*allocator*/,
                           CFUUIDRef       requestedTypeUUID)
{
    const bool typeMatch = CFEqual(requestedTypeUUID, kCMIOHardwarePlugInTypeID);
    vclog("PluginCreateInstance called, typeMatch=%d", typeMatch ? 1 : 0);
    if (!typeMatch)
    {
        return nullptr;
    }
    g_plugin.refCount.store(1);
    return &s_interfacePtr;
}
