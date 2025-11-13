# RetroCapture Architecture

## Overview

RetroCapture is a real-time video capture application for Linux that applies RetroArch shaders (GLSL) to video feeds from V4L2 capture cards. The architecture was designed to be modular, efficient, and low-latency.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                              │
│                    (Main Orchestrator)                           │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ VideoCapture │    │ WindowManager│    │ ShaderEngine │
│   (V4L2)     │    │   (GLFW)     │    │  (OpenGL)    │
└──────────────┘    └──────────────┘    └──────────────┘
        │                     │                     │
        │                     ▼                     │
        │            ┌──────────────┐                │
        │            │ OpenGLRenderer│               │
        │            │   (OpenGL)    │               │
        │            └──────────────┘                │
        │                     │                      │
        └─────────────────────┼──────────────────────┘
                              │
                              ▼
                    ┌──────────────┐
                    │   UIManager   │
                    │   (ImGui)     │
                    └──────────────┘
```

## Main Components

### 1. Application (`src/core/Application.h/cpp`)

**Responsibility**: Orchestration of all components and application lifecycle management.

**Main Features**:
- Initialization and shutdown of all components
- Main rendering loop
- Video frame processing
- Format conversion (YUYV → RGB)
- Coordination between capture, shaders, and rendering

**Dependencies**:
- `VideoCapture`: Frame capture
- `WindowManager`: Window management
- `OpenGLRenderer`: OpenGL rendering
- `ShaderEngine`: Shader processing
- `UIManager`: Graphical interface

**Data Flow**:
```
VideoCapture → Frame (YUYV) → RGB Conversion → OpenGL Texture → ShaderEngine → Rendering
```

### 2. VideoCapture (`src/capture/VideoCapture.h/cpp`)

**Responsibility**: Video capture via V4L2 (Video4Linux2).

**Main Features**:
- Opening/closing V4L2 devices
- Format configuration (resolution, pixel format, framerate)
- V4L2 controls (brightness, contrast, saturation, etc.)
- Frame capture using memory mapping (mmap)
- Circular buffer for low latency

**Main API**:
```cpp
bool open(const std::string& device);
bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat);
bool setFramerate(uint32_t fps);
bool captureFrame(Frame& frame);
bool setControl(uint32_t controlId, int32_t value);
```

**Frame Format**:
```cpp
struct Frame {
    uint8_t* data;      // Frame data
    size_t size;        // Size in bytes
    uint32_t width;     // Width
    uint32_t height;    // Height
    uint32_t format;    // V4L2 format (e.g., V4L2_PIX_FMT_YUYV)
};
```

### 3. WindowManager (`src/output/WindowManager.h/cpp`)

**Responsibility**: Window and OpenGL context management via GLFW.

**Main Features**:
- Window creation/destruction
- OpenGL context management
- Fullscreen and multi-monitor support
- Event callbacks (resize, etc.)
- VSync

**Main API**:
```cpp
bool init(const WindowConfig& config);
void setFullscreen(bool fullscreen, int monitorIndex = -1);
void setResizeCallback(std::function<void(int, int)> callback);
void swapBuffers();
void pollEvents();
```

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

### 5. ShaderEngine (`src/shader/ShaderEngine.h/cpp`)

**Responsibility**: Loading, compilation, and application of RetroArch shaders.

**Main Features**:
- Loading RetroArch presets (`.glslp`)
- Parsing GLSL shaders with support for `#if defined(VERTEX)` / `#elif defined(FRAGMENT)`
- Vertex and fragment shader compilation
- Multiple pass management
- RetroArch uniform injection (`OutputSize`, `InputSize`, `FrameCount`, etc.)
- Intermediate framebuffer management
- Reference texture loading (LUTs, etc.)
- Support for configurable parameters (`#pragma parameter`)

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

### 6. ShaderPreset (`src/shader/ShaderPreset.h/cpp`)

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

### 7. UIManager (`src/ui/UIManager.h/cpp`)

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
- **Info**: Capture and application information

## Data Flow

### Rendering Pipeline

```
1. VideoCapture::captureFrame()
   └─▶ Frame (YUYV) from V4L2 device

2. Application::processFrame()
   └─▶ YUYV → RGB conversion
       └─▶ OpenGLRenderer::updateTexture()
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
    
    // 2. Capture frame
    if (processFrame()) {
        // 3. Apply shader (if any)
        GLuint outputTexture = m_texture;
        if (m_shaderEngine && m_shaderEngine->isShaderActive()) {
            outputTexture = m_shaderEngine->applyShader(m_texture, m_textureWidth, m_textureHeight);
        }
        
        // 4. Render to screen
        m_renderer->renderTexture(outputTexture, windowWidth, windowHeight, ...);
    }
    
    // 5. Render UI
    m_ui->beginFrame();
    m_ui->render();
    m_ui->endFrame();
    
    // 6. Swap buffers
    window->swapBuffers();
}
```

## Directory Structure

```
src/
├── main.cpp                 # Entry point, CLI argument parsing
├── core/
│   ├── Application.h/cpp    # Main orchestrator
├── capture/
│   ├── VideoCapture.h/cpp   # V4L2 capture
├── output/
│   ├── WindowManager.h/cpp  # Window management (GLFW)
├── renderer/
│   ├── OpenGLRenderer.h/cpp # OpenGL rendering
│   └── glad_loader.h/cpp   # Dynamic OpenGL function loading
├── shader/
│   ├── ShaderEngine.h/cpp   # Shader engine
│   └── ShaderPreset.h/cpp  # Preset parser
├── ui/
│   ├── UIManager.h/cpp     # Graphical interface (ImGui)
└── utils/
    └── Logger.h/cpp        # Logging system
```

## Dependencies

### Main Libraries
- **GLFW**: Window and OpenGL context management
- **OpenGL 3.3+**: Rendering and shaders
- **libv4l2**: V4L2 API for video capture
- **libpng**: Reference texture loading (LUTs)
- **ImGui**: Graphical interface
- **GLAD**: Dynamic OpenGL function loading

### Build System
- **CMake 3.10+**: Build system
- **pkg-config**: Dependency detection

## Threading

Currently, the application is **single-threaded**. Frame processing, shaders, and rendering happen on the same main thread. This is adequate for most cases, but can be optimized in the future with:

- Separate thread for video capture
- Thread for shader processing
- Thread for rendering

## Memory Management

- **RAII**: All resources are managed via constructors/destructors
- **Raw pointers**: Used for main components (can be converted to `std::unique_ptr` in the future)
- **OpenGL Resources**: Manually managed with `glDelete*` on shutdown
- **V4L2 Buffers**: Use memory mapping (mmap) for efficiency

## Extensibility

The architecture was designed to be extensible:

1. **New capture formats**: Implement interface similar to `VideoCapture`
2. **New shader types**: Extend `ShaderEngine` to support other formats
3. **New outputs**: Add output classes (recording, streaming, etc.)
4. **Plugins**: Structure allows adding processing plugins

## Performance Considerations

1. **Memory Mapping**: V4L2 uses mmap to avoid unnecessary copies
2. **Reusable Framebuffers**: ShaderEngine reuses framebuffers between frames
3. **Persistent Textures**: Textures are updated, not recreated
4. **VSync**: Enabled by default to avoid tearing
5. **Frame Skipping**: Application discards old frames if it can't process in real-time

## Known Limitations

1. **Single-threaded**: May limit performance on multi-core systems
2. **YUYV Format**: CPU conversion may be a bottleneck (can be optimized with shader)
3. **Shader Compilation**: Shaders are compiled at runtime (can be cached)
4. **Memory**: Intermediate framebuffers may consume a lot of memory at high resolutions
