#pragma once

#include "IVideoCapture.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#endif

/**
 * @brief AVFoundation implementation of IVideoCapture for macOS
 */
class VideoCaptureAVFoundation : public IVideoCapture
{
public:
    VideoCaptureAVFoundation();
    ~VideoCaptureAVFoundation() override;

    // IVideoCapture interface
    bool open(const std::string &device) override;
    void close() override;
    bool isOpen() const override;
    bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0) override;
    bool setFramerate(uint32_t fps) override;
    bool captureFrame(Frame &frame) override;
    bool setControl(const std::string &controlName, int32_t value) override;
    bool getControl(const std::string &controlName, int32_t &value) override;
    bool getControlMin(const std::string &controlName, int32_t &minValue) override;
    bool getControlMax(const std::string &controlName, int32_t &maxValue) override;
    bool getControlDefault(const std::string &controlName, int32_t &defaultValue) override;
    std::vector<DeviceInfo> listDevices() override;
    void setDummyMode(bool enabled) override;
    bool isDummyMode() const override;
    bool startCapture() override;
    void stopCapture() override;
    bool captureLatestFrame(Frame &frame) override;
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    uint32_t getPixelFormat() const override { return m_pixelFormat; }

    // Método público para delegate Objective-C acessar
    void onFrameCaptured(CVPixelBufferRef pixelBuffer);

private:
#ifdef __APPLE__
    AVCaptureSession *m_captureSession;
    AVCaptureDevice *m_captureDevice;
    AVCaptureVideoDataOutput *m_videoOutput;
    dispatch_queue_t m_captureQueue;
    CVPixelBufferRef m_latestPixelBuffer;
    id m_delegate; // VideoCaptureDelegate (Objective-C object)
    uint8_t *m_frameBuffer;
    size_t m_frameBufferSize;
    std::mutex m_bufferMutex;
#endif
    
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pixelFormat;
    bool m_isOpen;
    bool m_isCapturing;
    bool m_dummyMode;
    std::vector<uint8_t> m_dummyFrameBuffer;
    
    void generateDummyFrame(Frame &frame);
    bool convertPixelBufferToFrame(CVPixelBufferRef pixelBuffer, Frame &frame);
};
