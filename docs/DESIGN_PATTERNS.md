# Design Patterns in RetroCapture

This document describes the design patterns used in RetroCapture and how they are applied to guide contributors.

## 1. Facade

### Application: `Application` Class

The `Application` acts as a facade that simplifies the complex interface of multiple subsystems (capture, rendering, shaders, UI) into a unified and simple interface.

**Example**:
```cpp
class Application {
    std::unique_ptr<VideoCapture> m_capture;
    std::unique_ptr<WindowManager> m_window;
    std::unique_ptr<OpenGLRenderer> m_renderer;
    std::unique_ptr<FrameProcessor> m_frameProcessor;
    std::unique_ptr<ShaderEngine> m_shaderEngine;
    std::unique_ptr<UIManager> m_ui;
    
public:
    bool init();  // Initializes all subsystems
    void run();   // Orchestrates main loop
    void shutdown(); // Cleans up all resources (automatic via unique_ptr)
};
```

**Benefits**:
- Simple interface for client code (`main.cpp`)
- Encapsulates initialization and coordination complexity
- Facilitates testing and maintenance

**When to use**:
- When you need to simplify a complex interface
- To coordinate multiple subsystems

## 2. Strategy

### Application: Shader Processing Pipeline

The `ShaderEngine` allows different processing strategies (simple shader vs. multi-pass preset) without changing client code.

**Example**:
```cpp
class ShaderEngine {
    bool loadShader(const std::string& shaderPath);      // Strategy 1: Simple shader
    bool loadPreset(const std::string& presetPath);      // Strategy 2: Multi-pass preset
    GLuint applyShader(GLuint inputTexture, ...);        // Unified interface
};
```

**Benefits**:
- Flexibility to choose algorithm at runtime
- Easy to add new strategies (e.g., Slang support)
- Client code doesn't need to know details

**When to use**:
- When you have multiple ways to do the same thing
- To allow algorithm choice at runtime

## 3. Builder

### Application: Component Configuration

The `Application` setters allow building configuration before initialization.

**Example**:
```cpp
Application app;
app.setDevicePath("/dev/video0");
app.setResolution(1920, 1080);
app.setFramerate(60);
app.setPresetPath("shaders/crt.glslp");
app.setBrightness(1.2f);
app.setContrast(1.1f);
// ... more configuration
app.init();  // Initialize with all configurations
```

**Benefits**:
- Flexible and readable configuration
- Centralized validation in `init()`
- Facilitates creating different configurations

**When to use**:
- When you have many optional parameters
- To build complex objects step by step

## 4. Observer

### Application: Callbacks in `UIManager` and `WindowManager`

The callback system allows components to react to events without direct coupling.

**Example**:
```cpp
// UIManager allows registering callbacks
m_ui->setOnShaderChanged([this](const std::string& shader) {
    m_shaderEngine->loadPreset(shader);
});

m_ui->setOnBrightnessChanged([this](float brightness) {
    m_brightness = brightness;
});

// WindowManager notifies size changes
m_window->setResizeCallback([this](int width, int height) {
    m_shaderEngine->setViewport(width, height);
});
```

**Benefits**:
- Low coupling between components
- Easy to add new observers
- UI can notify Application without knowing details

**When to use**:
- When you need to notify multiple objects about events
- To decouple the object that generates events from those that react

## 5. Factory

### Application: OpenGL Resource Creation

The `OpenGLRenderer` and `ShaderEngine` act as factories to create OpenGL resources (textures, framebuffers, shaders).

**Example**:
```cpp
// OpenGLRenderer creates textures
GLuint texture = m_renderer->createTextureFromFrame(frame.data, width, height, format);

// ShaderEngine creates shader programs
bool success = m_shaderEngine->loadPreset("shader.glslp");
```

**Benefits**:
- Encapsulates complex creation logic
- Centralizes validation and error handling
- Facilitates implementation changes

**When to use**:
- When object creation is complex
- To centralize creation logic
- To hide implementation details

## 6. Singleton

### Application: Logger

The logging system uses an implicit singleton pattern through global macros.

**Example**:
```cpp
// Logger.h
#define LOG_INFO(msg) Logger::getInstance().info(msg)
#define LOG_ERROR(msg) Logger::getInstance().error(msg)

// Use anywhere
LOG_INFO("Application initialized");
LOG_ERROR("Failed to load shader");
```

**Benefits**:
- Easy global access
- Single instance guaranteed
- Convenient for logging

**When to use**:
- For resources that must have a single instance
- When global access is necessary (like logging)

**Cautions**:
- Can make testing difficult
- Can create hidden dependencies

## 7. Template Method

### Application: Processing Pipeline

The `Application::run()` defines the algorithm skeleton, but delegates specific steps to virtual/private methods.

**Example**:
```cpp
void Application::run() {
    while (!m_window->shouldClose()) {
        // Template: fixed structure
        m_window->pollEvents();
        handleKeyInput();
        
        if (processFrame()) {  // Specific method
            // Process frame
        }
        
        m_ui->beginFrame();
        m_ui->render();
        m_ui->endFrame();
        
        m_window->swapBuffers();
    }
}
```

**Benefits**:
- Defines common structure
- Allows variation in specific steps
- Facilitates maintenance

**When to use**:
- When you have an algorithm with fixed structure but variable steps
- To avoid code duplication

## 8. Adapter

### Application: Format Conversion

The `OpenGLRenderer` adapts V4L2 formats to OpenGL formats.

**Example**:
```cpp
class OpenGLRenderer {
    GLenum getGLFormat(uint32_t v4l2Format);  // Adapts V4L2 → OpenGL
    GLenum getGLInternalFormat(uint32_t v4l2Format);
    
    void updateTexture(GLuint texture, const uint8_t* data, 
                       uint32_t width, uint32_t height, 
                       uint32_t format);  // Accepts V4L2 format, converts internally
};
```

**Benefits**:
- Allows incompatible components to work together
- Encapsulates conversion logic
- Facilitates implementation changes

**When to use**:
- When you need to make incompatible interfaces work together
- To encapsulate complex conversions

## 9. Command

### Application: Callbacks as Commands

Callbacks in `UIManager` encapsulate actions as commands that can be executed.

**Example**:
```cpp
// Command encapsulated as function
m_ui->setOnShaderChanged([this](const std::string& shader) {
    // Command: load shader
    m_shaderEngine->loadPreset(shader);
});

// Can be executed by UI when needed
// Allows undo/redo, logging, etc. in the future
```

**Benefits**:
- Encapsulates requests as objects
- Allows queue, undo/redo, logging
- Decouples object that invokes from operation

**When to use**:
- When you need to parameterize objects with operations
- To support undo/redo
- For command queue

## 10. State

### Application: Initialization States

Components have clear states (not initialized, initialized, error).

**Example**:
```cpp
class Application {
    bool m_initialized = false;  // State
    
    bool init() {
        if (m_initialized) return true;  // Already initialized
        // ... initialization
        m_initialized = true;
        return true;
    }
    
    void shutdown() {
        if (!m_initialized) return;  // Not initialized
        // ... cleanup
        m_initialized = false;
    }
};
```

**Benefits**:
- Behavior varies with state
- Clear state transitions
- Facilitates debugging

**When to use**:
- When behavior depends on state
- To manage state transitions

## Applied Design Principles

### SOLID Principles

1. **Single Responsibility**: Each class has a clear responsibility
   - `VideoCapture`: Only video capture
   - `ShaderEngine`: Only shader processing
   - `OpenGLRenderer`: Only OpenGL rendering

2. **Open/Closed**: Extensible without modifying existing code
   - New capture formats can be added
   - New shader types can be supported

3. **Liskov Substitution**: (Not directly applicable, but prepared for inheritance)

4. **Interface Segregation**: Specific and focused interfaces
   - `VideoCapture` doesn't expose OpenGL details
   - `ShaderEngine` doesn't know about V4L2

5. **Dependency Inversion**: Dependencies on abstractions
   - `Application` depends on interfaces, not implementations
   - Callbacks allow dependency injection

### DRY (Don't Repeat Yourself)

- YUYV→RGB conversion centralized in `Application::convertYUYVtoRGB()`
- OpenGL resource creation centralized in specific classes
- Shader parsing centralized in `ShaderPreset`

### KISS (Keep It Simple, Stupid)

- Simple and straightforward structure
- No over-engineering
- Readable and maintainable code

### YAGNI (You Aren't Gonna Need It)

- We don't implement unnecessary features
- We focus on what's needed now
- Extensibility when necessary

## Guide for Contributors

### When Adding New Functionality

1. **Identify the responsible component**
   - Capture? → `VideoCapture`
   - Rendering? → `OpenGLRenderer`
   - Shaders? → `ShaderEngine`
   - UI? → `UIManager`

2. **Follow existing patterns**
   - Use the same design patterns
   - Maintain consistency with existing code
   - Follow naming conventions

3. **Keep responsibilities separated**
   - Don't mix logic from different domains
   - Use callbacks for communication between components

4. **Document design decisions**
   - Comment why you chose a pattern
   - Document trade-offs

### Example: Adding New Output Type

```cpp
// 1. Create new class following existing pattern
class VideoRecorder {
public:
    bool init();
    void shutdown();
    bool recordFrame(GLuint texture, uint32_t width, uint32_t height);
};

// 2. Integrate into Application using Facade pattern
class Application {
    VideoRecorder* m_recorder = nullptr;
    
    bool initRecorder() {
        m_recorder = new VideoRecorder();
        return m_recorder->init();
    }
};

// 3. Use callbacks for control (Observer pattern)
m_ui->setOnRecordChanged([this](bool recording) {
    if (recording) {
        m_recorder->start();
    } else {
        m_recorder->stop();
    }
});
```

### Pull Request Checklist

- [ ] Follows existing design patterns
- [ ] Maintains separated responsibilities
- [ ] Doesn't break existing interfaces
- [ ] Documents important decisions
- [ ] Tests in different scenarios
- [ ] Maintains code consistency

## References

- **Design Patterns: Elements of Reusable Object-Oriented Software** (Gang of Four)
- **Clean Code** (Robert C. Martin)
- **SOLID Principles** (Robert C. Martin)
