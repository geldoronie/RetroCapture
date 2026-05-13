#pragma once

#include "IVideoCapture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare FFmpeg types so the FFmpeg headers stay out of this .h
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

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

private:
    void decodeLoop();
    bool initDecoder();
    void cleanupDecoder();

    std::string m_url;        // base URL — "/raw" is appended internally
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

    std::thread        m_decodeThread;
    std::atomic<bool>  m_decodeRunning{false};

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
    // (rather than dummy black) when the queue is momentarily empty.
    QueuedFrame             m_lastConsumed;
    std::atomic<bool>       m_hasFrame{false};

    // PTS-anchored playback. On the first decoded frame we record the
    // current wall clock as the "stream zero" reference and the frame's
    // PTS as the stream PTS origin. Every subsequent frame's target wall
    // time = m_streamStartWallUs + (pts - m_firstPtsTicks) * timebase.
    // captureLatestFrame() then only releases a frame once its target
    // time has been reached, holding the previously released frame on
    // screen until then — eliminates the irregular 1/2-refresh-cycle
    // judder we get when network/decoder bursts let us poll multiple
    // new frames in quick succession.
    bool    m_streamAnchored     = false;
    int64_t m_streamStartWallUs  = 0;
    int64_t m_firstPtsTicks      = 0;
    double  m_streamTimebaseSecs = 0.0;

    // Rolling 1-second decode/consume counters — produces a periodic
    // LOG_INFO so rate mismatches and dropped-burst frames are visible
    // in the log. Reset every second.
    uint32_t m_statProduced = 0;
    uint32_t m_statConsumed = 0;
    uint32_t m_statDropped  = 0;
    std::chrono::steady_clock::time_point m_statStart;
};
