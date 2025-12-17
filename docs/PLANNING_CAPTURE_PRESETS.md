# Planning: Capture Presets Feature

## 📋 Overview

Implement a **Capture Presets** system that allows saving and loading complete RetroCapture configurations, including shader, parameters, resolution, FPS and other settings, with visual thumbnails for quick identification.

## 🎯 Objectives

1. **Save complete configurations** in reusable presets
2. **Visual interface** with preset grid and thumbnails
3. **Real-time application** when clicking on a preset
4. **Quick creation** of presets from current state
5. **Automatic capture** of viewport thumbnails

## 🏗️ Architecture

### Main Components

```
┌─────────────────────────────────────────────────────────┐
│              UICapturePresets                           │
│  (New window to manage presets)                         │
└─────────────────────────────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│ PresetManager│ │ThumbnailGen  │ │ PresetLoader │
│ (IO/Storage) │ │ (glReadPixels│ │  (Apply)     │
└──────────────┘ └──────────────┘ └──────────────┘
        │               │               │
        └───────────────┼───────────────┘
                        │
                        ▼
              assets/presets/
              assets/thumbnails/
```

### Data Structure

#### Preset JSON (`assets/presets/{name}.json`)

```json
{
  "version": "1.0",
  "name": "CRT Classic",
  "description": "CRT shader with classic configuration",
  "created": "2024-01-15T10:30:00Z",
  "thumbnail": "assets/thumbnails/crt_classic.png",

  "shader": {
    "path": "shaders/crt/crt-royale.glslp",
    "parameters": {
      "BRIGHTNESS": 1.2,
      "CONTRAST": 1.1,
      "SATURATION": 1.0,
      "GAMMA": 2.4
    }
  },

  "capture": {
    "width": 1920,
    "height": 1080,
    "fps": 60,
    "device": "/dev/video0",
    "sourceType": 1
  },

  "image": {
    "brightness": 1.0,
    "contrast": 1.0,
    "maintainAspect": true,
    "fullscreen": false,
    "monitorIndex": 0
  },

  "streaming": {
    "width": 1280,
    "height": 720,
    "fps": 30,
    "bitrate": 5000000,
    "audioBitrate": 128000,
    "videoCodec": "libx264",
    "audioCodec": "aac",
    "h264Preset": "veryfast"
  },

  "v4l2Controls": {
    "Brightness": 128,
    "Contrast": 128,
    "Saturation": 128,
    "Hue": 0
  }
}
```

## 📁 File Structure

```
RetroCapture/
├── assets/
│   ├── presets/              # Saved presets
│   │   ├── crt_classic.json
│   │   ├── gameboy.json
│   │   └── scanlines.json
│   └── thumbnails/           # Preset thumbnails
│       ├── crt_classic.png
│       ├── gameboy.png
│       └── scanlines.png
└── src/
    ├── ui/
    │   ├── UICapturePresets.h/cpp        # New UI window
    │   └── UIManager.h/cpp                # Integration
    └── utils/
        ├── PresetManager.h/cpp            # Preset management
        └── ThumbnailGenerator.h/cpp       # Thumbnail generation
```

## 🔧 Implementation

### 1. PresetManager (`src/utils/PresetManager.h/cpp`)

**Responsibility**: Manage preset I/O (save, load, list, delete).

**API Principal**:

```cpp
class PresetManager {
public:
    struct PresetData {
        std::string name;
        std::string description;
        std::string thumbnailPath;
        std::string shaderPath;
        std::map<std::string, float> shaderParameters;
        uint32_t captureWidth, captureHeight, captureFps;
        std::string devicePath;
        int sourceType;
        float imageBrightness, imageContrast;
        bool maintainAspect;
        bool fullscreen;
        int monitorIndex;
        // ... streaming settings
        // ... v4l2 controls
    };

    bool savePreset(const std::string& name, const PresetData& data);
    bool loadPreset(const std::string& name, PresetData& data);
    bool deletePreset(const std::string& name);
    std::vector<std::string> listPresets();
    std::string getPresetsDirectory() const;
    std::string getThumbnailsDirectory() const;
};
```

**Features**:

- Create `assets/presets/` and `assets/thumbnails/` directories if they don't exist
- Validate JSON format
- Sanitize file names (remove invalid characters)
- Format versioning (for future migration)

### 2. ThumbnailGenerator (`src/utils/ThumbnailGenerator.h/cpp`)

**Responsibility**: Capture frame from viewport and save as PNG.

**Main API**:

```cpp
class ThumbnailGenerator {
public:
    // Capture current frame from framebuffer and save as PNG
    bool captureAndSaveThumbnail(
        const std::string& outputPath,
        uint32_t width = 320,
        uint32_t height = 240
    );

    // Capture frame from a specific texture
    bool captureTextureAsThumbnail(
        GLuint texture,
        uint32_t textureWidth,
        uint32_t textureHeight,
        const std::string& outputPath,
        uint32_t thumbnailWidth = 320,
        uint32_t thumbnailHeight = 240
    );
};
```

**Implementation**:

- Use `glReadPixels` to capture framebuffer (similar to streaming)
- Resize to thumbnail size (320x240 default)
- Convert RGB → PNG using `libpng` (already used in project)
- Save to `assets/thumbnails/{preset_name}.png`

**Note**: Capture must be done **after** shader is applied, to show final result.

### 3. UICapturePresets (`src/ui/UICapturePresets.h/cpp`)

**Responsibility**: Graphical interface window to manage presets.

**UI Structure**:

```
┌─────────────────────────────────────────────────────┐
│  Capture Presets                                    │
├─────────────────────────────────────────────────────┤
│                                                     │
│  [Create New Preset]                                │
│                                                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ [IMG]    │ │ [IMG]    │ │ [IMG]    │            │
│  │ CRT      │ │ Game Boy │ │ Scanlines│            │
│  │ Classic  │ │          │ │          │            │
│  └──────────┘ └──────────┘ └──────────┘            │
│                                                     │
│  ┌──────────┐ ┌──────────┐                        │
│  │ [IMG]    │ │ [IMG]    │                        │
│  │ Retro    │ │ Modern   │                        │
│  │          │ │          │                        │
│  └──────────┘ └──────────┘                        │
│                                                     │
└─────────────────────────────────────────────────────┘
```

**UI Features**:

- **Preset grid**: Display thumbnails in grid (3-4 columns)
- **Preset click**: Apply preset immediately
- **"Create" button**: Open dialog to create new preset
- **Context menu** (right-click):
  - Rename
  - Delete
  - Duplicate
  - Export/Import
- **Search/Filter**: Text field to filter presets
- **Sorting**: By name, creation date, etc.

**Creation Dialog**:

```
┌─────────────────────────────────────┐
│  Create New Preset                  │
├─────────────────────────────────────┤
│  Name: [________________]           │
│  Description: [_____________]       │
│                                     │
│  Capture thumbnail from current     │
│  viewport? [✓] Yes                  │
│                                     │
│  [Cancel]  [Create]                 │
└─────────────────────────────────────┘
```

**ImGui Integration**:

- Use `ImGui::ImageButton` for thumbnails
- Use `ImGui::BeginChild` with scroll for grid
- Use `ImGui::InputText` for name/description
- Use `ImGui::OpenPopup` for dialogs

### 4. PresetLoader (function in Application or UIManager)

**Responsibility**: Apply a loaded preset to the system.

**Application Flow**:

```cpp
void Application::applyPreset(const PresetManager::PresetData& preset) {
    // 1. Apply shader
    if (!preset.shaderPath.empty()) {
        m_shaderEngine->loadPreset(preset.shaderPath);

        // Apply shader parameters
        for (const auto& [name, value] : preset.shaderParameters) {
            m_shaderEngine->setShaderParameter(name, value);
        }
    }

    // 2. Reconfigure capture
    if (preset.captureWidth > 0 && preset.captureHeight > 0) {
        m_capture->setFormat(preset.captureWidth, preset.captureHeight);
        m_capture->setFramerate(preset.captureFps);
    }

    // 3. Apply image settings
    m_brightness = preset.imageBrightness;
    m_contrast = preset.imageContrast;
    m_maintainAspect = preset.maintainAspect;
    m_fullscreen = preset.fullscreen;
    m_monitorIndex = preset.monitorIndex;

    // 4. Apply V4L2 controls
    for (const auto& [name, value] : preset.v4l2Controls) {
        m_capture->setControl(name, value);
    }

    // 5. Update UI
    if (m_ui) {
        m_ui->setBrightness(m_brightness);
        m_ui->setContrast(m_contrast);
        // ... other setters
    }

    // 6. Save current configuration
    if (m_ui) {
        m_ui->saveConfig();
    }
}
```

## 🔄 Usage Flow

### Create Preset

1. User configures RetroCapture (shader, resolution, etc.)
2. Opens Capture Presets window and clicks "Create New Preset"
3. Enters name and description (optional)
4. System captures thumbnail from current viewport
5. System collects all current settings:
   - Current shader and parameters
   - Capture resolution and FPS
   - Image settings
   - V4L2 controls
   - Streaming settings (optional)
6. Saves JSON to `assets/presets/{name}.json`
7. Saves thumbnail to `assets/thumbnails/{name}.png`
8. Updates preset grid

### Apply Preset

1. User clicks on a preset in the grid
2. System loads preset JSON
3. System applies all settings in order:
   - Shader first (may take time)
   - Capture reconfiguration (may briefly pause)
   - Image settings
   - V4L2 controls
4. UI updates to reflect changes
5. System saves `config.json` with new settings

## 📝 Data to Save in Preset

### Required

- ✅ Preset name
- ✅ Shader path (if any)
- ✅ Shader parameters (if any)
- ✅ Capture resolution (width, height)
- ✅ Capture FPS
- ✅ Device path
- ✅ Source type (V4L2/DirectShow)

### Optional (but recommended)

- ✅ Image brightness/contrast
- ✅ Maintain aspect ratio
- ✅ Fullscreen state
- ✅ Monitor index
- ✅ V4L2 controls (if applicable)
- ✅ Streaming settings (if applicable)
- ✅ Thumbnail path

## 🎨 UI Details

### Preset Grid

- **Thumbnail size**: 320x240 pixels (or proportional)
- **Columns**: 3-4 columns (adjustable based on window width)
- **Spacing**: Padding between cards
- **Hover**: Highlight preset on mouse over
- **Tooltip**: Show name and description on hover
- **Loading state**: Show spinner while applying preset

### Thumbnail Placeholder

If no thumbnail exists:

- Show generic icon (e.g., empty image)
- Or generate default thumbnail with preset colors

### Responsiveness

- Grid adapts to window width
- Vertical scroll if there are many presets
- Responsive layout (mobile-friendly if applicable)

## 🔌 Integration with Existing Components

### UIManager

Open `UICapturePresets` window (separate window, not a tab):

```cpp
// In UIManager.h/cpp or Application
if (m_showPresetsWindow) {
    m_capturePresetsUI->render(); // Opens as separate ImGui window
}
```

### Application

Add method to apply preset:

```cpp
class Application {
    // ...
    void applyPreset(const std::string& presetName);
    void createPresetFromCurrentState(const std::string& name, const std::string& description);
};
```

### Required Callbacks

```cpp
// In UIManager
void setOnPresetApplied(std::function<void(const std::string&)> callback);
void setOnPresetCreated(std::function<void(const std::string&, const std::string&)> callback);
```

## 🧪 Testing and Validation

### Test Cases

1. **Create preset**:

   - ✅ Create preset with valid name
   - ✅ Create preset with invalid name (special characters)
   - ✅ Create preset without shader
   - ✅ Create preset with shader and parameters
   - ✅ Thumbnail is generated correctly

2. **Apply preset**:

   - ✅ Apply valid preset
   - ✅ Apply preset with non-existent shader (should fail gracefully)
   - ✅ Apply preset with non-existent device
   - ✅ Apply partial preset (some settings not applicable)

3. **Management**:

   - ✅ Delete preset
   - ✅ Rename preset
   - ✅ Duplicate preset
   - ✅ List empty presets

4. **Edge cases**:
   - ✅ Preset without thumbnail (should show placeholder)
   - ✅ Preset with corrupted thumbnail
   - ✅ Multiple presets with same name (should overwrite or add suffix)
   - ✅ Preset directory doesn't exist (should create)

## 📦 Dependencies

### Already Existing

- ✅ `nlohmann/json` - For JSON parsing/serialization
- ✅ `libpng` - For saving PNG thumbnails
- ✅ OpenGL - For `glReadPixels`
- ✅ ImGui - For UI

### New (if needed)

- None! All dependencies are already in the project.

## 🚀 Implementation Phases

### Phase 1: Core (MVP)

1. ✅ Create basic `PresetManager` (save/load JSON)
2. ✅ Create `PresetData` data structure
3. ✅ Implement saving of current settings
4. ✅ Implement basic loading and application

### Phase 2: Thumbnails

1. ✅ Create `ThumbnailGenerator`
2. ✅ Implement framebuffer capture
3. ✅ Implement PNG saving
4. ✅ Integrate with preset creation

### Phase 3: Basic UI

1. ✅ Create `UICapturePresets`
2. ✅ Implement preset grid
3. ✅ Implement "Create" button
4. ✅ Implement creation dialog
5. ✅ Implement click to apply

### Phase 4: Advanced UI

1. ✅ Context menu (rename, delete, duplicate)
2. ✅ Search/filter
3. ✅ Sorting
4. ✅ Loading states
5. ✅ Validation and error messages

### Phase 5: Polish

1. ✅ Tooltips
2. ✅ Smooth animations
3. ✅ Data validation
4. ✅ Robust error handling
5. ✅ Documentation

## ⚠️ Important Considerations

### Performance

- **Thumbnail generation**: Can be expensive. Do it asynchronously or in separate thread?
- **Grid rendering**: If there are many presets, use virtual scrolling?
- **Preset application**: May briefly pause during reconfiguration. Show loading?

### Threading

- **Thumbnail capture**: Must be done in main thread (OpenGL context)
- **File I/O**: Can be done in separate thread to avoid blocking UI

### Synchronization

- **Current state**: Ensure current state is captured correctly before saving
- **Application**: Ensure all settings are applied in correct order

### Compatibility

- **Versioning**: Presets must have version for future migration
- **Backward compatibility**: Old presets must continue working
- **Cross-platform**: File paths must work on Linux and Windows

## 📚 References

- Existing configuration pattern (`config.json`)
- Shader preset system (already has saving of modified presets)
- Frame capture for streaming (already has `glReadPixels`)

## 🎯 Next Steps

1. **Review planning** with team/user
2. **Create branch** `feature/capture-presets`
3. **Implement Phase 1** (Core)
4. **Test** basic save/load
5. **Iterate** on following phases

---

**Status**: 📋 Planning Complete
**Author**: AI Assistant
**Date**: 2024-01-15
**Version**: 1.0
