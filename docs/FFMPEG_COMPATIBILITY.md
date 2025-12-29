# FFmpeg Compatibility Across Linux Distributions

## Problem

Different Linux distributions ship different versions of FFmpeg/libavcodec:
- **Manjaro/Arch**: FFmpeg 5.8 (libavcodec 58) and FFmpeg 6.2+ (libavcodec 62+)
- **Ubuntu**: FFmpeg 6.0 (libavcodec 60)
- **Other distributions**: Various versions

The FFmpeg API has breaking changes between versions, particularly:
- **Channel Layout API**: Changed in FFmpeg 5.9+ (libavcodec 59+)
  - Old API: `channels` and `channel_layout` fields
  - New API: `ch_layout` structure
- **AVFrame key_frame**: Changed in FFmpeg 5.9+ (libavcodec 59+)
  - Old API: `key_frame` field
  - New API: `flags` with `AV_FRAME_FLAG_KEY`
- **AVIO write callback**: Changed in FFmpeg 6.0+ (libavformat 60+)
  - Old API: `uint8_t*` (non-const)
  - New API: `const uint8_t*`

## Solution

We've implemented a **compatibility layer** (`src/utils/FFmpegCompat.h`) that:

1. **Detects FFmpeg version** at compile time using `LIBAVCODEC_VERSION_MAJOR` and `LIBAVFORMAT_VERSION_MAJOR`
2. **Provides wrapper functions** that automatically use the correct API based on the detected version
3. **Centralizes compatibility logic** in one place for easier maintenance

### Usage

Instead of using version-specific code directly:

```cpp
// ❌ OLD WAY (version-specific)
#if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&codecCtx->ch_layout, channels);
#else
    codecCtx->channels = channels;
    codecCtx->channel_layout = av_get_default_channel_layout(channels);
#endif
```

Use the compatibility functions:

```cpp
// ✅ NEW WAY (compatible with all versions)
#include "../utils/FFmpegCompat.h"

FFmpegCompat::setChannelLayout(codecCtx, channels);
FFmpegCompat::setFrameChannelLayout(frame, channels);
FFmpegCompat::setKeyFrame(frame, true);
```

### Available Functions

- `FFmpegCompat::setChannelLayout(AVCodecContext* ctx, int channels)` - Set channel layout for codec context
- `FFmpegCompat::setFrameChannelLayout(AVFrame* frame, int channels)` - Set channel layout for frame
- `FFmpegCompat::uninitChannelLayout(AVCodecContext* ctx)` - Uninitialize channel layout (only needed for 59+)
- `FFmpegCompat::uninitFrameChannelLayout(AVFrame* frame)` - Uninitialize frame channel layout (only needed for 59+)
- `FFmpegCompat::setKeyFrame(AVFrame* frame, bool isKeyFrame)` - Set key frame flag

### Version Detection

The CMakeLists.txt automatically detects FFmpeg version during configuration:

```cmake
execute_process(
    COMMAND pkg-config --modversion libavcodec
    OUTPUT_VARIABLE AVCODEC_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
```

This information is logged during build configuration to help with debugging.

## Supported Versions

The compatibility layer supports:
- **FFmpeg 5.8** (libavcodec 58) - Manjaro/Arch
- **FFmpeg 6.0** (libavcodec 60) - Ubuntu
- **FFmpeg 6.2+** (libavcodec 62+) - Manjaro/Arch (newer)

## Building

The build system automatically detects the installed FFmpeg version and compiles with the appropriate compatibility code. No manual configuration is needed.

## Testing

To verify compatibility:
1. Build on Manjaro (FFmpeg 5.8 or 6.2+)
2. Build on Ubuntu (FFmpeg 6.0)
3. Both should compile and run correctly

## Future Compatibility

When new FFmpeg versions are released with API changes:
1. Update `FFmpegCompat.h` with new compatibility functions
2. Update version detection macros if needed
3. Test on all target distributions
