# Streaming Feature Planning

## Overview
Implement streaming functionality to allow remote access to the RetroCapture video feed over the network.

## Target Protocols

### Phase 1: HTTP MJPEG (Simple)
- **Priority**: High
- **Complexity**: Low
- **Use Case**: Quick browser access, simple setup
- **Implementation**: 
  - HTTP server (libmicrohttpd or custom)
  - MJPEG encoding (convert RGB frames to JPEG)
  - Simple `/stream` endpoint

### Phase 2: RTSP (Professional)
- **Priority**: High
- **Complexity**: Medium
- **Use Case**: Professional streaming, VLC/ffplay clients
- **Implementation**:
  - RTSP server (live555 or custom with libavformat)
  - H.264 encoding (libx264 via FFmpeg)
  - RTP packetization

### Phase 3: WebRTC (Modern)
- **Priority**: Medium
- **Complexity**: High
- **Use Case**: Browser-native, low latency
- **Implementation**:
  - WebRTC library (libwebrtc or Pion)
  - WebSocket signaling server
  - VP8/VP9 or H.264 encoding

## Architecture

```
Application
    └── StreamManager (new)
        ├── HTTPMJPEGStreamer
        ├── RTSPStreamer
        └── WebRTCStreamer
```

## Dependencies

### Required
- **FFmpeg/libav** (for encoding)
  - libavcodec (H.264 encoding)
  - libavformat (RTSP, container formats)
  - libswscale (format conversion)

### Optional
- **libmicrohttpd** (lightweight HTTP server)
- **live555** (RTSP server implementation)
- **libjpeg-turbo** (fast JPEG encoding for MJPEG)

## Implementation Steps

1. **Create StreamManager interface**
   - Abstract base class for streamers
   - Common configuration (port, quality, etc.)

2. **Implement HTTP MJPEG Streamer**
   - Simple HTTP server
   - JPEG encoding per frame
   - `/stream` endpoint returning multipart/x-mixed-replace

3. **Integrate with Application**
   - Add StreamManager to Application
   - Hook into frame processing pipeline
   - Add UI controls (start/stop, port config)

4. **Implement RTSP Streamer**
   - RTSP server setup
   - H.264 encoding pipeline
   - RTP streaming

5. **Add configuration**
   - CLI parameters for streaming
   - UI controls for streaming settings
   - Quality/bitrate settings

## Frame Pipeline Integration

```
Capture → Process → Render → [Stream]
                              ↑
                         StreamManager
```

The StreamManager should:
- Receive processed frames (after shaders, if enabled)
- Encode frames in background thread
- Serve to clients

## Configuration

### CLI Parameters
- `--stream-enable`: Enable streaming
- `--stream-protocol`: Protocol (mjpeg, rtsp, webrtc)
- `--stream-port`: Port number
- `--stream-quality`: Quality/bitrate
- `--stream-width`: Stream resolution width
- `--stream-height`: Stream resolution height

### UI Controls
- Enable/disable streaming
- Protocol selection
- Port configuration
- Quality slider
- Active clients count
- Stream URL display

## Security Considerations

- Authentication (optional)
- Network binding (localhost vs. all interfaces)
- Rate limiting
- Connection limits

## Performance

- Encoding should happen in separate thread
- Frame rate may need to be limited for streaming
- Consider frame skipping if encoding can't keep up
- Memory management for encoded frames

