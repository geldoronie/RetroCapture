# SOLID Principles Analysis and Refactoring Plan

This document analyzes the RetroCapture codebase for adherence to SOLID principles and identifies refactoring opportunities.

## Executive Summary

The codebase shows good separation of concerns in most areas, but there are several violations of SOLID principles that should be addressed to improve maintainability, testability, and extensibility.

## 1. Single Responsibility Principle (SRP)

### ❌ Violations

#### 1.1 `Application` Class - Too Many Responsibilities
**Location**: `src/core/Application.h`, `src/core/Application.cpp`

**Problems**:
- Manages window initialization
- Manages renderer initialization
- Manages capture initialization
- Manages UI initialization
- Handles frame processing (YUYV to RGB conversion)
- Manages texture lifecycle
- Handles keyboard input
- Coordinates all subsystems
- Contains business logic for V4L2 control mapping

**Current Responsibilities**:
1. Component lifecycle management (init/shutdown)
2. Frame capture and processing
3. Texture management
4. Event handling
5. UI callback coordination
6. V4L2 control name-to-ID mapping

**Refactoring**:
- Extract frame processing to `FrameProcessor` class
- Extract texture management to `TextureManager` class
- Extract V4L2 control mapping to `V4L2ControlMapper` class
- Keep `Application` as orchestrator only

#### 1.2 `ShaderEngine` Class - Multiple Responsibilities
**Location**: `src/shader/ShaderEngine.h`, `src/shader/ShaderEngine.cpp`

**Problems**:
- Shader compilation and linking
- Shader source preprocessing (includes, parameter extraction)
- Uniform management
- Framebuffer management
- Texture reference loading (PNG)
- History buffer management
- Shader parameter extraction and storage
- Preset loading and parsing coordination

**Current Responsibilities**:
1. Shader compilation
2. Shader preprocessing
3. Resource management (framebuffers, textures)
4. Uniform injection
5. Preset coordination
6. History buffer management

**Refactoring**:
- Extract shader preprocessing to `ShaderPreprocessor` class
- Extract uniform management to `UniformManager` class
- Extract resource management to `ShaderResourceManager` class
- Keep `ShaderEngine` as coordinator

#### 1.3 `UIManager` Class - Mixed Concerns
**Location**: `src/ui/UIManager.h`, `src/ui/UIManager.cpp`

**Problems**:
- ImGui initialization and lifecycle
- Shader scanning (file system operations)
- V4L2 device scanning (hardware access)
- UI rendering logic
- Callback management

**Current Responsibilities**:
1. UI rendering
2. File system operations (shader scanning)
3. Hardware discovery (V4L2 devices)
4. Callback coordination

**Refactoring**:
- Extract shader scanning to `ShaderScanner` class
- Extract V4L2 device discovery to `V4L2DeviceScanner` class
- Keep `UIManager` focused on UI rendering only

### ✅ Good Examples

- `VideoCapture`: Focused on V4L2 capture operations
- `WindowManager`: Focused on GLFW window management
- `OpenGLRenderer`: Focused on OpenGL rendering operations
- `ShaderPreset`: Focused on preset file parsing

## 2. Open/Closed Principle (OCP)

### ❌ Violations

#### 2.1 Hard-coded V4L2 Control Mapping
**Location**: `src/core/Application.cpp:478-492`

**Problem**:
```cpp
if (name == "Brightness") cid = V4L2_CID_BRIGHTNESS;
else if (name == "Contrast") cid = V4L2_CID_CONTRAST;
// ... more hard-coded mappings
```

**Issue**: Adding new V4L2 controls requires modifying `Application` class.

**Refactoring**:
- Create `V4L2ControlRegistry` with registration system
- Use map-based lookup instead of if-else chain
- Allow dynamic registration of controls

#### 2.2 Hard-coded Shader Uniform Names
**Location**: `src/shader/ShaderEngine.cpp` (multiple locations)

**Problem**: Uniform names are hard-coded throughout the codebase.

**Refactoring**:
- Create `UniformRegistry` with configurable uniform names
- Use strategy pattern for different shader formats

#### 2.3 Hard-coded Resolution Presets
**Location**: `src/ui/UIManager.cpp`

**Problem**: Resolution buttons are hard-coded in UI.

**Refactoring**:
- Create `ResolutionPreset` class
- Load presets from configuration file
- Allow user-defined presets

### ✅ Good Examples

- Shader loading uses strategy pattern (simple shader vs. preset)
- Callback system allows extension without modification

## 3. Liskov Substitution Principle (LSP)

### ⚠️ Not Directly Applicable

The codebase doesn't use inheritance extensively. However, if we introduce interfaces, we should ensure substitutability.

**Recommendation**: When introducing interfaces (see DIP), ensure derived classes can be substituted without breaking functionality.

## 4. Interface Segregation Principle (ISP)

### ❌ Violations

#### 4.1 `UIManager` - Fat Interface
**Location**: `src/ui/UIManager.h`

**Problem**: `UIManager` exposes many setters and callbacks that may not all be needed by clients.

**Current Interface**:
- 20+ setter methods
- 10+ callback setters
- Mixed concerns (shaders, V4L2, capture info, etc.)

**Refactoring**:
- Split into multiple interfaces:
  - `IShaderUI`
  - `IV4L2UI`
  - `ICaptureInfoUI`
  - `IImageControlsUI`
- Use composition in `UIManager`

#### 4.2 `VideoCapture` - Mixed Interface
**Location**: `src/capture/VideoCapture.h`

**Problem**: Combines low-level V4L2 operations with high-level control methods.

**Refactoring**:
- Split into:
  - `IVideoCapture` (core capture operations)
  - `IV4L2Controls` (control operations)
  - Or use composition

### ✅ Good Examples

- `ShaderEngine` has focused public interface
- `OpenGLRenderer` has minimal, focused interface

## 5. Dependency Inversion Principle (DIP)

### ❌ Violations

#### 5.1 Direct Dependency on Concrete Classes
**Location**: `src/core/Application.h:50-54`

**Problem**:
```cpp
VideoCapture* m_capture = nullptr;
WindowManager* m_window = nullptr;
OpenGLRenderer* m_renderer = nullptr;
ShaderEngine* m_shaderEngine = nullptr;
UIManager* m_ui = nullptr;
```

**Issue**: `Application` depends on concrete implementations, making testing and swapping implementations difficult.

**Refactoring**:
- Create interfaces:
  - `IVideoCapture`
  - `IWindowManager`
  - `IRenderer`
  - `IShaderEngine`
  - `IUIManager`
- Use dependency injection in constructor or init methods

#### 5.2 Manual Memory Management
**Location**: Throughout `src/core/Application.cpp`

**Problem**: Using `new`/`delete` directly.

**Issue**: 
- Memory leaks if exceptions occur
- Difficult to test (can't easily mock)
- Tight coupling to concrete types

**Refactoring**:
- Use smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- Use factory pattern for creation
- Use dependency injection

#### 5.3 Direct File System Access
**Location**: `src/shader/ShaderEngine.cpp`, `src/ui/UIManager.cpp`

**Problem**: Direct `std::ifstream` usage and filesystem operations.

**Refactoring**:
- Create `IFileSystem` interface
- Inject file system dependency
- Allows testing with mock file system

### ✅ Good Examples

- Callback system uses `std::function`, allowing dependency injection
- `WindowManager` uses callback for resize events

## Additional Issues

### 6. Code Duplication

#### 6.1 V4L2 Control Configuration
**Location**: `src/core/Application.cpp:217-280`

**Problem**: Repetitive code for each V4L2 control.

**Refactoring**:
```cpp
struct V4L2ControlConfig {
    int32_t* member;
    std::function<bool(VideoCapture*, int32_t)> setter;
    std::string name;
};

std::vector<V4L2ControlConfig> controls = {
    {&m_v4l2Brightness, [](VideoCapture* c, int32_t v) { return c->setBrightness(v); }, "Brightness"},
    // ...
};

for (const auto& config : controls) {
    if (*config.member >= 0) {
        if (config.setter(m_capture, *config.member)) {
            LOG_INFO(config.name + " V4L2 configurado: " + std::to_string(*config.member));
        }
    }
}
```

#### 6.2 Shader Extension Checking
**Location**: Multiple locations in `ShaderEngine.cpp`

**Problem**: Same extension checking logic repeated.

**Refactoring**: Extract to helper function.

### 7. Error Handling

#### 7.1 Inconsistent Error Handling
**Location**: Throughout codebase

**Problem**: Mix of return codes, exceptions, and logging.

**Refactoring**:
- Standardize error handling approach
- Consider `std::expected` (C++23) or similar
- Create error types hierarchy

### 8. Configuration Management

#### 8.1 Scattered Configuration
**Location**: `src/core/Application.h:61-85`

**Problem**: Configuration stored as individual member variables.

**Refactoring**:
- Create `ApplicationConfig` struct
- Create `V4L2Config` struct
- Use builder pattern for configuration

## Refactoring Priority

### High Priority (Core Architecture)

1. **Extract interfaces and use dependency injection** (DIP violation)
   - Impact: High (enables testing, flexibility)
   - Effort: Medium
   - Dependencies: None

2. **Replace manual memory management with smart pointers**
   - Impact: High (prevents leaks, improves safety)
   - Effort: Low
   - Dependencies: None

3. **Extract frame processing from Application** (SRP violation)
   - Impact: Medium (improves testability)
   - Effort: Low
   - Dependencies: None

### Medium Priority (Code Quality)

4. **Extract V4L2 control mapping** (SRP, OCP violations)
   - Impact: Medium (improves extensibility)
   - Effort: Low
   - Dependencies: None

5. **Extract shader preprocessing** (SRP violation)
   - Impact: Medium (improves maintainability)
   - Effort: Medium
   - Dependencies: None

6. **Split UIManager interface** (ISP violation)
   - Impact: Medium (improves flexibility)
   - Effort: Medium
   - Dependencies: None

### Low Priority (Nice to Have)

7. **Configuration management refactoring**
   - Impact: Low (improves organization)
   - Effort: Low
   - Dependencies: None

8. **Standardize error handling**
   - Impact: Low (improves consistency)
   - Effort: Medium
   - Dependencies: None

## Implementation Strategy

### Phase 1: Foundation (Week 1)
1. Create interfaces for core components
2. Replace `new`/`delete` with smart pointers
3. Implement dependency injection in `Application`

### Phase 2: Extraction (Week 2)
1. Extract `FrameProcessor` from `Application`
2. Extract `V4L2ControlMapper` from `Application`
3. Extract `ShaderPreprocessor` from `ShaderEngine`

### Phase 3: Refinement (Week 3)
1. Split `UIManager` interfaces
2. Create configuration management
3. Standardize error handling

## Testing Strategy

After refactoring, we should have:
- Unit tests for each extracted component
- Integration tests for the full pipeline
- Mock implementations for testing

The dependency injection changes will make testing significantly easier.

## Conclusion

The codebase is generally well-structured, but there are clear opportunities to improve adherence to SOLID principles. The most critical improvements are:

1. **Dependency Inversion**: Introduce interfaces and dependency injection
2. **Single Responsibility**: Extract specialized classes for specific tasks
3. **Open/Closed**: Replace hard-coded mappings with configurable systems

These changes will improve:
- **Testability**: Easier to unit test components
- **Maintainability**: Clearer responsibilities
- **Extensibility**: Easier to add new features
- **Flexibility**: Easier to swap implementations

