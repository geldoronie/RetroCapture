#pragma once

#include "IVideoCapture.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief V4L2 implementation of IVideoCapture for Linux
 */
class VideoCaptureV4L2 : public IVideoCapture
{
public:
    VideoCaptureV4L2();
    ~VideoCaptureV4L2() override;

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

    // Additional V4L2-specific methods (for backward compatibility)
    bool setControl(uint32_t controlId, int32_t value);
    bool getControl(uint32_t controlId, int32_t &value);
    bool getControl(uint32_t controlId, int32_t &value, int32_t &min, int32_t &max, int32_t &step);
    bool setBrightness(int32_t value);
    bool setContrast(int32_t value);
    bool setSaturation(int32_t value);
    bool setHue(int32_t value);
    bool setGain(int32_t value);
    bool setExposure(int32_t value);
    bool setSharpness(int32_t value);
    bool setGamma(int32_t value);
    bool setWhiteBalanceTemperature(int32_t value);
    std::vector<uint32_t> getSupportedFormats();

private:
    int m_fd = -1;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pixelFormat = 0;

    struct Buffer
    {
        void *start = nullptr;
        size_t length = 0;
    };

    std::vector<Buffer> m_buffers;
    bool m_streaming = false;
    bool m_dummyMode = false;
    std::vector<uint8_t> m_dummyFrameBuffer;

    bool initMemoryMapping();
    void cleanupBuffers();
    void generateDummyFrame(Frame &frame);
    uint32_t getControlIdFromName(const std::string &controlName);
};

