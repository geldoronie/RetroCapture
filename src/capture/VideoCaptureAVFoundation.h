#pragma once

#include "IVideoCapture.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

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
    
    // Métodos para delegate acessar informações de framerate
    AVCaptureDevice* getCaptureDevice() const;
    AVCaptureVideoDataOutput* getVideoOutput() const;
    
    // Format enumeration and selection (for UI)
    std::vector<AVFoundationFormatInfo> listFormats(const std::string &deviceId = "") override;
    bool setFormatByIndex(int formatIndex, const std::string &deviceId = "");
    bool setFormatById(const std::string &formatId, const std::string &deviceId = "") override;
    
    // Audio capture methods (for capturing audio from video device)
    bool hasAudio() const override;
    size_t getAudioSamples(int16_t* buffer, size_t maxSamples) override;
    uint32_t getAudioSampleRate() const override;
    uint32_t getAudioChannels() const override;
    void onAudioSampleBuffer(CMSampleBufferRef sampleBuffer);
    
    // Audio device enumeration and selection
    std::vector<DeviceInfo> listAudioDevices();
    bool setAudioDevice(const std::string &audioDeviceId);
    std::string getCurrentAudioDevice() const;

private:
#ifdef __APPLE__
    AVCaptureSession *m_captureSession;
    AVCaptureDevice *m_captureDevice;
    AVCaptureVideoDataOutput *m_videoOutput;
    AVCaptureAudioDataOutput *m_audioOutput;
    dispatch_queue_t m_captureQueue;
    dispatch_queue_t m_audioQueue;
    CVPixelBufferRef m_latestPixelBuffer;
    id m_delegate; // VideoCaptureDelegate (Objective-C object)
    id m_audioDelegate; // AudioCaptureDelegate (Objective-C object)
    uint8_t *m_frameBuffer;
    size_t m_frameBufferSize;
    std::mutex m_bufferMutex;
    
    // Audio capture state
    std::vector<int16_t> m_audioBuffer;
    std::mutex m_audioBufferMutex;
    uint32_t m_audioSampleRate;
    uint32_t m_audioChannels;
    bool m_hasAudio;
    std::string m_selectedAudioDeviceId; // User-selected audio device ID
#endif
    
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pixelFormat;
    uint32_t m_fps; // Store requested framerate
    bool m_isOpen;
    bool m_isCapturing;
    std::atomic<bool> m_isClosing{false}; // Flag to indicate device is being closed (prevents callbacks from accessing freed resources) - atomic for thread safety
    bool m_dummyMode;
    std::vector<uint8_t> m_dummyFrameBuffer;
    
    // Track if a specific format was selected via UI (to avoid being overwritten)
    bool m_formatSelectedViaUI;
    std::string m_selectedFormatId;
    
    void generateDummyFrame(Frame &frame);
    bool convertPixelBufferToFrame(CVPixelBufferRef pixelBuffer, Frame &frame);
    
    // Centralized method to apply format and framerate atomically
    bool applyFormatAndFramerate(AVCaptureDeviceFormat* format, uint32_t fps, bool stopSessionIfRunning = false);
};
