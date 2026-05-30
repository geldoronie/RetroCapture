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

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace retrocapture::dal_plugin;
using retrocapture::virtcam_ipc::FrameHeader;

namespace {

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
    if (!g_plugin.reader.open(err))
    {
        // Host isn't writing yet — keep trying. The thread exits
        // only when streamActive flips false.
    }

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

OSStatus getDataSize(CMIOObjectID objectID,
                     const CMIOObjectPropertyAddress *addr,
                     UInt32 *dataSize)
{
    if (dataSize == nullptr || addr == nullptr) return kCMIOHardwareIllegalOperationError;

    switch (addr->mSelector)
    {
        case kCMIOObjectPropertyName:
        case kCMIOObjectPropertyManufacturer:
        case kCMIODevicePropertyDeviceUID:
        case kCMIODevicePropertyModelUID:
            *dataSize = sizeof(CFStringRef);
            return noErr;
        case kCMIODevicePropertyDeviceIsAlive:
            *dataSize = sizeof(UInt32);
            return noErr;
        case kCMIODevicePropertyStreams:
            *dataSize = sizeof(CMIOObjectID); // one stream
            return noErr;
        case kCMIOStreamPropertyFormatDescription:
            *dataSize = sizeof(CMFormatDescriptionRef);
            return noErr;
        default:
            return kCMIOHardwareUnknownPropertyError;
    }
    (void)objectID;
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

    switch (addr->mSelector)
    {
        case kCMIOObjectPropertyName:
        case kCMIOObjectPropertyManufacturer:
            return copyString(
                addr->mSelector == kCMIOObjectPropertyName
                    ? kPluginFriendlyName : kManufacturerName,
                cap, used, out);

        case kCMIODevicePropertyDeviceUID:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyString(kDeviceUID, cap, used, out);

        case kCMIODevicePropertyModelUID:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyString(kModelUID, cap, used, out);

        case kCMIODevicePropertyDeviceIsAlive:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyUInt32(1, cap, used, out);

        case kCMIODevicePropertyStreams:
            if (!isDevice) return kCMIOHardwareUnknownPropertyError;
            return copyObjectIDs(&g_plugin.streamID, 1, cap, used, out);

        case kCMIOStreamPropertyFormatDescription:
            if (!isStream) return kCMIOHardwareUnknownPropertyError;
            {
                CMFormatDescriptionRef fmt = ensureFormatDescription();
                if (cap < sizeof(CMFormatDescriptionRef))
                {
                    return kCMIOHardwareBadPropertySizeError;
                }
                *static_cast<CMFormatDescriptionRef *>(out) =
                    static_cast<CMFormatDescriptionRef>(CFRetain(fmt));
                if (used) *used = sizeof(CMFormatDescriptionRef);
                return noErr;
            }

        default:
            (void)isPlugin;
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
    return noErr;
}

OSStatus cbInitializeWithObjectID(CMIOHardwarePlugInRef /*self*/,
                                  CMIOObjectID objectID)
{
    g_plugin.pluginID = objectID;

    // Mint Device + Stream IDs. CMIO requires us to call
    // CMIOObjectCreate and stash the returned IDs. This part
    // is delicate on first run — the exact CMIOObjectCreate
    // overload and the parent linking are easy to get wrong;
    // verify on a real Mac. (TODO).
    g_plugin.deviceID = 0;  // TODO: CMIOObjectCreate(...)
    g_plugin.streamID = 0;  // TODO: CMIOObjectCreate(...)
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
    if (!CFEqual(requestedTypeUUID, kCMIOHardwarePlugInTypeID))
    {
        return nullptr;
    }
    g_plugin.refCount.store(1);
    return &s_interfacePtr;
}
