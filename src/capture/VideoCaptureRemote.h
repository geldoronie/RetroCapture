#pragma once

#include "IVideoCapture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare FFmpeg types so the FFmpeg headers stay out of this .h
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;
struct SwrContext;
class  IAudioPlayback;

/**
 * @brief Remote MPEG-TS source — consumes /raw from another RetroCapture
 *        instance.
 *
 * Phase 3 of issue #47. open(url) takes the BASE URL of a remote server
 * (e.g. "http://host:8080"); the implementation appends "/raw" by
 * convention. Frames decoded from the stream are converted to RGB24 and
 * delivered through the same IVideoCapture contract used by V4L2 / DS.
 *
 * Hardware-control surface (brightness/contrast/etc.) is a no-op: a
 * remote stream isn't a piece of capture hardware and there's nothing to
 * tweak from this side. The viewer-side shader pipeline lives in the
 * usual ShaderEngine and is wired in Phase 4.
 */
class VideoCaptureRemote : public IVideoCapture
{
public:
    VideoCaptureRemote();
    ~VideoCaptureRemote() override;

    // IVideoCapture
    bool open(const std::string &url) override;
    void close() override;
    bool isOpen() const override { return m_open.load(); }

    bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0) override;
    bool setFramerate(uint32_t fps) override;

    bool captureFrame(Frame &frame) override;
    bool captureLatestFrame(Frame &frame) override;

    // Hardware controls — not applicable to a remote stream.
    bool setControl(const std::string &, int32_t) override { return false; }
    bool getControl(const std::string &, int32_t &) override { return false; }
    bool getControlMin(const std::string &, int32_t &) override { return false; }
    bool getControlMax(const std::string &, int32_t &) override { return false; }
    bool getControlDefault(const std::string &, int32_t &) override { return false; }

    std::vector<DeviceInfo> listDevices() override { return {}; }

    void setDummyMode(bool) override {}
    bool isDummyMode() const override { return false; }

    bool startCapture() override;
    void stopCapture() override;

    uint32_t getWidth() const override
    {
        const uint32_t tw = m_targetWidth.load();
        return tw ? tw : m_width;
    }
    uint32_t getHeight() const override
    {
        const uint32_t th = m_targetHeight.load();
        return th ? th : m_height;
    }
    uint32_t getPixelFormat() const override { return m_pixelFormat; }

    /**
     * Resize the decoded frame to (width, height) before delivering it to
     * the rest of the capture pipeline. Used by the remote-source path
     * when the host's /meta reports a different source resolution than
     * what the /raw stream is encoded at — the client wants the shader
     * to run on the host's logical source dims, not the smaller
     * transmission dims. Pass (0, 0) to disable rescaling and pass the
     * decoded frames through at their native size.
     */
    void setTargetResolution(uint32_t width, uint32_t height);

    // Per-refresh interpolation strategy when filling display refreshes
    // between two consecutive stream frames. See captureLatestFrame for
    // the per-mode semantics.
    enum class InterpolationMode
    {
        Linear,  // LERP prev/next — smooth motion, slight ghosting
        Nearest, // pick the temporally closer frame — clean image, 3:2 pulldown
        Off      // strict PTS gate, hold prev until next is due
    };
    void setInterpolationMode(InterpolationMode mode);

    /**
     * #77 — client-side playback gain for the incoming audio, linear
     * in [0.0, 1.0] (1.0 = unity). Stored in an atomic and pushed to
     * the audio sink by the decode thread, so it can be called live
     * from the UI thread without touching the sink pointer directly.
     * Applied lazily too: a value set before the sink exists (e.g. from
     * config at connect time) is carried into ensureAudioOutput().
     */
    void setAudioVolume(float linear);

    /**
     * #49 Phase 3 — bearer token sent to the host's /raw endpoint
     * for password-protected streams. Set before open(); empty
     * string disables the header (the host's open-by-default path
     * doesn't require it).
     */
    void setAuthToken(const std::string &tokenHex) { m_authToken = tokenHex; }

private:
    void decodeLoop();
    bool initDecoder();
    void cleanupDecoder();
    // Bring up the audio sink + resampler from the audio decoder's
    // current params (m_audioCodecCtx). Returns false (and leaves
    // m_audioPlayback/m_swrCtx null) when the params aren't resolved
    // yet — e.g. a mid-join AAC probe that reports 0 channels. Called
    // eagerly in initDecoder and lazily from the decode loop once the
    // first decoded frame populates the codec context. #98.
    bool ensureAudioOutput();

    std::string m_url;        // base URL — "/raw" is appended internally
    std::string m_authToken;  // sha256 hex of password, empty == no auth
    std::atomic<bool> m_open{false};

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pixelFormat = 0; // 0 means "RGB24" for downstream FrameProcessor

    // Optional rescale target. 0/0 means "pass through at stream resolution".
    // Mutated from the application's main thread; read on the decode thread.
    std::atomic<uint32_t> m_targetWidth{0};
    std::atomic<uint32_t> m_targetHeight{0};

    AVFormatContext *m_formatCtx = nullptr;
    AVCodecContext  *m_codecCtx  = nullptr;
    SwsContext      *m_swsCtx    = nullptr;
    int              m_videoStreamIdx = -1;

    // Remote-stream audio path. Activated when the /raw demuxer
    // exposes an audio stream — we decode it alongside video in the
    // same decodeLoop and submit the float-PCM samples to the
    // platform playback sink. The sink's getClockUs() becomes the
    // A/V master clock for the captureLatestFrame() blend gate.
    AVCodecContext  *m_audioCodecCtx = nullptr;
    SwrContext      *m_swrCtx        = nullptr;
    int              m_audioStreamIdx = -1;
    std::unique_ptr<IAudioPlayback> m_audioPlayback;
    // Reusable scratch so we don't reallocate per audio frame. Sized
    // by the resampler's max-output estimate.
    std::vector<float> m_audioScratch;

    std::thread        m_decodeThread;
    std::atomic<bool>  m_decodeRunning{false};
    // Set during stopCapture()/close() so the FFmpeg interrupt_callback
    // can abort whichever blocking I/O the decode thread is parked in.
    // Distinct from m_decodeRunning because m_decodeRunning is only
    // true while the worker thread is alive; the callback fires from
    // inside open()/avformat_open_input as well, and we don't want it
    // tripping during the initial open just because the worker hasn't
    // started yet.
    std::atomic<bool>  m_decodeAborted{false};

    // Small bounded queue of decoded RGB frames. Decoder pushes to back;
    // consumer pops front. When the queue would exceed kMaxQueued the
    // oldest frame is dropped — bounds the latency at a few frames while
    // absorbing TCP / decoder bursts that would otherwise overwrite mid-
    // burst frames if we kept a single-slot buffer. Replaces the "single
    // slot, newer overwrites older" design from the first cut of this
    // class.
    struct QueuedFrame
    {
        std::vector<uint8_t> rgb;
        uint32_t             width  = 0;
        uint32_t             height = 0;
        // Wall-clock target time (steady_clock microseconds) at which this
        // frame should become the on-screen frame. Computed in the decode
        // loop from frame.pts and the stream anchor — see decodeLoop().
        int64_t              targetWallUs = 0;
    };
    // Bigger buffer absorbs the bursty arrival pattern produced by the
    // server's variable-rate encoder + TCP buffering. PTS-anchored
    // release means frames sit in the queue waiting for their target
    // display moment, so bursts that arrive faster than the stream's
    // average rate stack up briefly before draining. 5 was tight enough
    // that bursts overflowed regularly (user observed drops=1-7/s in
    // the decode loop telemetry); 20 gives ~500 ms of headroom at 40
    // fps which is plenty for ordinary network jitter and still under
    // the latency budget for distributed shader preview.
    static constexpr size_t kMaxQueued = 20;

    std::mutex             m_frameMutex;
    std::deque<QueuedFrame> m_frameQueue;
    // Last frame handed to the consumer — used to keep something on screen
    // (rather than dummy black) when the queue is momentarily empty, and
    // as one of the two endpoints for the per-refresh interpolation
    // (see m_blendBuffer below).
    QueuedFrame             m_lastConsumed;
    std::atomic<bool>       m_hasFrame{false};

    std::atomic<InterpolationMode> m_interpolationMode{InterpolationMode::Linear};

    // #77 client-side audio gain, written by the UI thread, read by the
    // decode thread which forwards it to m_audioPlayback each frame.
    std::atomic<float> m_audioVolume{1.0f};

    // Per-refresh interpolation output buffer. The classic 3:2 pulldown
    // problem (60 fps stream into a 144 Hz panel = 2.4 refreshes per
    // frame → alternating 2/3-refresh hold times → visible judder) is
    // a non-integer ratio issue that no choice of stream rate can fully
    // solve at the client side. Instead, on every captureLatestFrame
    // call we LERP between m_lastConsumed and the next queued frame
    // using t = (now - prevTarget) / (nextTarget - prevTarget), so each
    // refresh shows a unique intermediate image rather than a held
    // duplicate. Indistinguishable from real motion at typical
    // stream:refresh ratios; the dependency on the server transmitting
    // at a "magic" matched rate goes away.
    std::vector<uint8_t> m_blendBuffer;

    // PTS-anchored playback. On the first decoded frame we record the
    // current wall clock as the "stream zero" reference and the frame's
    // PTS as the stream PTS origin. Every subsequent frame's target wall
    // time = m_streamStartWallUs + (pts - m_firstPtsTicks) * timebase.
    // captureLatestFrame() then only releases a frame once its target
    // time has been reached, holding the previously released frame on
    // screen until then — eliminates the irregular 1/2-refresh-cycle
    // judder we get when network/decoder bursts let us poll multiple
    // new frames in quick succession.
    std::atomic<bool> m_streamAnchored{false};
    // Steady-clock microseconds of the most recently decoded frame.
    // Read by isReceivingFrames() to detect a stalled stream that
    // m_streamAnchored hasn't reset yet (TCP read still blocking
    // post host-disappear). Updated on every decoded video frame.
    std::atomic<int64_t> m_lastFrameAtSteadyUs{0};
    int64_t m_streamStartWallUs  = 0;
    int64_t m_firstPtsTicks      = 0;
    // PTS of the first video frame in microseconds. Lets the audio
    // path (which has its own stream timebase) translate its absolute
    // PTS into the same stream-origin-relative coordinate the video
    // queue uses. Both audio and video share the same MPEG-TS clock,
    // so subtracting this gives 'microseconds since the first frame
    // of the stream'.
    std::atomic<int64_t> m_firstPtsUs{0};
    double  m_streamTimebaseSecs = 0.0;

    // Rolling 1-second decode/consume counters — produces a periodic
    // LOG_INFO so rate mismatches and dropped-burst frames are visible
    // in the log. Reset every second.
    uint32_t m_statProduced = 0;
    uint32_t m_statConsumed = 0;
    uint32_t m_statDropped  = 0;
    std::chrono::steady_clock::time_point m_statStart;

public:
    /**
     * Reconnect-backoff signal for the UI (#58).
     *
     * True once `decodeLoop` has retried the connection enough times
     * to suspect the host is gone for good rather than briefly
     * hiccuping (~15 min at the 60s ceiling = 30 consecutive
     * failures). The remote-connection window reads this and shows
     * "Host appears offline" so the user can stop expecting a quick
     * recovery and disconnect/reconnect manually if they want.
     *
     * Cleared as soon as any reconnect succeeds.
     */
    bool isHostLikelyOffline() const override { return m_hostLikelyOffline.load(); }
    // 'Are we actively decoding right now?' — combines the
    // m_streamAnchored handshake bit (true once the first frame
    // decoded, reset in cleanupDecoder() / av_read_frame failure)
    // with a 'last frame seen recently' staleness check. The
    // staleness check matters because when the host disappears the
    // TCP/TLS read can block for up to ~10 s before av_read_frame
    // surfaces an error — without the staleness fallback the
    // overlay would think we're still connected the whole time.
    bool isReceivingFrames() const override
    {
        if (!m_streamAnchored.load()) return false;
        const int64_t lastUs = m_lastFrameAtSteadyUs.load();
        if (lastUs == 0) return false;
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        // 2 s without a frame on a 60 fps stream = ~120 frames gap.
        // Plenty of headroom for a hiccup but tight enough that a
        // killed host is reflected in the overlay quickly.
        return (nowUs - lastUs) < 2'000'000;
    }

private:
    // Capped exponential backoff state. The table is consulted with
    // `m_consecutiveReconnectFailures` clamped to the table size; on
    // any successful reconnect the counter resets to 0 (and the
    // offline hint clears with it).
    std::atomic<uint32_t> m_consecutiveReconnectFailures{0};
    std::atomic<bool>     m_hostLikelyOffline{false};
};
