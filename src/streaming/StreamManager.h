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
     * Set Web Portal title in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalTitle(const std::string &title);

    /**
     * Set Web Portal subtitle in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalSubtitle(const std::string &subtitle);

    /**
     * Set Web Portal image path in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalImagePath(const std::string &path);

    /**
     * Set Web Portal background image path in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalBackgroundImagePath(const std::string &path);

    /**
     * Set Web Portal colors in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalColors(
        const float bg[4], const float text[4], const float primary[4],
        const float primaryLight[4], const float primaryDark[4],
        const float secondary[4], const float secondaryHighlight[4],
        const float cardHeader[4], const float border[4],
        const float success[4], const float warning[4], const float danger[4], const float info[4]);

    /**
     * Set Web Portal texts in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setWebPortalTexts(
        const std::string &streamInfo, const std::string &quickActions, const std::string &compatibility,
        const std::string &status, const std::string &codec, const std::string &resolution,
        const std::string &streamUrl, const std::string &copyUrl, const std::string &openNewTab,
        const std::string &supported, const std::string &format, const std::string &codecInfo,
        const std::string &supportedBrowsers, const std::string &formatInfo, const std::string &codecInfoValue,
        const std::string &connecting);

    /**
     * Set HLS performance parameters in HTTPTSStreamer (if available)
     * This can be called while streaming is active
     */
    void setHLSParameters(
        bool lowLatencyMode,
        float backBufferLength,
        float maxBufferLength,
        float maxMaxBufferLength,
        bool enableWorker);

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
