#pragma once

#include <cstdint>
#include <string>

/**
 * Abstract interface for video streamers.
 * 
 * Implementations should handle encoding and serving video frames
 * over various protocols (HTTP MJPEG, WebRTC, etc.)
 */
class IStreamer {
public:
    virtual ~IStreamer() = default;
    
    /**
     * Get the streamer type name (e.g., "HTTP MJPEG")
     */
    virtual std::string getType() const = 0;
    
    /**
     * Initialize the streamer with configuration
     * @param port Port to listen on
     * @param width Stream width
     * @param height Stream height
     * @param fps Target framerate
     * @return true if initialization successful
     */
    virtual bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) = 0;
    
    /**
     * Start streaming
     * @return true if started successfully
     */
    virtual bool start() = 0;
    
    /**
     * Stop streaming
     */
    virtual void stop() = 0;
    
    /**
     * Check if streamer is active
     */
    virtual bool isActive() const = 0;
    
    /**
     * Push a frame to be streamed
     * @param data RGB frame data (width * height * 3 bytes)
     * @param width Frame width
     * @param height Frame height
     * @return true if frame was queued successfully
     */
    virtual bool pushFrame(const uint8_t* data, uint32_t width, uint32_t height) = 0;
    
    /**
     * Get the stream URL (for display in UI)
     */
    virtual std::string getStreamUrl() const = 0;
    
    /**
     * Get number of active clients
     */
    virtual uint32_t getClientCount() const = 0;
    
    /**
     * Cleanup resources
     */
    virtual void cleanup() = 0;
};

