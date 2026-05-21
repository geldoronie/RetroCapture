#pragma once

#include <string>
#include <cstdint>

/**
 * RecordingSettings - Configuration for video recording
 */
struct RecordingSettings
{
    // Video
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate = 8000000; // 8 Mbps
    std::string codec = "h264"; // h264, h265, vp8, vp9
    std::string preset = "veryfast";
    std::string h265Profile = "main";
    std::string h265Level = "auto";
    int vp8Speed = 12;
    int vp9Speed = 6;

    // Audio
    uint32_t audioBitrate = 256000; // 256 kbps
    std::string audioCodec = "aac";

    // Container
    std::string container = "mp4"; // mp4, mkv, avi
    std::string outputPath = "recordings/"; // Base directory
    std::string filenameTemplate = "recording_%Y%m%d_%H%M%S"; // strftime format

    // Options
    bool includeAudio = true;
    bool autoStart = false;
    uint64_t maxDurationUs = 0; // 0 = no limit
    uint64_t maxFileSize = 0;   // 0 = no limit

    // Hardware encoder selection (#59) — mirrors the streaming side.
    // Same int encoding as MediaEncoder::HardwareEncoder
    // (0=Auto, 1=Software, 2=NVENC, 3=VAAPI, 4=QSV, 5=AMF). Auto is
    // the safe default: MediaEncoder probes the build for hardware
    // support and falls back to libx264 if none is wired up. hwPreset
    // carries the backend-specific tuning string (NVENC p1..p7,
    // VAAPI CBR/VBR/CQP, QSV preset names, AMF quality levels).
    int         hardwareEncoder = 0;
    std::string hwPreset = "";
};
