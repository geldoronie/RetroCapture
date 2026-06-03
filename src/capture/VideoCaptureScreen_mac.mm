#include "ScreenBackend.h"

// macOS screen-capture backend: ScreenCaptureKit (macOS 12.3+). Enumerates
// displays + windows, captures a display (or window) as BGRA CMSampleBuffers
// and hands them to the cross-platform sink (which crops + GPU-uploads).
// Compiled only when RETROCAPTURE_SCREEN_SCK is set (macOS); the test
// pattern stub stands in otherwise. The project builds Objective-C without
// ARC, so memory is managed manually (retain/release).
#ifdef RETROCAPTURE_SCREEN_SCK

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>

#include "../utils/Logger.h"
#include "../audio/SckSystemAudioHub.h" // #109 route screen audio → system-audio

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ── delegate: receives sample buffers, forwards BGRA to the C++ sink ──
API_AVAILABLE(macos(12.3))
@interface RCScreenDelegate : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    IScreenFrameSink *sink;
}
@end

@implementation RCScreenDelegate
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    // #109 — audio rides on the same SCStream (running a second stream
    // just for audio makes it go silent). Forward it to the hub, which
    // hands it to the system-audio source.
    if (type == SCStreamOutputTypeAudio)
    {
        static bool loggedFirstScreenAudio = false;
        if (!loggedFirstScreenAudio)
        {
            loggedFirstScreenAudio = true;
            LOG_INFO("VideoCaptureScreen(sck): first audio buffer received from screen stream");
        }
        AudioBufferList abl;
        CMBlockBufferRef block = nullptr;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                sampleBuffer, nullptr, &abl, sizeof(abl), nullptr, nullptr,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) == noErr &&
            block)
        {
            auto toI16 = [](float f) -> int16_t {
                if (f >  1.0f) f =  1.0f;
                if (f < -1.0f) f = -1.0f;
                return static_cast<int16_t>(f * 32767.0f);
            };
            if (abl.mNumberBuffers == 1)
            {
                const float *src = static_cast<const float *>(abl.mBuffers[0].mData);
                const size_t n   = abl.mBuffers[0].mDataByteSize / sizeof(float);
                if (src && n)
                {
                    std::vector<int16_t> out(n);
                    for (size_t i = 0; i < n; ++i) out[i] = toI16(src[i]);
                    SckSystemAudioHub::instance().onScreenAudio(out.data(), out.size());
                }
            }
            else
            {
                const uint32_t ch     = abl.mNumberBuffers;
                const uint32_t frames = abl.mBuffers[0].mDataByteSize / sizeof(float);
                if (frames)
                {
                    std::vector<int16_t> inter(static_cast<size_t>(frames) * ch);
                    for (uint32_t c = 0; c < ch; ++c)
                    {
                        const float *src = static_cast<const float *>(abl.mBuffers[c].mData);
                        if (!src) continue;
                        for (uint32_t f = 0; f < frames; ++f)
                            inter[static_cast<size_t>(f) * ch + c] = toI16(src[f]);
                    }
                    SckSystemAudioHub::instance().onScreenAudio(inter.data(), inter.size());
                }
            }
            CFRelease(block);
        }
        return;
    }

    if (type != SCStreamOutputTypeScreen || sink == nullptr) return;

    CVImageBufferRef px = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!px) return;

    CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(px));
    const uint32_t w      = static_cast<uint32_t>(CVPixelBufferGetWidth(px));
    const uint32_t h      = static_cast<uint32_t>(CVPixelBufferGetHeight(px));
    const uint32_t stride = static_cast<uint32_t>(CVPixelBufferGetBytesPerRow(px));
    if (base && w && h)
    {
        static bool loggedDims = false;
        if (!loggedDims)
        {
            loggedDims = true;
            LOG_INFO("VideoCaptureScreen(sck): delivering " + std::to_string(w) + "x" +
                     std::to_string(h) + " (stride " + std::to_string(stride) + ")");
        }
        sink->onScreenFrame(base, w, h, stride, ScreenPixelFormat::BGRA);
    }
    CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    LOG_WARN(std::string("VideoCaptureScreen(sck): stream stopped — ") +
             (error ? error.localizedDescription.UTF8String : "?"));
}
@end

namespace
{
// Fetch the shareable content synchronously (the API is async).
SCShareableContent *fetchContentBlocking() API_AVAILABLE(macos(12.3))
{
    __block SCShareableContent *result = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent *content, NSError *error) {
            if (content) result = [content retain]; // keep past the block (MRR)
            (void)error;
            dispatch_semaphore_signal(sem);
        }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
                                               (int64_t)(5 * NSEC_PER_SEC)));
    return result; // caller releases
}

class SckScreenBackend : public ScreenBackend
{
public:
    explicit SckScreenBackend(IScreenFrameSink &sink) : m_sink(sink) {}
    ~SckScreenBackend() override { stop(); }

    bool start(const std::string &target, bool captureCursor) override
    {
        if (m_running.exchange(true)) return true;
        if (@available(macOS 12.3, *))
        {
            return startSCK(target, captureCursor);
        }
        LOG_ERROR("VideoCaptureScreen(sck): ScreenCaptureKit needs macOS 12.3+");
        m_running.store(false);
        return false;
    }

    void stop() override
    {
        if (!m_running.exchange(false)) return;
        // #109 — tell the hub the screen audio producer is gone (it will
        // fall back to its own audio-only stream if a consumer remains).
        SckSystemAudioHub::instance().setScreenProducerActive(false);
        if (@available(macOS 12.3, *))
        {
            if (m_stream)
            {
                [m_stream stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; }];
                [m_stream release];
                m_stream = nil;
            }
            if (m_delegate) { [m_delegate release]; m_delegate = nil; }
        }
    }

    std::vector<DeviceInfo> listTargets() override
    {
        std::vector<DeviceInfo> out;
        if (@available(macOS 12.3, *))
        {
            SCShareableContent *content = fetchContentBlocking();
            if (content)
            {
                int i = 0;
                for (SCDisplay *d in content.displays)
                {
                    DeviceInfo di;
                    di.id   = "monitor:" + std::to_string(i++);
                    di.name = "Display " + std::to_string(i) + " (" +
                              std::to_string((int)d.width) + "x" +
                              std::to_string((int)d.height) + ")";
                    di.driver = "screencapturekit";
                    out.push_back(di);
                }
                for (SCWindow *win in content.windows)
                {
                    if (!win.title || win.title.length == 0) continue;
                    DeviceInfo di;
                    di.id   = "window:" + std::to_string((unsigned long)win.windowID);
                    di.name = std::string("Window: ") + win.title.UTF8String;
                    di.driver = "screencapturekit";
                    out.push_back(di);
                }
                [content release];
            }
        }
        if (out.empty())
        {
            DeviceInfo di;
            di.id = "monitor:0"; di.name = "Primary display"; di.driver = "screencapturekit";
            out.push_back(di);
        }
        return out;
    }

private:
    bool startSCK(const std::string &target, bool captureCursor) API_AVAILABLE(macos(12.3))
    {
        SCShareableContent *content = fetchContentBlocking();
        if (!content || content.displays.count == 0)
        {
            LOG_ERROR("VideoCaptureScreen(sck): no shareable content (screen-recording permission?)");
            [content release];
            m_running.store(false);
            return false;
        }

        // Resolve the target. "window:<id>" → a window; "monitor:N" /
        // default → a display. Window capture uses the display the
        // window's filter is built from.
        SCContentFilter *filter = nil;
        uint32_t capW = 0, capH = 0;
        // ScreenCaptureKit only captures system audio with a DISPLAY filter;
        // capturesAudio is silently ignored for desktop-independent window
        // capture (#109). So we only let the screen stream act as the audio
        // producer when grabbing a full display — otherwise the hub runs its
        // own audio-only display stream.
        bool isDisplayCapture = false;

        if (target.rfind("window:", 0) == 0)
        {
            const std::string idStr = target.substr(7);
            const unsigned long wantId = idStr.empty() ? 0 : strtoul(idStr.c_str(), nullptr, 10);
            for (SCWindow *win in content.windows)
            {
                if ((unsigned long)win.windowID == wantId)
                {
                    filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:win];
                    // win.frame is in POINTS. Capture at native pixels so a
                    // Retina (2x) window isn't grabbed at half resolution —
                    // which made shaders (scanlines) space wrong. Scale by
                    // the backing factor of the display the window sits on.
                    double scale = 1.0;
                    for (SCDisplay *d in content.displays)
                    {
                        if (CGRectContainsPoint(d.frame,
                                CGPointMake(CGRectGetMidX(win.frame), CGRectGetMidY(win.frame))))
                        {
                            CGDisplayModeRef m = CGDisplayCopyDisplayMode(d.displayID);
                            if (m)
                            {
                                // True backing scale from the mode itself
                                // (pixels / points), independent of how
                                // SCDisplay reports its size.
                                const double ptW = CGDisplayModeGetWidth(m);
                                if (ptW > 0)
                                    scale = (double)CGDisplayModeGetPixelWidth(m) / ptW;
                                CGDisplayModeRelease(m);
                            }
                            break;
                        }
                    }
                    if (scale < 1.0) scale = 1.0;
                    capW = (uint32_t)(win.frame.size.width  * scale);
                    capH = (uint32_t)(win.frame.size.height * scale);
                    break;
                }
            }
        }

        if (!filter)
        {
            int idx = 0;
            const auto p = target.find(':');
            if (p != std::string::npos)
            {
                const std::string n = target.substr(p + 1);
                if (!n.empty()) idx = atoi(n.c_str());
            }
            if (idx < 0 || idx >= (int)content.displays.count) idx = 0;
            SCDisplay *display = content.displays[idx];
            filter = [[SCContentFilter alloc] initWithDisplay:display
                                            excludingWindows:@[]];
            capW = (uint32_t)display.width;
            capH = (uint32_t)display.height;
            isDisplayCapture = true;
        }

        if (!filter || capW == 0 || capH == 0)
        {
            LOG_ERROR("VideoCaptureScreen(sck): could not build a content filter");
            [filter release];
            [content release];
            m_running.store(false);
            return false;
        }

        SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
        cfg.width  = capW;
        cfg.height = capH;
        cfg.pixelFormat = kCVPixelFormatType_32BGRA;
        cfg.minimumFrameInterval = CMTimeMake(1, 60); // up to 60 fps
        cfg.queueDepth = 5;
        cfg.showsCursor = captureCursor ? YES : NO;

        // #109 — capture system audio on this same stream (macOS 13+) when
        // grabbing a full display, so the system-audio source doesn't need a
        // second SCStream. For window capture SCK won't deliver audio, so we
        // leave it off and the hub falls back to its own display-filter
        // audio stream. 48 kHz stereo to match the audio bus.
        bool wantAudio = false;
        if (@available(macOS 13.0, *))
        {
            if (isDisplayCapture)
            {
                cfg.capturesAudio = YES;
                cfg.sampleRate    = 48000;
                cfg.channelCount  = 2;
                cfg.excludesCurrentProcessAudio = YES; // never capture ourselves
                wantAudio = true;
            }
            LOG_INFO(std::string("VideoCaptureScreen(sck): macOS 13+, ") +
                     (isDisplayCapture ? "display capture — system audio ON this stream"
                                       : "window capture — system audio via separate stream"));
        }
        else
        {
            LOG_INFO("VideoCaptureScreen(sck): macOS < 13 — no SCK audio on this stream");
        }

        m_delegate = [[RCScreenDelegate alloc] init];
        m_delegate->sink = &m_sink;

        m_stream = [[SCStream alloc] initWithFilter:filter
                                      configuration:cfg
                                           delegate:m_delegate];
        [filter release];
        [cfg release];

        NSError *addErr = nil;
        dispatch_queue_t q = dispatch_queue_create("com.retrocapture.sck",
                                                   DISPATCH_QUEUE_SERIAL);
        BOOL added = [m_stream addStreamOutput:m_delegate
                                          type:SCStreamOutputTypeScreen
                            sampleHandlerQueue:q
                                         error:&addErr];
        if (added && wantAudio)
        {
            if (@available(macOS 13.0, *))
            {
                dispatch_queue_t aq = dispatch_queue_create("com.retrocapture.sck.audio",
                                                           DISPATCH_QUEUE_SERIAL);
                NSError *aErr = nil;
                if ([m_stream addStreamOutput:m_delegate
                                         type:SCStreamOutputTypeAudio
                           sampleHandlerQueue:aq
                                        error:&aErr])
                {
                    LOG_INFO("VideoCaptureScreen(sck): audio output added — screen stream is the system-audio producer");
                    SckSystemAudioHub::instance().setScreenProducerActive(true);
                }
                else
                {
                    LOG_WARN(std::string("VideoCaptureScreen(sck): audio output add failed — ") +
                             (aErr ? aErr.localizedDescription.UTF8String : "?"));
                }
                dispatch_release(aq);
            }
        }
        dispatch_release(q);
        if (!added)
        {
            LOG_ERROR(std::string("VideoCaptureScreen(sck): addStreamOutput failed — ") +
                      (addErr ? addErr.localizedDescription.UTF8String : "?"));
            [m_stream release]; m_stream = nil;
            [m_delegate release]; m_delegate = nil;
            [content release];
            m_running.store(false);
            return false;
        }

        [m_stream startCaptureWithCompletionHandler:^(NSError *e) {
            if (e)
                LOG_ERROR(std::string("VideoCaptureScreen(sck): startCapture failed — ") +
                          e.localizedDescription.UTF8String);
            else
                LOG_INFO("VideoCaptureScreen(sck): capture started " +
                         std::to_string(capW) + "x" + std::to_string(capH));
        }];

        [content release];
        return true;
    }

    IScreenFrameSink &m_sink;
    std::atomic<bool>  m_running{false};
    SCStream          *m_stream   = nil;   // owned (retain/release)
    RCScreenDelegate  *m_delegate = nil;   // owned
};
} // namespace

std::unique_ptr<ScreenBackend> createScreenBackend(IScreenFrameSink &sink)
{
    LOG_INFO("ScreenBackend: ScreenCaptureKit (macOS)");
    return std::make_unique<SckScreenBackend>(sink);
}

#endif // RETROCAPTURE_SCREEN_SCK
