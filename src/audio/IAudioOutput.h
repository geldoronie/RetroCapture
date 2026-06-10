#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * @brief Abstract interface for audio output/monitoring across different platforms
 */
class IAudioOutput
{
public:
    virtual ~IAudioOutput() = default;

    // Open audio output device
    // sampleRate and channels are optional - if 0, will use defaults or device capabilities
    virtual bool open(const std::string& deviceName = "", uint32_t sampleRate = 0, uint32_t channels = 0) = 0;
    
    // Close audio output
    virtual void close() = 0;
    
    // Check if audio output is open
    virtual bool isOpen() const = 0;
    
    // Start audio output
    virtual bool start() = 0;
    
    // Stop audio output
    virtual void stop() = 0;
    
    // Write audio samples (non-blocking, returns number of samples written)
    virtual size_t write(const int16_t* samples, size_t numSamples) = 0;
    
    // Get audio format information
    virtual uint32_t getSampleRate() const = 0;
    virtual uint32_t getChannels() const = 0;
    
    // Set volume (0.0 to 1.0)
    virtual void setVolume(float volume) = 0;
    virtual float getVolume() const = 0;
    
    // Check if monitoring is enabled
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;
};
