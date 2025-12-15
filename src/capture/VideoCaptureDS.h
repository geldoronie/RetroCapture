#pragma once

#include "IVideoCapture.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <comdef.h>
#include <dshow.h>

// Forward declarations
struct IGraphBuilder;
struct ICaptureGraphBuilder2;
struct IBaseFilter;
struct ISampleGrabber;
struct IMediaControl;
struct IMediaEventEx;
struct IAMStreamConfig;
struct IAMVideoProcAmp;
struct IAMCameraControl;
struct IPin;
struct IEnumPins;

// Forward declaration for custom filter
class DSFrameGrabber;

/**
 * @brief DirectShow implementation of IVideoCapture for Windows
 * Uses DirectShow instead of Media Foundation for better MinGW/MXE compatibility
 */
class VideoCaptureDS : public IVideoCapture
{
public:
    VideoCaptureDS();
    ~VideoCaptureDS() override;

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
    uint32_t getPixelFormat() const override;
    
    // Obter resoluções disponíveis do dispositivo (sem precisar abrir completamente)
    std::vector<std::pair<uint32_t, uint32_t>> getSupportedResolutions(const std::string &deviceId);

private:
    // DirectShow objects
    IGraphBuilder *m_graphBuilder;
    ICaptureGraphBuilder2 *m_captureGraphBuilder;
    IBaseFilter *m_captureFilter;
    ISampleGrabber *m_sampleGrabber;
    IMediaControl *m_mediaControl;
    IMediaEventEx *m_mediaEvent;
    IAMStreamConfig *m_streamConfig;
    IAMVideoProcAmp *m_videoProcAmp;
    IAMCameraControl *m_cameraControl;

    // Frame buffer
    std::vector<uint8_t> m_frameBuffer;
    std::mutex m_bufferMutex;
    Frame m_latestFrame;
    bool m_hasFrame;
    
    // Alternative capture without Sample Grabber
    IPin *m_capturePin; // Pin de captura para obter samples diretamente
    bool m_useAlternativeCapture; // Flag para usar captura alternativa
    IBaseFilter *m_customGrabberFilter; // Filtro customizado para capturar frames

    // Format information
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_fps;
    uint32_t m_pixelFormat; // Pixel format code (0 = RGB24)

    // State
    bool m_isOpen;
    bool m_streaming;
    bool m_dummyMode;
    std::string m_deviceId;

    // Dummy mode buffer
    std::vector<uint8_t> m_dummyFrameBuffer;

    // Helper methods
    bool initializeCOM();
    void shutdownCOM();
    bool createCaptureGraph(const std::string &deviceId);
    bool configureCaptureFormat();
    bool readSample(Frame &frame);
    void generateDummyFrame(Frame &frame);
    std::string getControlNameFromDS(const std::string &controlName);
    bool setControlDS(const std::string &controlName, int32_t value);
    bool getControlDS(const std::string &controlName, int32_t &value);
    
    // COM initialization tracking
    bool m_comInitialized;
};

#endif // _WIN32

