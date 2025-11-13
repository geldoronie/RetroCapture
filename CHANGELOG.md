# Changelog

All notable changes to RetroCapture will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Real-time video capture via V4L2
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
- Portable AppImage distribution
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

### Known Issues
- Not all shaders work perfectly (some complex multi-pass presets may have issues)
- Some shader features may not be fully implemented yet
- Single-threaded architecture may limit performance on multi-core systems

---

## [0.1.0] - TBD

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
