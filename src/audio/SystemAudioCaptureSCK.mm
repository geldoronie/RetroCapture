#include "SystemAudioCaptureSCK.h"

// Compiled only on macOS (the CMake glob excludes it elsewhere). Guarded
// anyway for safety.
#ifdef __APPLE__

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>

#include "../utils/Logger.h"

#include <functional>
#include <vector>

// ── delegate: pulls float PCM out of the audio sample buffers ────────
API_AVAILABLE(macos(13.0))
@interface RCSystemAudioDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    // Pointer to the std::function owned by Impl (trivial ivar — avoids a
    // non-trivial C++ ivar in a non-ARC ObjC object).
    SystemAudioCaptureSCK::SampleSink *sink;
    uint32_t                           channels;
}
@end

@implementation RCSystemAudioDelegate
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeAudio || sink == nullptr || !*sink) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    static bool loggedFirst = false;
    if (!loggedFirst)
    {
        loggedFirst = true;
        LOG_INFO("SystemAudioCaptureSCK: first audio buffer received");
    }

    // SCK delivers stereo, frequently as PLANAR float (one buffer per
    // channel). A fixed `AudioBufferList abl; sizeof(abl)` only has room for
    // ONE buffer, so the extract call fails ("array too small") for planar
    // stereo and we silently dropped every real audio buffer (#109). Query
    // the needed size first, then allocate an AudioBufferList that big.
    size_t ablSize = 0;
    OSStatus s = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, &ablSize, nullptr, 0, nullptr, nullptr,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, nullptr);
    if (s != noErr || ablSize == 0) return;

    AudioBufferList *abl = static_cast<AudioBufferList *>(malloc(ablSize));
    if (!abl) return;
    CMBlockBufferRef block = nullptr;
    s = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, nullptr, abl, ablSize, nullptr, nullptr,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (s != noErr || !block) { free(abl); return; }

    static bool loggedFmt = false;
    if (!loggedFmt)
    {
        loggedFmt = true;
        LOG_INFO("SystemAudioCaptureSCK: extracted audio, buffers=" +
                 std::to_string(abl->mNumberBuffers));
    }

    // The AudioBus stores interleaved int16; SCK delivers float32. Convert
    // (clamp + scale) into int16 before pushing.
    auto toI16 = [](float f) -> int16_t {
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        return static_cast<int16_t>(f * 32767.0f);
    };

    if (abl->mNumberBuffers == 1)
    {
        // Single buffer: interleaved float for all channels.
        const float *src = static_cast<const float *>(abl->mBuffers[0].mData);
        const size_t n   = abl->mBuffers[0].mDataByteSize / sizeof(float);
        if (src && n)
        {
            std::vector<int16_t> out(n);
            for (size_t i = 0; i < n; ++i) out[i] = toI16(src[i]);
            (*sink)(out.data(), out.size());
        }
    }
    else
    {
        // Planar float (one buffer per channel) — interleave to int16.
        const uint32_t ch     = abl->mNumberBuffers;
        const uint32_t frames = abl->mBuffers[0].mDataByteSize / sizeof(float);
        if (frames)
        {
            std::vector<int16_t> inter(static_cast<size_t>(frames) * ch);
            for (uint32_t c = 0; c < ch; ++c)
            {
                const float *src = static_cast<const float *>(abl->mBuffers[c].mData);
                if (!src) continue;
                for (uint32_t f = 0; f < frames; ++f)
                    inter[static_cast<size_t>(f) * ch + c] = toI16(src[f]);
            }
            (*sink)(inter.data(), inter.size());
        }
    }
    CFRelease(block);
    free(abl);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    LOG_WARN(std::string("SystemAudioCaptureSCK: stream stopped — ") +
             (error ? error.localizedDescription.UTF8String : "?"));
}
@end

struct SystemAudioCaptureSCK::Impl
{
    SCStream              *stream   = nil;
    RCSystemAudioDelegate *delegate = nil;
    SampleSink             sink;     // owned here; delegate holds a pointer
};

SystemAudioCaptureSCK::SystemAudioCaptureSCK() : m_impl(new Impl) {}
SystemAudioCaptureSCK::~SystemAudioCaptureSCK() { stop(); }

bool SystemAudioCaptureSCK::start(SampleSink sink, uint32_t sampleRate, uint32_t channels)
{
    if (@available(macOS 13.0, *))
    {
        // Fetch shareable content (need a display for the filter). The
        // completion runs on a background queue, so blocking here is safe.
        __block SCShareableContent *content = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *c, NSError *e) {
                if (c) content = [c retain];
                (void)e;
                dispatch_semaphore_signal(sem);
            }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
                                                   (int64_t)(5 * NSEC_PER_SEC)));
        if (!content || content.displays.count == 0)
        {
            LOG_ERROR("SystemAudioCaptureSCK: no shareable content "
                      "(screen-recording permission?)");
            [content release];
            return false;
        }

        SCContentFilter *filter =
            [[SCContentFilter alloc] initWithDisplay:content.displays[0]
                                    excludingWindows:@[]];

        SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
        cfg.capturesAudio = YES;
        cfg.sampleRate    = (NSInteger)sampleRate;
        cfg.channelCount  = (NSInteger)channels;
        cfg.excludesCurrentProcessAudio = YES; // never capture ourselves
        // We only consume audio, but an SCStream always has a video plane.
        // Keep it small but valid (2x2 can be rejected and stop the stream
        // from starting) and slow.
        cfg.width  = 128;
        cfg.height = 72;
        cfg.minimumFrameInterval = CMTimeMake(1, 1);

        m_impl->sink = std::move(sink);

        m_impl->delegate = [[RCSystemAudioDelegate alloc] init];
        m_impl->delegate->sink     = &m_impl->sink;
        m_impl->delegate->channels = channels;

        m_impl->stream = [[SCStream alloc] initWithFilter:filter
                                            configuration:cfg
                                                 delegate:m_impl->delegate];
        [filter release];
        [cfg release];

        dispatch_queue_t q = dispatch_queue_create("com.retrocapture.sck.audio",
                                                   DISPATCH_QUEUE_SERIAL);
        NSError *addErr = nil;
        const BOOL added = [m_impl->stream addStreamOutput:m_impl->delegate
                                                      type:SCStreamOutputTypeAudio
                                        sampleHandlerQueue:q
                                                     error:&addErr];
        dispatch_release(q);
        if (!added)
        {
            LOG_ERROR(std::string("SystemAudioCaptureSCK: addStreamOutput(audio) failed — ") +
                      (addErr ? addErr.localizedDescription.UTF8String : "?"));
            [m_impl->stream release]; m_impl->stream = nil;
            [m_impl->delegate release]; m_impl->delegate = nil;
            [content release];
            return false;
        }

        [m_impl->stream startCaptureWithCompletionHandler:^(NSError *e) {
            if (e)
                LOG_ERROR(std::string("SystemAudioCaptureSCK: startCapture failed — ") +
                          e.localizedDescription.UTF8String);
            else
                LOG_INFO("SystemAudioCaptureSCK: capturing system audio " +
                         std::to_string(sampleRate) + " Hz x " +
                         std::to_string(channels) + " ch");
        }];

        [content release];
        return true;
    }
    LOG_ERROR("SystemAudioCaptureSCK: system-audio capture needs macOS 13+");
    return false;
}

void SystemAudioCaptureSCK::stop()
{
    if (@available(macOS 13.0, *))
    {
        if (m_impl && m_impl->stream)
        {
            [m_impl->stream stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; }];
            [m_impl->stream release];
            m_impl->stream = nil;
        }
        if (m_impl && m_impl->delegate)
        {
            [m_impl->delegate release];
            m_impl->delegate = nil;
        }
    }
}

#endif // __APPLE__
