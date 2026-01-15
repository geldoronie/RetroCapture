#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

/**
 * RecordingMetadata - Metadata structure for video recordings
 *
 * Stores information about recorded videos including codec info,
 * file properties, and timestamps.
 */
struct RecordingMetadata
{
    std::string id;              // UUID or hash
    std::string filename;        // File name
    std::string filepath;        // Full path
    std::string container;       // mp4, mkv, etc.

    // Codec info
    std::string videoCodec;
    std::string audioCodec;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;

    // File info
    uint64_t fileSize = 0;        // Bytes
    uint64_t durationUs = 0;     // Microseconds
    std::string createdAt;       // ISO 8601 timestamp

    // Thumbnail (optional)
    std::string thumbnailPath;   // Thumbnail path

    // JSON serialization
    nlohmann::json toJSON() const;
    static RecordingMetadata fromJSON(const nlohmann::json& json);
};
