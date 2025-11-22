# RetroCapture

RetroCapture is a real-time video capture application for Linux that allows you to apply RetroArch shaders (GLSL) to live video feeds from capture cards, providing authentic retro visual effects and advanced image processing.

## 🎯 Motivation

RetroCapture was born from the desire to experience retro gaming with the authentic feel of classic CRT televisions, while also supporting modern upscaling techniques used in real hardware emulators. The project aims to bridge the gap between nostalgic visual experiences and modern capture technology.

### Key Motivations

- **Authentic Retro Experience**: Apply CRT shaders to recreate the look and feel of classic tube televisions, complete with scanlines, phosphor glow, and curvature effects
- **Modern Upscaling**: Use advanced upscaling shaders (xBR, Super-xBR, etc.) to enhance image quality while maintaining the retro aesthetic
- **Real Hardware Emulation**: Achieve visual fidelity similar to what you'd get from real hardware emulators like the Analogue Pocket, MiSTer, or FPGA-based systems
- **Live Processing**: Process video in real-time from capture cards, allowing you to stream or record with shader effects applied instantly
- **Flexibility**: Support for hundreds of RetroArch shaders, giving you the freedom to customize your visual experience

## ⚠️ Development Status

**RetroCapture is currently in active development (v0.2.0-alpha).** 

### Shader Support
While many shaders work perfectly, not all shaders function as expected. Some complex multi-pass presets may have issues, and certain shader features may not be fully implemented yet. However, a significant number of shaders are working well, including:

- ✅ Most CRT shaders (CRT-Geom, CRT-Pi, CRT-Guest, etc.)
- ✅ NTSC/PAL shaders
- ✅ Upscaling shaders (xBR, Super-xBR, etc.)
- ✅ Handheld console shaders (Game Boy, Game Boy Color, etc.)
- ✅ Many single-pass and simple multi-pass shaders

### Streaming Status
- ✅ **H.264**: Fully functional and stable
- ✅ **H.265**: Fully functional with profile and level support
- ⚠️ **VP8/VP9**: Functional but may appear as "Data: bin_data" in some players (implementation incomplete)

We're continuously working to improve compatibility and add support for more shader features. Contributions and bug reports are welcome!

## ✨ Features

### Video Capture & Processing
- ✅ Real-time video capture via V4L2
- ✅ Full support for RetroArch shaders (GLSL)
- ✅ Multi-pass presets
- ✅ V4L2 controls (brightness, contrast, saturation, etc.)
- ✅ Fullscreen and multi-monitor support
- ✅ Aspect ratio maintenance
- ✅ Real-time shader parameter editing

### Streaming (NEW in 0.2.0-alpha)
- ✅ **HTTP MPEG-TS Streaming**: Stream your processed video over HTTP
- ✅ **Multi-Codec Support**: H.264, H.265/HEVC, VP8, and VP9
- ✅ **Audio Streaming**: PulseAudio integration for system audio capture
- ✅ **Real-time A/V Synchronization**: Timestamp-based synchronization system
- ✅ **Multiple Concurrent Clients**: Support for multiple viewers simultaneously
- ✅ **Configurable Quality**: Adjustable bitrates, presets, and codec-specific options

### User Interface
- ✅ Graphical interface with ImGui
- ✅ Configuration persistence (automatic save/load)
- ✅ Stream management UI with codec selection
- ✅ Real-time stream status and client count

### Distribution
- ✅ Portable AppImage distribution

## 📸 Visual Examples

### Comparison: Without Shader vs. With Shader

RetroCapture allows you to apply RetroArch shaders in real-time to your video capture, transforming the visual appearance with authentic retro effects.

#### CRT Shader (Mattias)

| Without Shader | With CRT Mattias Shader |
|----------------|-------------------------|
| ![Without Shader - Mattias](docs/sonic-no-shaders-mattias.png) | ![With Shader - Mattias](docs/sonic-with-shaders-mattias.png) |

#### NTSC Shader

| Without Shader | With NTSC Shader |
|---------------|------------------|
| ![Without Shader - NTSC](docs/sonic-no-shaders-ntsc.png) | ![With Shader - NTSC](docs/sonic-with-shaders-ntsc.png) |

## 📋 Requirements

### Core Dependencies
- Linux (with V4L2 support)
- OpenGL 3.3+
- GLFW 3.x
- libv4l2
- libpng
- CMake 3.10+
- C++17 compiler

### Streaming Dependencies (for streaming features)
- **FFmpeg** (libavcodec, libavformat, libavutil, libswscale, libswresample)
  - **libx264**: For H.264 encoding
  - **libx265**: For H.265/HEVC encoding (optional)
  - **libvpx**: For VP8/VP9 encoding (optional)
  - **libfdk-aac** or **libavcodec AAC**: For audio encoding
- **PulseAudio**: For audio capture
- **nlohmann/json**: For configuration persistence (automatically fetched by CMake)

## 🔨 Building

```bash
# Build
./build.sh

# Run
./build/bin/retrocapture
```

## 📖 Usage

### Basic Usage

```bash
# Basic capture
./build/bin/retrocapture --device /dev/video0

# With shader
./build/bin/retrocapture --preset shaders/shaders_glsl/crt/crt-guest-dr-venom.glslp

# Custom resolution and framerate
./build/bin/retrocapture --width 1920 --height 1080 --fps 60

# Fullscreen
./build/bin/retrocapture --fullscreen --monitor 0
```

### Streaming

Streaming can be configured and started via the GUI (Stream tab) or will use default settings. Once streaming is active:

1. **Access the stream**: Open `http://localhost:8080/stream` in a media player (VLC, ffplay, mpv, etc.)
2. **Multiple clients**: Multiple viewers can connect simultaneously
3. **Codec selection**: Choose from H.264, H.265, VP8, or VP9 codecs
4. **Audio included**: System audio is automatically captured and streamed (via PulseAudio)

**Example with ffplay**:
```bash
# In one terminal, start RetroCapture
./build/bin/retrocapture --preset shaders/shaders_glsl/crt/crt-pi.glslp

# In another terminal, view the stream
ffplay http://localhost:8080/stream
```

**Example with VLC**:
```bash
# Open VLC → Media → Open Network Stream
# Enter: http://localhost:8080/stream
```

### Command-Line Parameters

#### Shader Options

- `--shader <path>`: Load a simple GLSL shader file (.glsl)
- `--preset <path>`: Load a RetroArch shader preset (.glslp)

#### Capture Device

- `--device <path>`: Specify V4L2 device path (default: `/dev/video0`)

#### Capture Resolution and Framerate

- `--width <pixels>`: Capture width (default: 1920)
- `--height <pixels>`: Capture height (default: 1080)
- `--fps <fps>`: Capture framerate (default: 60)

#### Window Configuration

- `--window-width <pixels>`: Initial window width (default: 1920)
- `--window-height <pixels>`: Initial window height (default: 1080)
- `--fullscreen`: Start in fullscreen mode
- `--monitor <index>`: Select monitor for fullscreen (-1 = primary, 0+ = specific monitor)
- `--maintain-aspect`: Maintain aspect ratio when resizing window

#### Image Processing

- `--brightness <value>`: Overall image brightness (0.0 to 2.0, default: 1.0)
- `--contrast <value>`: Overall image contrast (0.0 to 2.0, default: 1.0)

#### V4L2 Hardware Controls

These parameters directly control the capture device hardware settings. Values are device-specific and may vary.

- `--v4l2-brightness <value>`: V4L2 brightness control
- `--v4l2-contrast <value>`: V4L2 contrast control
- `--v4l2-saturation <value>`: V4L2 saturation control
- `--v4l2-hue <value>`: V4L2 hue control
- `--v4l2-gain <value>`: V4L2 gain control
- `--v4l2-exposure <value>`: V4L2 exposure control
- `--v4l2-sharpness <value>`: V4L2 sharpness control
- `--v4l2-gamma <value>`: V4L2 gamma control
- `--v4l2-whitebalance <value>`: V4L2 white balance temperature control

**Note**: V4L2 control ranges and availability depend on your capture device. Use the GUI to discover available controls and their ranges.

### Example Commands

```bash
# Capture from /dev/video1 at 640x480, 30fps with CRT shader
./build/bin/retrocapture \
  --device /dev/video1 \
  --width 640 --height 480 --fps 30 \
  --preset shaders/shaders_glsl/crt/crt-pi.glslp

# Fullscreen capture with NTSC shader on monitor 1
./build/bin/retrocapture \
  --fullscreen --monitor 1 \
  --preset shaders/shaders_glsl/ntsc/ntsc-320px-svideo-gauss-scanline.glslp \
  --maintain-aspect

# Capture with custom brightness/contrast and V4L2 controls
./build/bin/retrocapture \
  --brightness 1.2 --contrast 1.1 \
  --v4l2-brightness 60 --v4l2-contrast 40 \
  --preset shaders/shaders_glsl/crt/crt-geom.glslp
```

## 📦 Building AppImage

```bash
./build-appimage.sh
```

This will generate a `RetroCapture-<version>-x86_64.AppImage` file that can be run on any compatible Linux distribution without installation.

## 🎮 Using the GUI

Press **F12** to toggle the graphical interface. The GUI provides:

- **Shaders Tab**: Browse and select shaders, edit shader parameters in real-time, save modified presets
- **Image Tab**: Adjust brightness/contrast, toggle aspect ratio, fullscreen settings, monitor selection
- **V4L2 Tab**: Control hardware settings, change resolution/framerate, select capture device
- **Stream Tab** (NEW): Configure streaming settings
  - Enable/disable streaming
  - Select video codec (H.264, H.265, VP8, VP9)
  - Configure codec-specific options (presets, profiles, levels, speed)
  - Set stream resolution and FPS (via dropdowns)
  - Adjust video and audio bitrates
  - View stream URL and active client count
  - Start/stop streaming
- **Info Tab**: View capture device information, resolution, framerate, and application details

**Note**: All settings are automatically saved to `config.json` and restored on application startup.

## 📚 Documentation

### For Users

- **[CHANGELOG.md](CHANGELOG.md)**: Complete changelog with all version history and new features.

### For Developers

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**: Complete system architecture documentation, main components, data flow, streaming architecture, and code structure.

- **[docs/DESIGN_PATTERNS.md](docs/DESIGN_PATTERNS.md)**: Design patterns used in the project, SOLID principles applied, and contributor guide.

- **[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)**: Contribution guidelines, commit rules, and development process.

## 🤝 Contributing

We welcome contributions! Before contributing:

1. Read [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) to understand commit rules and contribution process
2. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) to understand the code structure
3. Consult [docs/DESIGN_PATTERNS.md](docs/DESIGN_PATTERNS.md) to follow established patterns
4. Keep documentation updated when adding new features

### Reporting Issues

When reporting shader compatibility issues, please include:
- Shader preset name and path
- Error messages (if any)
- Expected vs. actual behavior
- Your system information (GPU, drivers, etc.)

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🔗 Additional Resources

- **Source Code**: See `src/` for implementations
- **Shader Examples**: See `shaders/` for shader examples
- **Build System**: See `build.sh` and `CMakeLists.txt` for build information
- **Streaming**: See `src/streaming/` for streaming implementation
- **Audio Capture**: See `src/audio/` for PulseAudio integration

## 🆕 What's New in 0.2.0-alpha

This release introduces major new features:

- **HTTP MPEG-TS Streaming**: Stream your processed video with audio over HTTP
- **Multi-Codec Support**: Choose from H.264, H.265, VP8, or VP9 codecs
- **Audio Capture**: System audio capture via PulseAudio with virtual sink support
- **Configuration Persistence**: All settings automatically saved and restored
- **Improved UI**: New Stream tab with codec selection and streaming controls
- **Better Performance**: Multi-threaded architecture for encoding and streaming

See [CHANGELOG.md](CHANGELOG.md) for complete details.

## 🙏 Acknowledgments

- RetroArch community for the extensive shader library
- GLFW, ImGui, and other open-source projects that make this possible

---

**Note**: RetroCapture is not affiliated with RetroArch or Sega. This is an independent project for video capture and shader processing.
