# RetroCapture Architecture

## Overview

RetroCapture is a real-time video capture application for Linux and Windows that applies RetroArch shaders (GLSL) to video feeds from capture cards and streams the processed output over HTTP. The architecture was designed to be modular, efficient, and low-latency, with support for multiple video codecs (H.264, H.265, VP8, VP9) and real-time audio/video synchronization. The application uses a cross-platform architecture with platform-specific implementations for video and audio capture.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                             │
│                    (Main Orchestrator)                          │
│                  (uses std::unique_ptr)                         │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ VideoCapture │    │ WindowManager│    │ ShaderEngine │
│ (V4L2/DS)    │    │   (GLFW)     │    │  (OpenGL)    │
└──────────────┘    └──────────────┘    └──────────────┘
        │                     │                       │
        │                     ▼                       │
        │            ┌──────────────┐                 │
        │            │ OpenGLRenderer│                │
        │            │   (OpenGL)    │                │
        │            └──────────────┘                 │
        │                     ▲                       │
        │                     │                       │
        ▼                     │                       │
┌──────────────┐              │                       │
│FrameProcessor│──────────────┘                       │
│  (YUYV→RGB)  │                                      │
│  (Textures)  │                                      │
└──────────────┘                                      │
        │                                             │
        └─────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ AudioCapture │    │StreamManager │    │  UIManager   │
│(Pulse/WASAPI)│    │  (Orchestr.) │    │   (ImGui)    │
└──────────────┘    └──────────────┘    └──────────────┘
        │                     │
        │                     ▼
        │            ┌──────────────┐
        │            │HTTPTSStreamer│
        │            │  (FFmpeg)    │
        │            └──────────────┘
        │                     │
        └─────────────────────┘
                    │
                    ▼
            HTTP MPEG-TS Stream
            (H.264/H.265/VP8/VP9 + AAC)

Utility Classes:
├── ShaderPreprocessor (shader preprocessing)
├── V4L2ControlMapper (control name→ID mapping)
├── ShaderScanner (shader discovery)
└── V4L2DeviceScanner (device discovery)
```

## Main Components

### 1. Application (`src/core/Application.h/cpp`)

**Responsibility**: Orchestration of all components and application lifecycle management.

**Main Features**:

- Initialization and shutdown of all components
- Main rendering loop
- Coordination between capture, shaders, and rendering
- Event handling (keyboard input, window events)

**Dependencies**:

- `VideoCapture`: Frame capture
- `WindowManager`: Window management
- `OpenGLRenderer`: OpenGL rendering
- `FrameProcessor`: Frame processing and texture management
- `ShaderEngine`: Shader processing
- `AudioCapture`: Audio capture from PulseAudio
- `StreamManager`: Streaming orchestration
- `UIManager`: Graphical interface

**Memory Management**:

- Uses `std::unique_ptr` for automatic memory management
- All components are owned by `Application` and automatically cleaned up

**Data Flow**:

```
VideoCapture (V4L2/DS) → FrameProcessor (YUYV→RGB, Texture) → ShaderEngine → OpenGLRenderer → Window
                                                                              ↓
                                                                    StreamManager → HTTPTSStreamer → HTTP Stream
AudioCapture (Pulse/WASAPI) → StreamManager → HTTPTSStreamer → HTTP Stream
```

### 2. VideoCapture (`src/capture/IVideoCapture.h` and implementations)

**Responsibility**: Cross-platform video capture abstraction.

**Architecture**:

- **Interface**: `IVideoCapture` provides abstract interface for video capture
- **Linux Implementation**: `VideoCaptureV4L2` uses V4L2 (Video4Linux2)
- **Windows Implementation**: `VideoCaptureDS` uses DirectShow
- **Factory Pattern**: `VideoCaptureFactory` creates platform-specific instances

**Main Features**:

- Opening/closing capture devices
- Format configuration (resolution, pixel format, framerate)
- Hardware controls (brightness, contrast, saturation, etc.)
- Frame capture
- Device enumeration
- Dummy mode for testing without capture device

**Main API**:

```cpp
bool open(const std::string& device);
bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat);
bool setFramerate(uint32_t fps);
bool captureFrame(Frame& frame);
bool setControl(const std::string& controlName, int32_t value);
std::vector<DeviceInfo> listDevices();
void setDummyMode(bool enabled);
```

**Frame Format**:

```cpp
struct Frame {
    uint8_t* data;      // Frame data
    size_t size;        // Size in bytes
    uint32_t width;     // Width
    uint32_t height;    // Height
    uint32_t format;    // Platform-specific format (V4L2_PIX_FMT_YUYV on Linux)
};
```

**Platform-Specific Details**:

- **Linux (V4L2)**: Uses memory mapping (mmap) for efficient frame capture, supports V4L2 controls
- **Windows (DirectShow)**: Uses DirectShow COM interfaces, supports DirectShow camera controls

### 3. WindowManager (`src/output/WindowManager.h/cpp`)

**Responsibility**: Window and OpenGL context management via GLFW.

**Main Features**:

- Window creation/destruction
- OpenGL context management
- Fullscreen and multi-monitor support
- Event callbacks (resize, etc.)
- VSync
- **Linux**: WM_CLASS setting for proper window manager identification

**Main API**:

```cpp
bool init(const WindowConfig& config);
void setFullscreen(bool fullscreen, int monitorIndex = -1);
void setResizeCallback(std::function<void(int, int)> callback);
void swapBuffers();
void pollEvents();
```

**Platform-Specific Features**:

- **Linux**: Sets WM_CLASS property using X11 to ensure proper application identification in taskbar/launcher
- **Windows**: Uses native window properties for proper taskbar integration

### 4. OpenGLRenderer (`src/renderer/OpenGLRenderer.h/cpp`)

**Responsibility**: Low-level OpenGL rendering.

**Main Features**:

- OpenGL texture creation and updates
- Quad rendering with texture
- Aspect ratio support (letterboxing/pillarboxing)
- Brightness and contrast controls
- V4L2 to OpenGL format conversion

**Main API**:

```cpp
GLuint createTextureFromFrame(const uint8_t* data, uint32_t width, uint32_t height, uint32_t format);
void updateTexture(GLuint texture, const uint8_t* data, uint32_t width, uint32_t height, uint32_t format);
void renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight,
                   bool flipY = true, bool enableBlend = false,
                   float brightness = 1.0f, float contrast = 1.0f,
                   bool maintainAspect = false, uint32_t textureWidth = 0, uint32_t textureHeight = 0);
```

### 5. FrameProcessor (`src/processing/FrameProcessor.h/cpp`)

**Responsibility**: Processing video frames from V4L2 capture and converting them to OpenGL textures.

**Main Features**:

- Frame capture from `VideoCapture`
- YUYV to RGB conversion
- OpenGL texture creation and management
- Texture updates (reuses textures when possible)
- Handles texture resizing when capture resolution changes

**Main API**:

```cpp
bool processFrame(VideoCapture* capture);
GLuint getTexture() const;
uint32_t getTextureWidth() const;
uint32_t getTextureHeight() const;
bool hasValidFrame() const;
void deleteTexture();
```

### 6. ShaderEngine (`src/shader/ShaderEngine.h/cpp`)

**Responsibility**: Loading, compilation, and application of RetroArch shaders.

**Main Features**:

- Loading RetroArch presets (`.glslp`)
- Multiple pass management
- RetroArch uniform injection (`OutputSize`, `InputSize`, `FrameCount`, etc.)
- Intermediate framebuffer management
- Reference texture loading (LUTs, etc.)
- Support for configurable parameters (`#pragma parameter`)

**Note**: Shader preprocessing (includes, parameter extraction, OutputSize correction) is handled by `ShaderPreprocessor`.

**Main API**:

```cpp
bool loadPreset(const std::string& presetPath);
GLuint applyShader(GLuint inputTexture, uint32_t inputWidth, uint32_t inputHeight);
void setViewport(uint32_t width, uint32_t height);
std::vector<ShaderParameter> getShaderParameters() const;
bool setShaderParameter(const std::string& name, float value);
```

**Pass Structure**:

```cpp
struct ShaderPassData {
    GLuint program;              // Compiled shader program
    GLuint vertexShader;         // Vertex shader
    GLuint fragmentShader;       // Fragment shader
    GLuint framebuffer;          // Framebuffer for output
    GLuint texture;              // Output texture
    uint32_t width;              // Pass width
    uint32_t height;             // Pass height
    bool floatFramebuffer;       // Uses float framebuffer?
    ShaderPass passInfo;         // Preset information
    std::map<std::string, float> extractedParameters; // Shader parameters
};
```

**Supported RetroArch Uniforms**:

- `SourceSize`, `InputSize`, `OutputSize`: Sizes (vec2)
- `FrameCount`: Frame counter (int or float)
- `TextureSize`: Current texture size (vec2)
- `MVPMatrix`: Model-View-Projection matrix (mat4)
- `FrameDirection`: Frame direction (int)
- `Texture`, `Source`, `Input`: Input texture
- `PassPrev#Texture`: Previous pass textures
- `PassOutputSize#`, `PassInputSize#`: Pass sizes
- Custom parameters via `#pragma parameter`

### 7. ShaderPreset (`src/shader/ShaderPreset.h/cpp`)

**Responsibility**: Parsing and management of RetroArch preset files (`.glslp`).

**Main Features**:

- Parsing `.glslp` files
- Resolving relative shader paths
- Reference texture loading
- Preset parameter management
- Saving modified presets

**Preset Format**:

```
shader0 = "path/to/shader.glsl"
scale_type0 = "source"
scale0 = 1.0
filter_linear0 = "true"
wrap_mode0 = "clamp_to_edge"
...
```

### 8. ShaderPreprocessor (`src/shader/ShaderPreprocessor.h/cpp`)

**Responsibility**: Preprocessing GLSL shader source code.

**Main Features**:

- Processing `#include` directives (recursive)
- Extracting `#pragma parameter` directives
- Correcting `OutputSize` uniform type based on usage
- Building final shader source with version, extensions, and defines
- Injecting compatibility code for specific shaders (interlacing, box-center, etc.)

**Main API**:

```cpp
static PreprocessResult preprocess(
    const std::string& shaderSource,
    const std::string& shaderPath,
    size_t passIndex,
    uint32_t outputWidth, uint32_t outputHeight,
    uint32_t inputWidth, uint32_t inputHeight,
    const std::vector<ShaderPass>& presetPasses);
static std::string processIncludes(const std::string& source, const std::string& basePath);
```

**Preprocessing Approach**:

- Uses the same source code for both vertex and fragment shaders
- Adds `#define VERTEX` or `#define FRAGMENT` before the code
- Lets the GLSL preprocessor handle conditional blocks (`#if defined(VERTEX)` / `#elif defined(FRAGMENT)`)

### 9. UIManager (`src/ui/UIManager.h/cpp`)

**Responsibility**: Graphical interface using ImGui.

**Main Features**:

- Shader selection interface
- Brightness and contrast controls
- V4L2 controls (brightness, contrast, saturation, etc.)
- Resolution and framerate configuration
- V4L2 device selection
- Shader parameter editing
- Saving modified presets

**Interface Tabs**:

- **Shaders**: Shader list, editable parameters, save preset
- **Image**: Brightness, contrast, maintain aspect ratio, fullscreen, monitor
- **V4L2**: Hardware controls, resolution, framerate, device
- **Stream**: Streaming configuration (codec, resolution, FPS, bitrate, quality presets)
- **Info**: Capture and application information

**Utility Classes Used**:

- `ShaderScanner`: Scans for shader presets in directories
- `V4L2DeviceScanner`: Scans for V4L2 capture devices
- `V4L2ControlMapper`: Maps control names to V4L2 control IDs

**Configuration Persistence**:

- Settings are automatically saved to `config.json` on changes
- Settings are loaded on application startup
- Includes streaming configuration, shader parameters, and UI preferences

### 10. AudioCapture (`src/audio/IAudioCapture.h` and implementations)

**Responsibility**: Cross-platform audio capture abstraction.

**Architecture**:

- **Interface**: `IAudioCapture` provides abstract interface for audio capture
- **Linux Implementation**: `AudioCapturePulse` uses PulseAudio
- **Windows Implementation**: `AudioCaptureWASAPI` uses WASAPI (Windows Audio Session API)
- **Factory Pattern**: `AudioCaptureFactory` creates platform-specific instances

**Main Features**:

- System audio capture
- Device enumeration
- 16-bit PCM audio capture
- Configurable sample rate and channel count
- Thread-safe audio buffer management
- Callback-based audio delivery

**Main API**:

```cpp
bool open(const std::string& deviceName = "");
void close();
bool startCapture();
void stopCapture();
size_t getSamples(int16_t* buffer, size_t maxSamples);
std::vector<AudioDeviceInfo> listDevices();
```

**Audio Format**:

- **Sample Format**: 16-bit signed little-endian PCM
- **Default Sample Rate**: 44100 Hz
- **Default Channels**: 2 (stereo)
- **Buffer Size**: Platform-specific (100ms fragments on Linux)

**Platform-Specific Details**:

- **Linux (PulseAudio)**: Creates virtual sink named "RetroCapture" for routing system audio, visible in PulseAudio tools (e.g., `qpwgraph`)
- **Windows (WASAPI)**: Uses WASAPI loopback capture for system audio, supports device enumeration via MMDevice API

### 11. StreamManager (`src/streaming/StreamManager.h/cpp`)

**Responsibility**: Orchestration and management of multiple streaming implementations.

**Main Features**:

- Manages multiple `IStreamer` instances
- Distributes frames and audio to all active streamers
- Coordinates initialization, start, and stop operations
- Provides unified interface for streaming operations

**Main API**:

```cpp
void addStreamer(std::unique_ptr<IStreamer> streamer);
bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps);
bool start();
void stop();
void pushFrame(const uint8_t* data, uint32_t width, uint32_t height);
void pushAudio(const int16_t* samples, size_t sampleCount);
std::vector<std::string> getStreamUrls() const;
uint32_t getTotalClientCount() const;
```

**Architecture**:

- Uses the `IStreamer` interface for abstraction
- Supports multiple streamers simultaneously (e.g., HTTP MJPEG, HTTP MPEG-TS)
- Thread-safe frame and audio distribution

### 12. HTTPTSStreamer (`src/streaming/HTTPTSStreamer.h/cpp`)

**Responsibility**: HTTP MPEG-TS streaming with FFmpeg encoding.

**Main Features**:

- Multi-codec support: H.264 (libx264), H.265 (libx265), VP8 (libvpx-vp8), VP9 (libvpx-vp9)
- AAC audio encoding
- MPEG-TS container muxing
- HTTP server for client connections
- Real-time audio/video synchronization
- Dynamic resolution and FPS configuration
- Configurable bitrates and quality presets
- Keyframe management and periodic I-frames
- Desynchronization detection and recovery
- Frame skipping for buffer management

**Main API**:

```cpp
bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps);
bool start();
void stop();
bool pushFrame(const uint8_t* data, uint32_t width, uint32_t height);
bool pushAudio(const int16_t* samples, size_t sampleCount);
void setVideoCodec(const std::string& codecName);
void setH264Preset(const std::string& preset);
void setH265Preset(const std::string& preset);
void setH265Profile(const std::string& profile);
void setH265Level(const std::string& level);
void setVP8Speed(int speed);
void setVP9Speed(int speed);
```

**Threading Architecture**:

- **Main Thread**: Receives frames and audio via `pushFrame()` and `pushAudio()`
- **Server Thread** (`serverThread`): Accepts HTTP client connections
- **Encoding Thread** (`encodingThread`): Encodes video/audio and muxes into MPEG-TS
- **Client Threads** (`handleClient`): Detached threads for each connected client

**Synchronization**:

- **Timestamp-based**: Uses `CLOCK_MONOTONIC` for frame timestamps
- **Master Clock**: Audio serves as master clock for synchronization
- **Buffer Management**: Separate buffers for video and audio with timestamp ordering
- **Sync Zone**: Calculates overlap zone between video and audio buffers for synchronized encoding
- **Desynchronization Detection**: Monitors PTS/DTS jumps and forces keyframes when needed

**Codec Configuration**:

**H.264 (libx264)**:

- Presets: `ultrafast`, `superfast`, `veryfast`, `faster`, `fast`, `medium`, `slow`, `slower`, `veryslow`
- Tune: `zerolatency` for low latency
- Keyframe interval: 1 second (configurable via `gop_size`)
- Threads: 1 (disables lookahead for immediate packet generation)

**H.265 (libx265)**:

- Presets: Same as H.264
- Profiles: `main`, `main10`
- Levels: `auto`, `1`, `2`, `2.1`, `3`, `3.1`, `4`, `4.1`, `5`, `5.1`, `5.2`, `6`, `6.1`, `6.2`

**VP8/VP9 (libvpx)**:

- Speed: 0-16 (VP8), 0-9 (VP9) - lower = better quality, higher = faster encoding
- Deadline: `realtime` for low latency
- Keyframe interval: 1 second
- Threads: 1

**Frame Processing Pipeline**:

```
1. pushFrame() / pushAudio()
   └─▶ Timestamp frame/audio with CLOCK_MONOTONIC
       └─▶ Add to timestamped buffer (m_timestampedVideoBuffer / m_timestampedAudioBuffer)

2. encodingThread
   └─▶ Calculate sync zone (overlap between video and audio buffers)
       └─▶ For each frame in sync zone:
           ├─▶ encodeVideoFrame()
           │   ├─▶ Convert RGB → YUV420p (SwsContext)
           │   ├─▶ Resize to stream resolution (if needed)
           │   ├─▶ Send frame to codec (avcodec_send_frame)
           │   └─▶ Receive packets (avcodec_receive_packet)
           │
           └─▶ encodeAudioFrame()
               ├─▶ Accumulate samples until frame size
               ├─▶ Convert int16 → float planar (SwrContext)
               ├─▶ Send frame to codec (avcodec_send_frame)
               └─▶ Receive packets (avcodec_receive_packet)

3. muxPacket()
   └─▶ Set PTS/DTS (ensuring monotonicity)
       └─▶ Mux packet into MPEG-TS (av_interleaved_write_frame)
           └─▶ writeCallback() → writeToClients()
               └─▶ Send to all connected clients via HTTP
```

**Buffer Management**:

- **Video Buffer**: `std::deque<TimestampedFrame>` with max 200 frames (overflow protection)
- **Audio Buffer**: `std::deque<TimestampedAudio>` with max 200 chunks
- **Overflow Protection**: Old frames are discarded when buffer exceeds limits
- **Desynchronization Recovery**: Aggressive frame skipping when desync detected

**Graceful Shutdown**:

- `m_stopRequest` flag signals all threads to exit gracefully
- Threads check `m_stopRequest` in their main loops
- `stop()` sets flags, closes sockets, waits for threads, then cleans up FFmpeg resources
- Idempotent cleanup prevents double-free errors

### 13. IStreamer (`src/streaming/IStreamer.h`)

**Responsibility**: Abstract interface for streaming implementations.

**Main Features**:

- Defines common streaming operations
- Allows multiple streaming implementations (HTTP MJPEG, HTTP MPEG-TS, etc.)
- Provides abstraction for `StreamManager`

**Interface**:

```cpp
virtual bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) = 0;
virtual bool start() = 0;
virtual void stop() = 0;
virtual bool pushFrame(const uint8_t* data, uint32_t width, uint32_t height) = 0;
virtual bool pushAudio(const int16_t* samples, size_t sampleCount) = 0;
virtual std::string getStreamUrl() const = 0;
virtual uint32_t getClientCount() const = 0;
```

## Data Flow

### Rendering Pipeline

```
1. VideoCapture::captureLatestFrame()
   └─▶ Frame (YUYV) from capture device (V4L2 on Linux, DirectShow on Windows)

2. FrameProcessor::processFrame()
   └─▶ YUYV → RGB conversion
       └─▶ OpenGL texture creation/update
           └─▶ OpenGL Texture (GLuint)

3. ShaderEngine::applyShader()
   └─▶ For each preset pass:
       ├─▶ Bind pass framebuffer
       ├─▶ Setup uniforms (OutputSize, InputSize, etc.)
       ├─▶ Bind textures (input, reference, previous passes)
       ├─▶ Render quad with shader
       └─▶ Output → next pass texture

4. OpenGLRenderer::renderTexture()
   └─▶ Render final texture to screen
       └─▶ With aspect ratio, brightness, contrast support

5. UIManager::render()
   └─▶ Render ImGui interface over image
```

### Main Loop

```cpp
while (!window->shouldClose()) {
    // 1. Process events
    window->pollEvents();
    handleKeyInput();

    // 2. Capture and process frame
    if (m_frameProcessor->processFrame(m_capture.get())) {
        // 3. Apply shader (if any)
        GLuint outputTexture = m_frameProcessor->getTexture();
        if (m_shaderEngine && m_shaderEngine->isShaderActive()) {
            outputTexture = m_shaderEngine->applyShader(
                m_frameProcessor->getTexture(),
                m_frameProcessor->getTextureWidth(),
                m_frameProcessor->getTextureHeight()
            );
        }

        // 4. Render to screen
        m_renderer->renderTexture(outputTexture, windowWidth, windowHeight, ...);

        // 5. Capture frame for streaming (if streaming is active)
        if (m_streamManager && m_streamManager->isActive()) {
            // Read rendered frame from OpenGL framebuffer
            glReadPixels(0, 0, renderWidth, renderHeight, GL_RGB, GL_UNSIGNED_BYTE, frameBuffer);
            m_streamManager->pushFrame(frameBuffer, renderWidth, renderHeight);
        }
    }

    // 6. Capture audio for streaming (if streaming is active)
    if (m_audioCapture && m_streamManager && m_streamManager->isActive()) {
        int16_t audioBuffer[4096];
        size_t samples = m_audioCapture->getSamples(audioBuffer, 4096);
        if (samples > 0) {
            m_streamManager->pushAudio(audioBuffer, samples);
        }
    }

    // 7. Render UI
    m_ui->beginFrame();
    m_ui->render();
    m_ui->endFrame();

    // 8. Swap buffers
    window->swapBuffers();
}
```

### Streaming Pipeline

```
1. Main Thread (Application::run())
   ├─▶ Capture video frame (VideoCapture - V4L2/DS)
   ├─▶ Process frame (FrameProcessor: YUYV→RGB)
   ├─▶ Apply shader (ShaderEngine)
   ├─▶ Render to screen (OpenGLRenderer)
   └─▶ Capture rendered frame (glReadPixels)
       └─▶ StreamManager::pushFrame() → HTTPTSStreamer::pushFrame()
           └─▶ Add to m_timestampedVideoBuffer with timestamp

2. Audio Thread (PulseAudio callback on Linux, WASAPI thread on Windows)
   └─▶ AudioCapture callback/thread
       └─▶ StreamManager::pushAudio() → HTTPTSStreamer::pushAudio()
           └─▶ Add to m_timestampedAudioBuffer with timestamp

3. Encoding Thread (HTTPTSStreamer::encodingThread)
   ├─▶ Calculate sync zone (overlap between video and audio buffers)
   ├─▶ For each synchronized frame:
   │   ├─▶ encodeVideoFrame()
   │   │   ├─▶ Convert RGB → YUV420p (SwsContext)
   │   │   ├─▶ Resize to stream resolution
   │   │   ├─▶ Encode with FFmpeg codec
   │   │   └─▶ Receive encoded packets
   │   │
   │   └─▶ encodeAudioFrame()
   │       ├─▶ Accumulate samples
   │       ├─▶ Convert int16 → float planar (SwrContext)
   │       ├─▶ Encode with AAC codec
   │       └─▶ Receive encoded packets
   │
   └─▶ muxPacket()
       ├─▶ Set PTS/DTS (monotonic)
       ├─▶ Mux into MPEG-TS (av_interleaved_write_frame)
       └─▶ writeCallback() → writeToClients()
           └─▶ Send to all HTTP clients

4. Server Thread (HTTPTSStreamer::serverThread)
   └─▶ Accept HTTP connections
       └─▶ handleClient() (detached thread per client)
           ├─▶ Send HTTP headers
           ├─▶ Send MPEG-TS format header
           └─▶ Forward stream packets to client
```

## Directory Structure

```
src/
├── main.cpp                 # Entry point, CLI argument parsing
├── core/
│   ├── Application.h/cpp    # Main orchestrator
├── capture/
│   ├── IVideoCapture.h      # Video capture interface
│   ├── VideoCaptureFactory.h/cpp # Factory for platform-specific implementations
│   ├── VideoCaptureV4L2.h/cpp   # Linux V4L2 implementation
│   ├── VideoCaptureDS.h/cpp     # Windows DirectShow implementation
│   └── [DS helper classes]      # DirectShow helper classes
├── audio/
│   ├── IAudioCapture.h     # Audio capture interface
│   ├── AudioCaptureFactory.h/cpp # Factory for platform-specific implementations
│   ├── AudioCapturePulse.h/cpp  # Linux PulseAudio implementation
│   └── AudioCaptureWASAPI.h/cpp # Windows WASAPI implementation
├── output/
│   ├── WindowManager.h/cpp  # Window management (GLFW)
├── processing/
│   ├── FrameProcessor.h/cpp # Frame processing (YUYV→RGB, textures)
├── renderer/
│   ├── OpenGLRenderer.h/cpp # OpenGL rendering
│   └── glad_loader.h/cpp   # Dynamic OpenGL function loading
├── shader/
│   ├── ShaderEngine.h/cpp   # Shader engine
│   ├── ShaderPreset.h/cpp  # Preset parser
│   └── ShaderPreprocessor.h/cpp # Shader preprocessing
├── streaming/
│   ├── IStreamer.h          # Streaming interface
│   ├── StreamManager.h/cpp  # Stream orchestrator
│   ├── HTTPTSStreamer.h/cpp # HTTP MPEG-TS streamer (FFmpeg)
│   ├── HTTPServer.h/cpp     # HTTP/HTTPS server implementation
│   ├── WebPortal.h/cpp      # Web portal implementation
│   └── APIController.h/cpp  # REST API controller
├── ui/
│   ├── UIManager.h/cpp     # Graphical interface (ImGui)
│   └── UIConfiguration*.h/cpp # Modular UI components
├── utils/
│   ├── Logger.h/cpp        # Logging system
│   ├── ShaderScanner.h/cpp # Shader discovery
│   ├── V4L2DeviceScanner.h/cpp # V4L2 device discovery (Linux only)
│   └── FilesystemCompat.h  # Cross-platform filesystem utilities
├── v4l2/
│   └── V4L2ControlMapper.h/cpp # V4L2 control name→ID mapping (Linux only)
└── web/
    ├── index.html          # Web portal HTML
    ├── style.css           # Web portal styles
    ├── control.js          # Web portal JavaScript
    ├── api.js              # API client library
    ├── manifest.json       # PWA manifest
    └── service-worker.js   # Service worker for PWA
```

## Dependencies

### Main Libraries

**Cross-Platform:**
- **GLFW**: Window and OpenGL context management
- **OpenGL 3.3+**: Rendering and shaders
- **libpng**: Reference texture loading (LUTs)
- **ImGui**: Graphical interface
- **GLAD**: Dynamic OpenGL function loading
- **FFmpeg** (libavcodec, libavformat, libavutil, libswscale, libswresample): Video/audio encoding and MPEG-TS muxing
  - **libx264**: H.264 video encoding
  - **libx265**: H.265/HEVC video encoding
  - **libvpx**: VP8/VP9 video encoding
  - **libfdk-aac** or **libavcodec AAC**: Audio encoding
- **nlohmann/json**: Configuration persistence
- **OpenSSL** (optional): HTTPS support

**Linux-Specific:**
- **libv4l2**: V4L2 API for video capture
- **PulseAudio**: Audio capture from system
- **X11**: Window manager integration (WM_CLASS)

**Windows-Specific:**
- **DirectShow**: Video capture (via COM interfaces)
- **WASAPI**: Audio capture (Windows Audio Session API)
- **Winsock2**: Network socket support

### Build System

- **CMake 3.10+**: Build system
- **pkg-config**: Dependency detection (Linux)
- **vcpkg** or **MXE**: Dependency management for Windows cross-compilation

## Threading

The application uses a **hybrid threading model**:

### Main Thread

- Video capture (V4L2 on Linux, DirectShow on Windows)
- Frame processing (YUYV→RGB conversion)
- Shader application
- OpenGL rendering
- UI rendering (ImGui)
- Frame capture for streaming (`glReadPixels`)
- Audio capture polling

### Streaming Threads (HTTPTSStreamer)

- **Server Thread** (`serverThread`): Accepts HTTP client connections
- **Encoding Thread** (`encodingThread`): Encodes video/audio and muxes into MPEG-TS
  - Runs independently from main thread
  - Processes frames from timestamped buffers
  - Handles synchronization between video and audio
- **Client Threads** (`handleClient`): Detached threads, one per connected HTTP client
  - Forwards stream packets to individual clients
  - Automatically cleaned up when client disconnects

### Audio Thread

- **Linux (PulseAudio)**: PulseAudio callback thread for audio data
- **Windows (WASAPI)**: WASAPI capture thread for audio data
- Delivers audio samples to `AudioCapture` buffer
- Thread-safe buffer management with mutexes

### Thread Safety

- **Mutexes**: Used for buffer access (`m_videoBufferMutex`, `m_audioBufferMutex`, `m_muxMutex`, etc.)
- **Atomic Flags**: `m_running`, `m_active`, `m_stopRequest`, `m_cleanedUp` for thread coordination
- **Graceful Shutdown**: `m_stopRequest` flag signals all threads to exit cleanly
- **RAII**: Threads are properly joined in destructors

## Memory Management

- **RAII**: All resources are managed via constructors/destructors
- **Smart Pointers**: `Application` uses `std::unique_ptr` for all main components
  - Automatic cleanup on destruction
  - Exception-safe memory management
  - Clear ownership semantics
- **OpenGL Resources**: Manually managed with `glDelete*` on shutdown
- **V4L2 Buffers** (Linux): Use memory mapping (mmap) for efficiency
- **DirectShow Buffers** (Windows): Use DirectShow sample grabber for frame capture

## Utility Classes

### ShaderPreprocessor (`src/shader/ShaderPreprocessor.h/cpp`)

- Handles all shader source preprocessing
- Extracted from `ShaderEngine` to improve separation of concerns

### V4L2ControlMapper (`src/v4l2/V4L2ControlMapper.h/cpp`) - Linux only

- Maps V4L2 control names to control IDs
- Extracted from `Application` to improve modularity
- Note: Windows uses DirectShow control interfaces (`IAMCameraControl`, `IAMVideoProcAmp`)

### ShaderScanner (`src/utils/ShaderScanner.h/cpp`)

- Scans directories for shader preset files (`.glslp`)
- Extracted from `UIManager` to improve separation of concerns

### V4L2DeviceScanner (`src/utils/V4L2DeviceScanner.h/cpp`) - Linux only

- Scans for available V4L2 video capture devices
- Extracted from `UIManager` to improve separation of concerns
- Note: Windows uses DirectShow device enumeration integrated into `VideoCaptureDS`

### FrameProcessor (`src/processing/FrameProcessor.h/cpp`)

- Handles frame processing and texture management
- Extracted from `Application` to improve separation of concerns

## Extensibility

The architecture was designed to be extensible:

1. **New capture formats**: Implement `IVideoCapture` interface
   - Current implementations: V4L2 (Linux), DirectShow (Windows)
   - Future: macOS AVFoundation, etc.
2. **New shader types**: Extend `ShaderEngine` to support other formats
   - Current: RetroArch GLSL shaders
   - Future: Slang shaders, custom formats
3. **New streaming protocols**: Implement `IStreamer` interface
   - Examples: WebRTC, RTSP, UDP streaming
   - `StreamManager` automatically distributes frames to all streamers
4. **New codecs**: Extend `HTTPTSStreamer` to support additional FFmpeg codecs
   - Codec selection is configurable via UI
   - Codec-specific parameters can be added to `HTTPTSStreamer` and `UIManager`
5. **New outputs**: Add output classes (recording, file output, etc.)
6. **Plugins**: Structure allows adding processing plugins
7. **New processors**: Add processing classes similar to `FrameProcessor`
8. **New audio sources**: Implement `IAudioCapture` interface
   - Current implementations: PulseAudio (Linux), WASAPI (Windows)
   - Future: ALSA (Linux), Core Audio (macOS), etc.

## Performance Considerations

1. **Memory Mapping**: V4L2 uses mmap to avoid unnecessary copies
2. **Reusable Framebuffers**: ShaderEngine reuses framebuffers between frames
3. **Persistent Textures**: Textures are updated, not recreated
4. **VSync**: Enabled by default to avoid tearing
5. **Frame Skipping**: Application discards old frames if it can't process in real-time
6. **Separate Encoding Thread**: Video/audio encoding runs on dedicated thread, not blocking main rendering
7. **Timestamped Buffers**: Frames and audio are buffered with timestamps for synchronization
8. **Dynamic SwsContext**: RGB→YUV conversion context is recreated only when dimensions change
9. **Buffer Overflow Protection**: Limits buffer sizes to prevent memory exhaustion
10. **Desynchronization Recovery**: Aggressive frame skipping when stream falls behind
11. **Keyframe Management**: Periodic I-frames ensure decoder recovery and stream stability
12. **Low-Latency Codec Settings**: Codecs configured with `zerolatency`/`realtime` presets

## Known Limitations

1. **YUYV Format**: CPU conversion may be a bottleneck (can be optimized with shader)
2. **Shader Compilation**: Shaders are compiled at runtime (can be cached)
3. **Memory**: Intermediate framebuffers may consume a lot of memory at high resolutions
4. **Streaming Codecs**: VP8/VP9 implementation has known issues (stream may appear as "Data: bin_data" in some players)
5. **Audio Synchronization**: Complex timestamp management required for A/V sync; may drift under heavy load
6. **Frame Capture**: `glReadPixels` is synchronous and may block the main thread at high resolutions
7. **HTTP Streaming**: No authentication or access control (stream is publicly accessible)
8. **Single Stream**: Only one active streamer type at a time (though architecture supports multiple)
9. **Configuration**: Some codec-specific settings are not yet exposed in UI (e.g., VP8/VP9 advanced options)
10. **Windows DirectShow Controls**: Hardware control mapping is partially implemented
11. **Cross-Platform Testing**: Some features may behave differently across platforms
