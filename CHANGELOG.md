# Changelog

All notable changes to RetroCapture will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned

- WebRTC streaming support
- RTSP streaming support
- Additional codec options and configurations
- Stream authentication and access control

---

## [0.2.0-alpha] - 2025-11-22

### Added

- **HTTP MPEG-TS Streaming**: Complete streaming system with audio and video support

  - HTTP server for client connections
  - MPEG-TS container format
  - Real-time encoding and muxing
  - Multiple concurrent client support
  - Stream URL display in UI

- **Multi-Codec Support**:

  - **H.264 (libx264)**: Full support with quality presets (ultrafast to veryslow)
  - **H.265/HEVC (libx265)**: Support with presets, profiles (main, main10), and levels (1.0 to 6.2)
  - **VP8 (libvpx-vp8)**: Support with speed control (0-16)
  - **VP9 (libvpx-vp9)**: Support with speed control (0-9)
  - Codec selection via UI dropdown
  - Codec-specific configuration options

- **Audio Capture and Streaming**:

  - PulseAudio integration for system audio capture
  - Virtual sink creation for audio routing
  - AAC audio encoding
  - Real-time audio/video synchronization
  - Configurable sample rate and channel count

- **Streaming Architecture**:

  - `StreamManager`: Orchestrates multiple streaming implementations
  - `IStreamer`: Abstract interface for extensible streaming protocols
  - `HTTPTSStreamer`: HTTP MPEG-TS implementation
  - Multi-threaded encoding pipeline (separate thread for encoding)
  - Timestamp-based synchronization system
  - Buffer management with overflow protection

- **UI Enhancements**:

  - New "Stream" tab in settings interface
  - Codec selection dropdown
  - Resolution and FPS dropdowns (replacing text inputs)
  - H.264 quality preset dropdown
  - H.265 preset, profile, and level dropdowns
  - VP8/VP9 speed sliders
  - Video and audio bitrate configuration
  - Stream status display (active clients, stream URL)
  - Start/stop streaming controls

- **Configuration Persistence**:

  - Automatic saving of all settings to `config.json`
  - Automatic loading of settings on application startup
  - Includes streaming configuration, shader parameters, and UI preferences
  - Uses `nlohmann/json` for JSON serialization

- **Synchronization System**:

  - Timestamp-based audio/video synchronization using `CLOCK_MONOTONIC`
  - Master clock synchronization (audio as master)
  - Sync zone calculation for frame pairing
  - Desynchronization detection and automatic recovery
  - Frame skipping for buffer management

- **Build System Improvements**:

  - Automatic copying of `shaders/` directory to build output
  - CMake POST_BUILD custom command for shader deployment
  - Ensures shaders are found at runtime

- **Code Quality**:
  - All compilation warnings resolved
  - Deprecated API usage replaced (`AVFrame::key_frame` → `AV_FRAME_FLAG_KEY`)
  - Unused variables and functions removed
  - Improved code organization and structure

### Changed

- **Architecture Refactoring**:

  - Migrated to smart pointers (`std::unique_ptr`) throughout codebase
  - Applied SOLID principles with extracted utility classes
  - Improved separation of concerns
  - Better memory management and resource cleanup

- **Threading Model**:

  - Changed from single-threaded to hybrid multi-threaded architecture
  - Main thread: capture, processing, rendering, UI
  - Encoding thread: video/audio encoding and muxing
  - Server thread: HTTP client connections
  - Client threads: per-client packet forwarding

- **UI Improvements**:

  - Menu fixed at top, configuration window floating
  - Better organization of streaming settings
  - Improved dropdown interfaces for common settings
  - Renamed `imgui.ini` to `RetroCapture.ini`

- **Streaming Pipeline**:
  - Frame capture moved to main thread with timestamped buffers
  - Encoding happens in dedicated thread
  - Improved frame processing and queue management
  - Better handling of frame drops and buffer overflow

### Fixed

- **Streaming Stability**:

  - Fixed packet corruption issues in MPEG-TS stream
  - Fixed "Invalid NAL unit 0" errors
  - Fixed "unspecified size" errors for video stream
  - Fixed DTS monotonicity violations
  - Fixed audio/video desynchronization
  - Fixed codec packet generation issues (x264 lookahead threads)
  - Fixed `SwsContext` initialization with invalid dimensions (0x1056)
  - Fixed graceful shutdown and cleanup (prevented crashes on stop/restart)
  - Fixed "Address already in use" socket errors
  - Fixed memory corruption and double-free errors
  - Fixed application freezing during streaming

- **UI and Configuration**:

  - Fixed hardcoded FPS values in buffer thresholds
  - Fixed UI value interpretation (dropdown vs text input)
  - Fixed window minimization in fullscreen mode
  - Fixed application pause when losing focus
  - Fixed streaming crash on window resize

- **Codec Issues**:

  - Fixed VP8/VP9 stream recognition (extradata generation)
  - Fixed codec parameter updates
  - Fixed keyframe generation and periodic I-frames
  - Fixed codec state management and flushing

- **Build and Compilation**:
  - Fixed all compiler warnings
  - Fixed deprecated API usage
  - Fixed unused parameter warnings
  - Fixed control flow warnings

### Performance

- Optimized encoding thread to reduce lock contention
- Implemented frame queue with overflow protection
- Optimized FPS with libswscale improvements
- Reduced frame loss with better buffer management
- Optimized audio and video processing pipeline
- Improved frame skipping for buffer management
- Reduced latency with low-latency codec presets

### Technical Details

- **New Dependencies**: FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample), PulseAudio, nlohmann/json
- **New Files**:
  - `src/streaming/` directory with streaming implementation
  - `src/audio/AudioCapture.h/cpp` for audio capture
  - `src/utils/ShaderScanner.h/cpp`, `V4L2DeviceScanner.h/cpp` (extracted utilities)
  - `src/v4l2/V4L2ControlMapper.h/cpp` (extracted utility)
  - `src/shader/ShaderPreprocessor.h/cpp` (extracted from ShaderEngine)
- **Statistics**: 42 commits, ~7,578 lines added, ~1,514 lines removed

### Known Issues

- VP8/VP9 streams may appear as "Data: bin_data" in some players (implementation incomplete)
- Audio/video synchronization may drift under heavy system load
- `glReadPixels` is synchronous and may block main thread at high resolutions
- HTTP streaming has no authentication (stream is publicly accessible)
- Some codec-specific advanced settings not yet exposed in UI

---

## [0.1.0-alpha] - 2025-11-13

### Added

- Initial release
- Real-time V4L2 video capture
- OpenGL 3.3+ rendering with GLFW
- YUYV to RGB conversion
- Logging system
- Low-latency pipeline
- Memory mapping for V4L2 buffers
- Non-blocking capture
- Basic shader support
- Modular architecture (capture, renderer, shader engine)
- Support for RetroArch GLSL shaders and presets (`.glslp`)
- Multi-pass shader preset support
- Graphical user interface with ImGui
- V4L2 hardware controls (brightness, contrast, saturation, hue, gain, exposure, sharpness, gamma, white balance)
- Fullscreen and multi-monitor support
- Aspect ratio maintenance (letterboxing/pillarboxing)
- Real-time shader parameter editing
- Shader preset saving (overwrite and save as)
- V4L2 device selection via GUI
- Dynamic resolution and framerate configuration
- Command-line interface with comprehensive parameter support
- Support for RetroArch uniforms: `OutputSize`, `InputSize`, `SourceSize`, `FrameCount`, `TextureSize`, `MVPMatrix`, `FrameDirection`
- Reference texture loading (LUTs, PNG images)
- Shader parameter extraction from `#pragma parameter` directives
- Automatic shader type detection and correction
- History buffer support for animated shaders

### Changed

- Improved shader path resolution for RetroArch-style relative paths
- Enhanced uniform type detection and injection
- Better error handling and logging

### Fixed

- Image orientation issues
- Shader compilation errors with type mismatches
- Framebuffer management for multi-pass shaders
- Texture binding for reference textures
- Uniform type detection (`vec2`, `vec3`, `vec4` for `OutputSize`)
- V4L2 control value validation and alignment
