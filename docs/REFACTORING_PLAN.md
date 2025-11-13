# Refactoring Plan - SOLID Principles

## Quick Reference: Main Violations

### 🔴 Critical (High Priority)

1. **Manual Memory Management** (`Application.cpp`)
   - **Issue**: Using `new`/`delete` directly
   - **Risk**: Memory leaks, exception safety
   - **Fix**: Replace with `std::unique_ptr`
   - **Effort**: Low
   - **Impact**: High

2. **Application Class - Too Many Responsibilities**
   - **Issue**: Handles capture, rendering, UI, frame processing, texture management
   - **Risk**: Hard to test, maintain, and extend
   - **Fix**: Extract `FrameProcessor`, `TextureManager`, `V4L2ControlMapper`
   - **Effort**: Medium
   - **Impact**: High

3. **Direct Dependency on Concrete Classes**
   - **Issue**: `Application` depends on concrete types
   - **Risk**: Hard to test, can't swap implementations
   - **Fix**: Create interfaces, use dependency injection
   - **Effort**: Medium
   - **Impact**: High

### 🟡 Important (Medium Priority)

4. **Hard-coded V4L2 Control Mapping** (`Application.cpp:478-492`)
   - **Issue**: If-else chain for control name to ID mapping
   - **Risk**: Hard to extend, violates OCP
   - **Fix**: Create `V4L2ControlMapper` with registry
   - **Effort**: Low
   - **Impact**: Medium

5. **ShaderEngine - Multiple Responsibilities**
   - **Issue**: Compilation, preprocessing, resource management, uniform injection
   - **Risk**: Hard to maintain and test
   - **Fix**: Extract `ShaderPreprocessor`, `UniformManager`, `ShaderResourceManager`
   - **Effort**: Medium
   - **Impact**: Medium

6. **UIManager - Mixed Concerns**
   - **Issue**: UI rendering + file system + hardware discovery
   - **Risk**: Hard to test UI in isolation
   - **Fix**: Extract `ShaderScanner`, `V4L2DeviceScanner`
   - **Effort**: Low
   - **Impact**: Medium

### 🟢 Nice to Have (Low Priority)

7. **Scattered Configuration**
   - **Issue**: 20+ individual member variables for config
   - **Risk**: Hard to manage, validate, serialize
   - **Fix**: Create `ApplicationConfig`, `V4L2Config` structs
   - **Effort**: Low
   - **Impact**: Low

8. **Inconsistent Error Handling**
   - **Issue**: Mix of return codes, logging, no exceptions
   - **Risk**: Hard to handle errors consistently
   - **Fix**: Standardize error handling approach
   - **Effort**: Medium
   - **Impact**: Low

## Recommended Implementation Order

### Phase 1: Safety First (Week 1)
**Goal**: Prevent memory leaks and improve exception safety

1. ✅ Replace `new`/`delete` with `std::unique_ptr`
   - Files: `Application.h`, `Application.cpp`
   - Risk: Low (compiler will catch issues)
   - Testing: Compile and run

### Phase 2: Extract and Isolate (Week 2)
**Goal**: Improve testability and maintainability

2. ✅ Extract `V4L2ControlMapper` from `Application`
   - Files: New `src/v4l2/V4L2ControlMapper.h/cpp`
   - Risk: Low (isolated functionality)
   - Testing: Unit tests for mapping

3. ✅ Extract `FrameProcessor` from `Application`
   - Files: New `src/processing/FrameProcessor.h/cpp`
   - Risk: Medium (core functionality)
   - Testing: Unit tests for YUYV conversion

4. ✅ Extract `ShaderScanner` and `V4L2DeviceScanner` from `UIManager`
   - Files: New `src/utils/ShaderScanner.h/cpp`, `src/utils/V4L2DeviceScanner.h/cpp`
   - Risk: Low (isolated functionality)
   - Testing: Unit tests for scanning

### Phase 3: Interfaces and Dependency Injection (Week 3)
**Goal**: Enable testing and flexibility

5. ✅ Create interfaces for core components
   - Files: New `src/interfaces/` directory
   - Risk: Medium (touches many files)
   - Testing: Integration tests

6. ✅ Implement dependency injection in `Application`
   - Files: `Application.h`, `Application.cpp`
   - Risk: Medium (core architecture change)
   - Testing: Integration tests

### Phase 4: Configuration Management (Week 4)
**Goal**: Improve configuration handling

7. ✅ Create configuration structs
   - Files: New `src/config/ApplicationConfig.h`
   - Risk: Low (additive change)
   - Testing: Unit tests

8. ✅ Extract `ShaderPreprocessor` from `ShaderEngine`
   - Files: New `src/shader/ShaderPreprocessor.h/cpp`
   - Risk: Medium (complex logic)
   - Testing: Unit tests for preprocessing

## Detailed Refactoring Steps

### Step 1: Smart Pointers Migration

**Files to modify**:
- `src/core/Application.h`
- `src/core/Application.cpp`

**Changes**:
```cpp
// Before
VideoCapture* m_capture = nullptr;

// After
#include <memory>
std::unique_ptr<VideoCapture> m_capture;
```

**Benefits**:
- Automatic memory management
- Exception safety
- Clear ownership semantics

**Testing**:
- Compile and run
- Verify no memory leaks with valgrind

### Step 2: V4L2ControlMapper Extraction

**New files**:
- `src/v4l2/V4L2ControlMapper.h`
- `src/v4l2/V4L2ControlMapper.cpp`

**Interface**:
```cpp
class V4L2ControlMapper {
public:
    static uint32_t nameToControlId(const std::string& name);
    static std::string controlIdToName(uint32_t cid);
    static std::vector<std::string> getAvailableControls();
};
```

**Benefits**:
- Single responsibility
- Easy to extend
- Testable in isolation

### Step 3: FrameProcessor Extraction

**New files**:
- `src/processing/FrameProcessor.h`
- `src/processing/FrameProcessor.cpp`

**Interface**:
```cpp
class FrameProcessor {
public:
    bool processFrame(const Frame& input, GLuint& outputTexture, 
                      uint32_t& width, uint32_t& height);
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, 
                          uint32_t width, uint32_t height);
};
```

**Benefits**:
- Isolated frame processing logic
- Reusable
- Testable

### Step 4: Configuration Structs

**New files**:
- `src/config/ApplicationConfig.h`

**Structure**:
```cpp
struct V4L2Config {
    int32_t brightness = -1;
    int32_t contrast = -1;
    // ... other controls
};

struct ApplicationConfig {
    std::string devicePath = "/dev/video0";
    uint32_t captureWidth = 1920;
    uint32_t captureHeight = 1080;
    uint32_t captureFps = 60;
    uint32_t windowWidth = 1920;
    uint32_t windowHeight = 1080;
    bool fullscreen = false;
    int monitorIndex = -1;
    bool maintainAspect = false;
    float brightness = 1.0f;
    float contrast = 1.0f;
    V4L2Config v4l2;
};
```

**Benefits**:
- Centralized configuration
- Easy to serialize/deserialize
- Validation in one place

## Testing Strategy

After each refactoring:

1. **Compile**: Ensure no compilation errors
2. **Run**: Basic functionality test
3. **Valgrind**: Check for memory leaks (after smart pointers)
4. **Unit Tests**: Test extracted components (after extraction)
5. **Integration Tests**: Test full pipeline (after interfaces)

## Rollback Plan

Each refactoring should be:
- Committed separately
- Tested before moving to next
- Documented with commit message

If issues arise:
- Revert the specific commit
- Document the issue
- Re-plan the approach

## Success Criteria

After refactoring:
- ✅ No memory leaks (valgrind clean)
- ✅ All existing functionality works
- ✅ Code is more testable (can unit test components)
- ✅ Code is more maintainable (clear responsibilities)
- ✅ Code is more extensible (easy to add features)

## Notes

- Refactor incrementally
- Test after each change
- Keep commits small and focused
- Document breaking changes
- Update architecture docs

