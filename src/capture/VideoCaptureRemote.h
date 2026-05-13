#pragma once

#include "IVideoCapture.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
#include <string>

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

    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    uint32_t getPixelFormat() const override { return m_pixelFormat; }

private:
    void decodeLoop();
    bool initDecoder();
    void cleanupDecoder();

    std::string m_url;        // base URL — "/raw" is appended internally
    std::atomic<bool> m_open{false};

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pixelFormat = 0; // 0 means "RGB24" for downstream FrameProcessor

    AVFormatContext *m_formatCtx = nullptr;
    AVCodecContext  *m_codecCtx  = nullptr;
    SwsContext      *m_swsCtx    = nullptr;
    int              m_videoStreamIdx = -1;

    std::thread        m_decodeThread;
    std::atomic<bool>  m_decodeRunning{false};

    // Latest decoded frame buffer. captureFrame / captureLatestFrame copy
    // from here. Single-slot — newer frames overwrite older ones if the
    // consumer hasn't pulled them yet (lossy by design; the encoder thread
    // upstream will keep producing).
    std::mutex            m_frameMutex;
    std::vector<uint8_t>  m_latestRGB;
    uint32_t              m_latestW = 0;
    uint32_t              m_latestH = 0;
    std::atomic<bool>     m_hasFrame{false};
};
