#pragma once

#include "IVideoCapture.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <comdef.h>

// Forward declarations
struct IMFMediaSource;
struct IMFSourceReader;
struct IMFMediaType;
struct IMFSample;
struct IMFMediaBuffer;

/**
 * @brief Media Foundation implementation of IVideoCapture for Windows
 */
class VideoCaptureMF : public IVideoCapture
{
public:
    VideoCaptureMF();
    ~VideoCaptureMF() override;

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

private:
    // Media Foundation objects
    IMFMediaSource *m_mediaSource;
    IMFSourceReader *m_sourceReader;
    IMFMediaType *m_mediaType;

    // Frame buffer
    std::vector<uint8_t> m_frameBuffer;
    std::mutex m_bufferMutex;
    Frame m_latestFrame;
    bool m_hasFrame;

    // Format information
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_fps;
    GUID m_pixelFormat; // MF video format GUID

    // State
    bool m_isOpen;
    bool m_streaming;
    bool m_dummyMode;
    std::string m_deviceId;

    // Dummy mode buffer
    std::vector<uint8_t> m_dummyFrameBuffer;

    // Helper methods
    bool initializeMediaFoundation();
    void shutdownMediaFoundation();
    bool createMediaSource(const std::string &deviceId);
    bool configureSourceReader();
    bool readSample(Frame &frame);
    void generateDummyFrame(Frame &frame);
    GUID getPixelFormatGUID(uint32_t pixelFormat);
    uint32_t getPixelFormatFromGUID(const GUID &guid);
    std::string getControlNameFromMF(const std::string &controlName);
    bool setControlMF(const std::string &controlName, int32_t value);
    bool getControlMF(const std::string &controlName, int32_t &value);
};

#endif // _WIN32

