#pragma once

#include "IVideoCapture.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Screen-capture video source (#107).
 *
 * Feeds the desktop — a whole monitor, a specific window, or an
 * arbitrary cropped region — into the same IVideoCapture contract used
 * by V4L2 / DirectShow / AVFoundation / Remote, so shaders, streaming,
 * recording and the virtual camera all compose with it for free.
 *
 * Target model
 * ------------
 * open(device) takes an identifier string describing what to capture:
 *   - "monitor:N"      capture display index N (N from listDevices())
 *   - "window:<id>"    capture a single window (platform-native handle)
 *   - ""               platform default (primary monitor, or — on
 *                      Wayland — let the portal's own picker decide)
 * A region crop (x, y, w, h in target pixels) layered on top of any
 * target is set via setRegion(); (0,0,0,0) means "no crop / full target".
 *
 * Platform backends (selected at compile time, hidden behind a pimpl):
 *   - Linux:   PipeWire + xdg-desktop-portal ScreenCast (Wayland + X11)
 *   - Windows: DXGI Desktop Duplication (monitor) / WGC|GDI (window)
 *   - macOS:   ScreenCaptureKit (display / window / region)
 *
 * Frames are delivered as packed 32-bit BGRA/RGBA (see getPixelFormat);
 * the FrameProcessor converts to RGB24 like every other source. Sizes
 * are dynamic — getWidth()/getHeight() report the live captured size
 * (after crop), which can change if the user switches target.
 */
class VideoCaptureScreen : public IVideoCapture
{
public:
    VideoCaptureScreen();
    ~VideoCaptureScreen() override;

    // IVideoCapture
    bool open(const std::string &device) override;
    void close() override;
    bool isOpen() const override { return m_open.load(); }

    bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0) override;
    bool setFramerate(uint32_t fps) override;

    bool captureFrame(Frame &frame) override;
    bool captureLatestFrame(Frame &frame) override;

    // No hardware knobs on a screen grab.
    bool setControl(const std::string &, int32_t) override { return false; }
    bool getControl(const std::string &, int32_t &) override { return false; }
    bool getControlMin(const std::string &, int32_t &) override { return false; }
    bool getControlMax(const std::string &, int32_t &) override { return false; }
    bool getControlDefault(const std::string &, int32_t &) override { return false; }

    std::vector<DeviceInfo> listDevices() override;

    void setDummyMode(bool enabled) override { m_dummyMode = enabled; }
    bool isDummyMode() const override { return m_dummyMode; }

    bool startCapture() override;
    void stopCapture() override;

    uint32_t getWidth() const override { return m_width.load(); }
    uint32_t getHeight() const override { return m_height.load(); }
    uint32_t getPixelFormat() const override { return m_pixelFormat; }

    bool isReceivingFrames() const override;

    /**
     * Region crop, in target pixels, applied on top of whatever target
     * open() selected. (0,0,0,0) disables cropping (capture full target).
     * Safe to call before or after open(); takes effect on the next
     * delivered frame.
     */
    void setRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    /** Whether the captured cursor should be composited into the frame. */
    void setCaptureCursor(bool on) { m_captureCursor.store(on); }

private:
    // Platform backend, pimpl so the PipeWire / DXGI / ScreenCaptureKit
    // headers stay out of this .h. Defined per-platform in the matching
    // VideoCaptureScreen_*.cpp.
    struct Backend;
    std::unique_ptr<Backend> m_backend;

    std::string        m_target;          // last opened identifier
    std::atomic<bool>  m_open{false};
    bool               m_dummyMode = false;

    // Live captured dimensions (after crop), updated by the backend.
    std::atomic<uint32_t> m_width{0};
    std::atomic<uint32_t> m_height{0};
    // Packed 32-bit; downstream treats this as BGRA→RGB in FrameProcessor.
    uint32_t           m_pixelFormat = 0;

    // Region crop (target pixels). Guarded because the UI thread sets it
    // while the capture thread reads it.
    std::mutex            m_regionMutex;
    uint32_t              m_regionX = 0, m_regionY = 0, m_regionW = 0, m_regionH = 0;

    std::atomic<bool>  m_captureCursor{true};
    std::atomic<bool>  m_receiving{false};

    // Latest decoded frame, RGB24. The backend writes under the lock;
    // captureLatestFrame() hands a pointer to it back to the pipeline.
    std::mutex            m_frameMutex;
    std::vector<uint8_t>  m_frameBuf;
    bool                  m_haveFrame = false;
};
