#pragma once

#include "IStreamer.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

/**
 * Manages video streaming to remote clients.
 *
 * Supports multiple streaming protocols (HTTP MJPEG, etc.)
 * and handles frame distribution to active streamers.
 */
class StreamManager
{
public:
    StreamManager();
    ~StreamManager();

    /**
     * Add a streamer instance
     */
    void addStreamer(std::unique_ptr<IStreamer> streamer);

    /**
     * Initialize all streamers
     */
    bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps);

    /**
     * Start all streamers
     */
    bool start();

    /**
     * Stop all streamers
     */
    void stop();

    /**
     * Check if any streamer is active
     */
    bool isActive() const;

    /**
     * Push a frame to all active streamers
     * @param data RGB frame data (width * height * 3 bytes)
     * @param width Frame width
     * @param height Frame height
     */
    void pushFrame(const uint8_t *data, uint32_t width, uint32_t height);
    void pushAudio(const int16_t *samples, size_t sampleCount);

    /**
     * Get all stream URLs
     */
    std::vector<std::string> getStreamUrls() const;

    /**
     * Get total client count across all streamers
     */
    uint32_t getTotalClientCount() const;

    /**
     * Get found SSL certificate path from HTTPTSStreamer (if available)
     * Returns empty string if not found or not using HTTPS
     */
    std::string getFoundSSLCertificatePath() const;

    /**
     * Get found SSL key path from HTTPTSStreamer (if available)
     * Returns empty string if not found or not using HTTPS
     */
    std::string getFoundSSLKeyPath() const;

    /**
     * Enable/disable Web Portal in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalEnabled(bool enabled);

    /**
     * Enable/disable HTTPS in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setHTTPSEnabled(bool enabled);

    /**
     * Cleanup all resources
     */
    void cleanup();

private:
    std::vector<std::unique_ptr<IStreamer>> m_streamers;
    bool m_initialized = false;
    bool m_active = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
