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

## [0.5.0-alpha] - 2026-01-15

### Added

- **Video Recording System**: Complete video recording feature with extensive configuration options

  - **Recording Manager**: Full-featured recording system (`RecordingManager`)
    - Real-time video and audio recording to local files
    - Support for multiple video codecs (H.264, H.265, VP8, VP9)
    - Support for multiple audio codecs (AAC, MP3, Opus)
    - Multiple container formats (MP4, MKV, AVI)
    - Configurable resolution, frame rate, and bitrates
    - Optional audio recording with synchronization
    - Dedicated encoding thread for non-blocking recording
    - Media synchronization using `MediaSynchronizer`
  
  - **Recording UI**: Complete recording interface in both local and web UIs
    - **Local UI (ImGui)**: Dedicated recording tab with full configuration
      - Real-time recording status (active/inactive, duration, file size, filename)
      - Resolution and FPS selection (with presets or match capture)
      - Codec selection and codec-specific options
      - Bitrate controls with visual sliders
      - Container format selection
      - Output path and filename template configuration
      - Start/Stop recording button with visual feedback
    - **Web Portal**: Full recording control through web interface
      - Recording configuration and management
      - Recording list with thumbnails
      - Download recordings directly from web interface
      - Delete and rename recordings
  
  - **Recording Management**: Complete recording lifecycle management
    - Recording list with metadata (name, duration, size, timestamp)
    - Automatic thumbnail generation (extracted from first frame)
    - Recording metadata persistence (JSON-based)
    - Delete and rename recordings
    - Download recordings via web interface
  
  - **Configuration Persistence**: All recording settings are automatically saved
    - Video settings (resolution, FPS, codec, bitrate, presets)
    - Audio settings (codec, bitrate, include audio flag)
    - Output settings (container, output path, filename template)
    - Settings restored on application startup
  
  - **FileRecorder**: Specialized file recording implementation
    - Writes encoded data to local files
    - Supports MP4, MKV, and AVI containers
    - Automatic directory creation
    - File size and duration tracking
  
  - **RecordingMetadata**: Metadata management for recordings
    - JSON-based metadata storage
    - Thumbnail generation and storage
    - Recording information tracking

- **Raspberry Pi Support**: Native ARM builds for Raspberry Pi

  - **ARM32v7 (ARMv7)**: Support for Raspberry Pi 2, 3, and Zero
  - **ARM64v8 (ARM64)**: Support for Raspberry Pi 4 and newer
  - **Docker-based Builds**: Automated Docker builds for ARM architectures
  - **DirectFB Support**: SDL2 with DirectFB for headless operation on ARM
  - **Build Scripts**: Automated build scripts for Raspberry Pi deployment
    - `build-on-raspberry-pi.sh`: Build directly on Raspberry Pi
    - `build-linux-arm32v7-docker.sh`: Cross-compile ARM32v7 from x86_64
    - `build-linux-arm64v8-docker.sh`: Cross-compile ARM64v8 from x86_64
    - `install-deps-raspberry-pi.sh`: Install dependencies on Raspberry Pi
    - `sync-source-raspiberry.sh`: Sync source code to Raspberry Pi for building

- **Capture Presets**: Save and load capture configurations

  - **Preset System**: Save complete capture configurations
    - Device selection
    - Resolution and framerate
    - Hardware controls (V4L2/DirectShow)
    - Shader selection
    - All capture-related settings
  - **Preset Management**: Load, save, and manage presets
    - Save presets with custom names
    - Load presets to restore complete configuration
    - Preset persistence across application restarts

- **Audio Configuration Improvements**:

  - **PulseAudio Integration**: Enhanced PulseAudio support on Linux
    - Automatic cleanup of orphaned sinks and loopbacks
    - Proper sink naming and identification
    - Thread-safe mainloop access
    - Connection persistence across restarts
  - **Audio Device Selection**: Improved audio device management
    - Input source selection and persistence
    - Configuration saved and restored automatically
    - Web and local UI support for audio device selection

- **Standalone Script**: Automated audio routing script for PipeWire

  - **retrocapture-standalone.sh**: Complete standalone deployment script
    - Automatic PipeWire link creation
    - Dynamic loopback ID discovery
    - Watchdog for application monitoring
    - Automatic reconnection on restart
    - Output sink detection and fallback

### Changed

- **Architecture Refactoring**:
  - Renamed `StreamSynchronizer` to `MediaSynchronizer` for clarity
  - Shared synchronization system between streaming and recording
  - Improved code reuse between streaming and recording features
  - Better separation of concerns in encoding pipeline

- **Build System**:
  - Updated Docker base to Ubuntu 24.04 for better compatibility
  - Enhanced cross-compilation support for ARM architectures
  - Improved Docker build scripts for multiple platforms
  - Better dependency management for ARM builds

- **Configuration System**:
  - Extended configuration persistence to include recording settings
  - Added output resolution (outputWidth, outputHeight) to image settings
  - Improved configuration loading and saving reliability

- **UI Improvements**:
  - Modular recording UI components
  - Better organization of recording settings
  - Improved visual feedback for recording status
  - Enhanced web portal recording interface

### Fixed

- **Recording System**:
  - Fixed frame capture for recording with shaders applied
  - Fixed MP4 recording file truncation and moov atom issues
  - Fixed MKV recording H.264 extradata extraction and memory corruption
  - Fixed recording deletion dialog and file handling
  - Fixed thumbnail generation and deletion
  - Fixed filesystem compatibility issues with recording paths
  - Fixed frame capture logic to match streaming behavior

- **Audio System**:
  - Fixed PulseAudio mainloop thread-safety issues
  - Fixed audio sink cleanup on application close
  - Fixed audio connection persistence
  - Fixed orphaned sink and loopback cleanup

- **Web Portal**:
  - Fixed web portal navigation and cache issues
  - Fixed text visibility in recording cards
  - Fixed recording management UI responsiveness

- **Build System**:
  - Fixed Docker build compatibility issues
  - Fixed ARM build dependencies
  - Fixed filesystem compatibility in build scripts

### Technical Details

- **New Dependencies**: 
  - No new external dependencies (reuses existing FFmpeg libraries)
- **New Files**:
  - `src/recording/RecordingManager.h/cpp`: Recording orchestration
  - `src/recording/FileRecorder.h/cpp`: File recording implementation
  - `src/recording/RecordingSettings.h`: Recording configuration structure
  - `src/recording/RecordingMetadata.h/cpp`: Recording metadata management
  - `src/ui/UIConfigurationRecording.h/cpp`: Recording configuration UI
  - `src/ui/UIRecordings.h/cpp`: Recording management UI
  - `tools/retrocapture-standalone.sh`: Standalone deployment script
  - `tools/build-linux-arm32v7-docker.sh`: ARM32v7 build script
  - `tools/build-linux-arm64v8-docker.sh`: ARM64v8 build script
  - `tools/build-on-raspberry-pi.sh`: Native Raspberry Pi build script
  - `tools/install-deps-raspberry-pi.sh`: Raspberry Pi dependency installer
  - `tools/sync-source-raspiberry.sh`: Source sync script for Raspberry Pi
  - `tools/check-directfb.sh`: DirectFB compatibility checker
- **Modified Files**:
  - `src/core/Application.cpp`: Recording integration
  - `src/ui/UIManager.h/cpp`: Recording UI integration
  - `src/streaming/APIController.cpp`: Recording API endpoints
  - `src/web/control.js`: Recording web interface
  - `src/web/style.css`: Recording UI styles
  - `CMakeLists.txt`: Recording build configuration
  - `tools/build-windows-installer.sh`: Updated for new features

---

## [0.4.0-alpha] - 2025-12-15

### Added

- **Cross-Platform Support**: Native Windows support alongside existing Linux support

  - **Windows Video Capture**: DirectShow implementation (`VideoCaptureDS`)
    - Enumeration of DirectShow capture devices
    - Hardware controls (brightness, contrast, saturation, etc.)
    - Dummy mode for testing without capture device
    - Compatible with MinGW/MXE and works in Wine
  
  - **Windows Audio Capture**: WASAPI implementation (`AudioCaptureWASAPI`)
    - System audio capture via Windows Audio Session API
    - Device enumeration via MMDevice API
    - Thread-based capture for better performance
  
  - **Cross-Platform Architecture**:
    - Abstract interfaces: `IVideoCapture` and `IAudioCapture`
    - Factory pattern for platform-specific implementations
    - Automatic platform detection in build system
    - Shared code between platforms where possible

- **Windows Distribution**:
  - **Windows Installer (NSIS)**: Complete installer with all components
    - Executable with embedded icon
    - All required DLLs
    - Shaders, assets, web portal, and SSL certificates
    - Start Menu shortcuts with proper icons
  - **Application Icon**: Embedded icon in Windows executable
    - Icon appears in taskbar, file explorer, and shortcuts
    - Icon configured in installer and application shortcuts

- **Linux Improvements**:
  - **WM_CLASS Configuration**: Proper application identification in window managers
    - Application appears correctly in taskbar/launcher
    - Proper window manager integration using X11
    - Fixed "Unknown" application name issue

- **Web Portal Enhancements**:
  - **Platform Detection**: Automatic platform detection in web interface
  - **Windows API Endpoints**: DirectShow-specific API endpoints
    - `GET /api/v1/platform`: Platform and source type information
    - `GET /api/v1/ds/devices`: DirectShow device enumeration
    - `POST /api/v1/ds/device`: DirectShow device selection
  - **Adaptive UI**: Interface adapts based on detected platform

- **Build System**:
  - Cross-compilation support for Windows from Linux
  - Docker-based build system for Windows
  - MXE/MinGW-w64 support
  - Automatic platform-specific file exclusion

### Changed

- **Architecture Refactoring**:
  - Migrated to abstract interfaces for video and audio capture
  - Factory pattern for platform-specific implementations
  - Improved code organization and separation of concerns
  - Better cross-platform compatibility

- **Documentation**:
  - Updated README.md with cross-platform information
  - Updated ARCHITECTURE.md with multiplatform architecture details
  - Replaced version-specific "What's New" sections with general "Key Features"
  - Removed temporary planning documents

- **Code Standardization**:
  - Standardized code formatting (braces on new lines, spacing)
  - Translated log messages and comments from Portuguese to English
  - Consistent code style across all files

### Fixed

- **Linux Window Manager**:
  - Fixed application appearing as "Unknown" in taskbar/launcher
  - Proper WM_CLASS configuration using X11

- **Windows Networking**:
  - Fixed Winsock initialization for Windows networking
  - Proper cleanup of Winsock resources

### Technical Details

- **New Dependencies**: 
  - Windows: DirectShow (COM), WASAPI, Winsock2
  - Linux: X11 (for window manager integration)
- **New Files**:
  - `src/capture/IVideoCapture.h`: Video capture interface
  - `src/capture/VideoCaptureFactory.h/cpp`: Video capture factory
  - `src/capture/VideoCaptureDS.h/cpp`: Windows DirectShow implementation
  - `src/audio/IAudioCapture.h`: Audio capture interface
  - `src/audio/AudioCaptureFactory.h/cpp`: Audio capture factory
  - `src/audio/AudioCaptureWASAPI.h/cpp`: Windows WASAPI implementation
  - `src/retrocapture.rc`: Windows resource file for icon
  - `assets/logo.ico`: Application icon
- **Modified Files**:
  - `CMakeLists.txt`: Cross-platform build configuration
  - `src/core/Application.cpp`: Factory usage
  - `src/output/WindowManager.cpp`: WM_CLASS configuration (Linux)
  - `src/streaming/HTTPServer.cpp`: Winsock support (Windows)
  - `src/streaming/APIController.cpp`: Windows API endpoints
  - `src/web/control.js`: Platform detection
  - `README.md`: Cross-platform documentation
  - `docs/ARCHITECTURE.md`: Multiplatform architecture

---

## [0.3.0-alpha] - 2025-12-03

### Added

- **Web Portal with Remote Control**: Complete web-based control interface

  - Full REST API for remote control of all application features
  - Real-time status updates and bidirectional communication
  - Control capture sources, shaders, resolution, FPS, image adjustments, streaming settings, and V4L2 controls
  - Modern, responsive UI with RetroCapture styleguide
  - Customizable colors, images, and text labels
  - Independent operation from streaming (can be enabled/disabled separately)

- **Progressive Web App (PWA) Support**:

  - Installable on mobile devices and desktop
  - Service worker for offline functionality and caching
  - App manifest with icons and metadata
  - Works on local network IPs (requires HTTPS)

- **HTTPS/SSL Support**:

  - Optional HTTPS for web portal and streaming
  - Self-signed certificate support for local development
  - Configurable SSL certificate and key paths
  - Support for Subject Alternative Names (SANs) for local network IPs

- **Source Type Selection**:

  - New `--source` parameter to select source type (none, v4l2)
  - Dummy mode for testing without capture device
  - V4L2 controls only applied when source is V4L2

- **Enhanced CLI Parameters**:

  - `--source <type>`: Select source type (none, v4l2)
  - `--v4l2-device <path>`: V4L2 device path (renamed from --device for consistency)
  - `--web-portal-enable/--web-portal-disable`: Enable/disable web portal
  - `--web-portal-port <port>`: Web portal port
  - `--web-portal-https`: Enable HTTPS
  - `--web-portal-ssl-cert <path>`: SSL certificate path
  - `--web-portal-ssl-key <path>`: SSL key path

- **REST API Endpoints**:

  - `GET /api/source`: Get current source configuration
  - `POST /api/source`: Set source type and device
  - `GET /api/shader`: Get current shader
  - `POST /api/shader`: Set shader
  - `GET /api/shader/list`: Get available shaders
  - `GET /api/shader/parameters`: Get shader parameters
  - `POST /api/shader/parameter`: Set shader parameter
  - `GET /api/capture/resolution`: Get capture resolution
  - `POST /api/capture/resolution`: Set capture resolution
  - `GET /api/capture/fps`: Get capture FPS
  - `POST /api/capture/fps`: Set capture FPS
  - `GET /api/image`: Get image settings
  - `POST /api/image`: Set image settings
  - `GET /api/streaming/settings`: Get streaming settings
  - `POST /api/streaming/settings`: Set streaming settings
  - `POST /api/streaming/control`: Start/stop streaming
  - `GET /api/streaming/status`: Get streaming status
  - `GET /api/v4l2/devices`: Get available V4L2 devices
  - `POST /api/v4l2/devices/refresh`: Refresh V4L2 device list
  - `GET /api/v4l2/controls`: Get V4L2 controls
  - `POST /api/v4l2/control`: Set V4L2 control value
  - `POST /api/v4l2/device`: Set V4L2 device

- **Streaming Cooldown System**:

  - 10-second cooldown after stopping streaming
  - Prevents rapid start/stop cycles that could cause issues
  - Visual feedback in both local and web UIs
  - API returns cooldown status and remaining time

- **UI Refactoring**:

  - Modular UI architecture with separate classes per window/tab
  - `UIConfiguration`: Main configuration window
  - `UIConfigurationSource`: Source selection and V4L2 controls
  - `UIConfigurationImage`: Image adjustments
  - `UIConfigurationShader`: Shader selection and parameters
  - `UIConfigurationStreaming`: Streaming configuration
  - `UIConfigurationWebPortal`: Web portal settings
  - `UIInfoPanel`: Information display
  - Better code organization and maintainability

- **Real-time Updates**:
  - All controls update in real-time without apply/save buttons
  - Immediate feedback for all parameter changes
  - Synchronized state between local UI and web portal

### Changed

- **CLI Parameter Reorganization**:

  - `--device` renamed to `--v4l2-device` for consistency
  - V4L2 parameters grouped together in help
  - V4L2 parameters only applied when `--source v4l2`
  - Improved parameter validation and error messages

- **Web Portal Architecture**:

  - Web portal can run independently of streaming
  - Same HTTP server used for both portal and streaming
  - Improved request routing and static file serving
  - Better error handling and 404 responses

- **Streaming Architecture**:

  - Simplified resource management
  - Better thread synchronization
  - Improved cleanup and shutdown procedures

- **Audio Processing**:
  - Continuous PulseAudio mainloop processing even when streaming is inactive
  - Prevents system audio freezing
  - Multiple mainloop iterations per call for better responsiveness

### Fixed

- **Audio System**:

  - Fixed critical issue where RetroCapture would freeze entire PC audio after extended use
  - Audio now processes correctly even when streaming is not active
  - Proper PulseAudio mainloop handling

- **Streaming Stability**:

  - Fixed heap corruption issues during streaming stop/restart
  - Improved resource cleanup and deallocation
  - Better error handling and recovery

- **Web Portal**:

  - Fixed V4L2 device list not populating correctly
  - Fixed CSS issues with sliders and controls
  - Fixed infinite recursion in slider controls
  - Fixed streaming status not updating in frontend
  - Fixed button state management during start/stop operations

- **UI**:
  - Fixed button being clickable multiple times during stop operation
  - Fixed processing state not being properly displayed
  - Fixed cooldown display in both local and web UIs

### Removed

- **Unused Code**:
  - Removed `HTTPMJPEGStreamer` class (not being used)
  - Removed HLS/player.js related code (file doesn't exist)
  - Removed `setHLSParameters()` method and related variables
  - Removed `libmicrohttpd` dependency from CMakeLists.txt
  - Removed `--stream-quality` CLI parameter (was for MJPEG)
  - Removed `--stream-audio-buffer-size` CLI parameter (managed automatically)
  - Removed `setStreamingQuality()` method and `m_streamingQuality` variable

### Technical Details

- **New Dependencies**: OpenSSL (optional, for HTTPS support)
- **New Files**:
  - `src/streaming/WebPortal.h/cpp`: Web portal implementation
  - `src/streaming/HTTPServer.h/cpp`: HTTP/HTTPS server implementation
  - `src/streaming/APIController.h/cpp`: REST API controller
  - `src/ui/UIConfiguration*.h/cpp`: Modular UI components
  - `src/web/`: Web portal static files (HTML, CSS, JS)
  - `src/web/manifest.json`: PWA manifest
  - `src/web/service-worker.js`: Service worker for PWA
- **Statistics**: 29 commits, significant refactoring and new features

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
