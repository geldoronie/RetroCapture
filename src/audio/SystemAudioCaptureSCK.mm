#include "SystemAudioCaptureSCK.h"

// Compiled only on macOS (the CMake glob excludes it elsewhere). Guarded
// anyway for safety.
#ifdef __APPLE__

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>

#include "AudioBus.h"
#include "../utils/Logger.h"

#include <vector>

// ── delegate: pulls float PCM out of the audio sample buffers ────────
API_AVAILABLE(macos(13.0))
@interface RCSystemAudioDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    AudioBus *bus;
    uint32_t  channels;
}
@end

@implementation RCSystemAudioDelegate
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeAudio || bus == nullptr) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    AudioBufferList abl;
    CMBlockBufferRef block = nullptr;
    const OSStatus s = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, nullptr, &abl, sizeof(abl), nullptr, nullptr,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (s != noErr || !block) return;

    if (abl.mNumberBuffers == 1)
    {
        // Interleaved float already — push as-is.
        const float *src = static_cast<const float *>(abl.mBuffers[0].mData);
        const size_t n   = abl.mBuffers[0].mDataByteSize / sizeof(float);
        if (src && n) bus->push(src, n);
    }
    else
    {
        // Planar float (one buffer per channel) — interleave for the bus.
        const uint32_t ch     = abl.mNumberBuffers;
        const uint32_t frames = abl.mBuffers[0].mDataByteSize / sizeof(float);
        if (frames)
        {
            std::vector<float> inter(static_cast<size_t>(frames) * ch);
            for (uint32_t c = 0; c < ch; ++c)
            {
                const float *src = static_cast<const float *>(abl.mBuffers[c].mData);
                if (!src) continue;
                for (uint32_t f = 0; f < frames; ++f)
                    inter[static_cast<size_t>(f) * ch + c] = src[f];
            }
            bus->push(inter.data(), inter.size());
        }
    }
    CFRelease(block);
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
    SCStream             *stream   = nil;
    RCSystemAudioDelegate *delegate = nil;
};

SystemAudioCaptureSCK::SystemAudioCaptureSCK() : m_impl(new Impl) {}
SystemAudioCaptureSCK::~SystemAudioCaptureSCK() { stop(); }

bool SystemAudioCaptureSCK::start(AudioBus *bus, uint32_t sampleRate, uint32_t channels)
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
        // Minimal video — we only consume audio, but a filter/stream
        // always has a video plane. Keep it tiny and slow.
        cfg.width  = 2;
        cfg.height = 2;
        cfg.minimumFrameInterval = CMTimeMake(1, 1);

        m_impl->delegate = [[RCSystemAudioDelegate alloc] init];
        m_impl->delegate->bus      = bus;
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
