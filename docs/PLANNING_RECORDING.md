# Feature: Video Recording System

## 📋 Overview

Implement a complete video recording system that allows saving processed content (with shaders applied) to video files, with full control via local interface and web portal.

## 🎯 Objectives

- ✅ Record processed video (with shaders) to local files
- ✅ Complete codec, resolution, bitrate configuration, etc.
- ✅ Dedicated local interface (Recording + Recordings List)
- ✅ Full control via web portal
- ✅ List and manage recordings
- ✅ Reuse existing encoding infrastructure

## 🏗️ Architecture

### Main Components

```
Application
├── RecordingManager (new)
│   ├── Manages recording state
│   ├── Coordinates MediaEncoder/MediaMuxer
│   └── Manages recording files
├── FileRecorder (new)
│   ├── MediaMuxer specialization for files
│   ├── Writes to file instead of HTTP
│   └── Manages recording metadata
└── RecordingMetadata (new)
    ├── Recording information
    ├── Timestamp, duration, size, codec, etc.
    └── JSON persistence

Local UI
├── UIConfigurationRecording (new)
│   ├── Recording configuration
│   ├── Start/Stop recording
│   └── Real-time status
└── UIRecordingsList (new)
    ├── List existing recordings
    ├── Metadata preview
    ├── Download/Delete
    └── Open recordings folder

Web Portal
├── Recording Tab (new)
│   ├── Same functionality as local UI
│   └── Full remote control
└── Recordings List Tab (new)
    ├── List recordings
    ├── Download via HTTP
    └── Delete via API

REST API
├── GET /api/v1/recording/settings
├── POST /api/v1/recording/settings
├── POST /api/v1/recording/control (start/stop)
├── GET /api/v1/recording/status
├── GET /api/v1/recordings (list)
├── GET /api/v1/recordings/{id}
├── GET /api/v1/recordings/{id}/download
└── DELETE /api/v1/recordings/{id}
```

## 📁 File Structure

### New Files

```
src/recording/
├── RecordingManager.h/cpp
│   └── Manages recording lifecycle
├── FileRecorder.h/cpp
│   └── Specialization for file recording
└── RecordingMetadata.h/cpp
    └── Metadata and persistence

src/ui/
├── UIConfigurationRecording.h/cpp
│   └── Recording configuration window
└── UIRecordingsList.h/cpp
    └── Recordings list window

src/web/
├── recording.html (optional - if using separate tabs)
└── recordings-list.html (optional)

config/
└── recordings.json (recording metadata)
```

## 🔧 Detailed Implementation

### 1. FileRecorder (src/recording/FileRecorder.h/cpp)

**Responsibility**: Write muxed data to file instead of HTTP.

```cpp
class FileRecorder {
public:
    bool initialize(const MediaEncoder::VideoConfig& videoConfig,
                    const MediaEncoder::AudioConfig& audioConfig,
                    void* videoCodecContext,
                    void* audioCodecContext,
                    const std::string& outputPath);
    
    bool startRecording();
    void stopRecording();
    bool isRecording() const;
    
    // Reuses MediaMuxer with file callback
    bool muxPacket(const MediaEncoder::EncodedPacket& packet);
    void flush();
    void cleanup();
    
    // Metadata
    std::string getOutputPath() const;
    uint64_t getFileSize() const;
    uint64_t getDurationUs() const;
};
```

**Features**:
- Reuses `MediaMuxer` with custom file callback
- Supports multiple formats (MP4, MKV, AVI via FFmpeg)
- Manages recording directory creation
- Thread-safe file writing

### 2. RecordingManager (src/recording/RecordingManager.h/cpp)

**Responsibility**: Orchestrate recording, coordinate with Application.

```cpp
class RecordingManager {
public:
    bool initialize();
    void shutdown();
    
    // Recording control
    bool startRecording(const RecordingSettings& settings);
    void stopRecording();
    bool isRecording() const;
    
    // Configuration
    void setRecordingSettings(const RecordingSettings& settings);
    RecordingSettings getRecordingSettings() const;
    
    // Status
    RecordingStatus getStatus() const;
    uint64_t getCurrentDurationUs() const;
    uint64_t getCurrentFileSize() const;
    
    // Frame/Audio input (called by Application)
    void pushFrame(const uint8_t* data, uint32_t width, uint32_t height);
    void pushAudio(const int16_t* samples, size_t sampleCount);
    
    // Recording list
    std::vector<RecordingMetadata> listRecordings();
    bool deleteRecording(const std::string& recordingId);
    std::string getRecordingPath(const std::string& recordingId);
    
private:
    std::unique_ptr<FileRecorder> m_recorder;
    std::unique_ptr<MediaEncoder> m_encoder;
    std::unique_ptr<StreamSynchronizer> m_synchronizer;
    RecordingSettings m_settings;
    RecordingMetadata m_currentMetadata;
    std::thread m_encodingThread;
    // ...
};
```

**Features**:
- Manages complete recording lifecycle
- Separate encoding thread (similar to streaming)
- A/V synchronization using existing `StreamSynchronizer`
- JSON metadata persistence

### 3. RecordingSettings (src/recording/RecordingMetadata.h)

```cpp
struct RecordingSettings {
    // Video
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate = 8000000; // 8 Mbps
    std::string codec = "h264"; // h264, h265, vp8, vp9
    std::string preset = "veryfast";
    std::string h265Profile = "main";
    std::string h265Level = "auto";
    int vp8Speed = 12;
    int vp9Speed = 6;
    
    // Audio
    uint32_t audioBitrate = 256000; // 256 kbps
    std::string audioCodec = "aac";
    
    // Container
    std::string container = "mp4"; // mp4, mkv, avi
    std::string outputPath = "recordings/"; // Base directory
    std::string filenameTemplate = "recording_%Y%m%d_%H%M%S"; // strftime format
    
    // Options
    bool includeAudio = true;
    bool autoStart = false;
    uint64_t maxDurationUs = 0; // 0 = no limit
    uint64_t maxFileSize = 0; // 0 = no limit
};
```

### 4. RecordingMetadata (src/recording/RecordingMetadata.h/cpp)

```cpp
struct RecordingMetadata {
    std::string id;              // UUID or hash
    std::string filename;        // File name
    std::string filepath;        // Full path
    std::string container;       // mp4, mkv, etc.
    
    // Codec info
    std::string videoCodec;
    std::string audioCodec;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    
    // File info
    uint64_t fileSize;           // Bytes
    uint64_t durationUs;         // Microseconds
    std::string createdAt;       // ISO 8601 timestamp
    
    // Thumbnail (optional)
    std::string thumbnailPath;   // Thumbnail path
    
    // JSON serialization
    nlohmann::json toJSON() const;
    static RecordingMetadata fromJSON(const nlohmann::json& json);
};
```

### 5. UIConfigurationRecording (src/ui/UIConfigurationRecording.h/cpp)

**Local Interface**:
- "Recording" tab in configuration window
- Controls similar to Streaming tab:
  - Codec selection (H.264, H.265, VP8, VP9)
  - Resolution dropdown
  - FPS dropdown
  - Bitrate controls (video + audio)
  - Container format (MP4, MKV, AVI)
  - Output directory selector
  - Filename template
  - Max duration/size limits
- Start/Stop button with status
- Progress indicator (duration, size)
- Cooldown system (similar to streaming)

### 6. UIRecordingsList (src/ui/UIRecordingsList.h/cpp)

**Local Interface**:
- New separate window (not a tab)
- Open via menu "View → Recordings" or dedicated button
- Recording list with:
  - Thumbnail (first frame)
  - File name
  - Date/time
  - Duration
  - Size
  - Codec info
  - Resolution
- Actions:
  - Play (open with external player)
  - Open folder
  - Delete
  - Copy path
- Filters:
  - By date
  - By codec
  - By size
- Sorting:
  - Date (newest first)
  - Size
  - Duration

### 7. REST API Endpoints

#### GET /api/v1/recording/settings
```json
{
  "width": 1920,
  "height": 1080,
  "fps": 60,
  "bitrate": 8000000,
  "codec": "h264",
  "preset": "veryfast",
  "audioBitrate": 256000,
  "audioCodec": "aac",
  "container": "mp4",
  "outputPath": "recordings/",
  "filenameTemplate": "recording_%Y%m%d_%H%M%S",
  "includeAudio": true,
  "maxDuration": 0,
  "maxFileSize": 0
}
```

#### POST /api/v1/recording/settings
```json
{
  "width": 1280,
  "height": 720,
  "fps": 30,
  "codec": "h265",
  ...
}
```

#### POST /api/v1/recording/control
```json
{
  "action": "start" | "stop"
}
```

#### GET /api/v1/recording/status
```json
{
  "isRecording": true,
  "duration": 12345,
  "fileSize": 1048576,
  "currentFile": "recordings/recording_20241215_143022.mp4",
  "settings": { ... }
}
```

#### GET /api/v1/recordings
```json
{
  "recordings": [
    {
      "id": "abc123",
      "filename": "recording_20241215_143022.mp4",
      "filepath": "/path/to/recording_20241215_143022.mp4",
      "container": "mp4",
      "videoCodec": "h264",
      "audioCodec": "aac",
      "width": 1920,
      "height": 1080,
      "fps": 60,
      "fileSize": 104857600,
      "duration": 30000000,
      "createdAt": "2024-12-15T14:30:22Z"
    },
    ...
  ],
  "total": 10
}
```

#### GET /api/v1/recordings/{id}
Returns metadata for a specific recording.

#### GET /api/v1/recordings/{id}/download
Download recording file via HTTP.

#### DELETE /api/v1/recordings/{id}
Delete recording from disk.

### 8. Web Portal Integration

**New Tabs in Web Portal**:

1. **Recording Tab**:
   - Same controls as local UI
   - Real-time status updates
   - Start/Stop controls
   - Settings form

2. **Recordings List Tab**:
   - Recordings table
   - Thumbnails (first frame)
   - Download buttons
   - Delete buttons
   - Filters and sorting

**Files to modify**:
- `src/web/index.html` - Add new tabs
- `src/web/control.js` - Control logic
- `src/web/api.js` - API methods
- `src/web/style.css` - Styles

## 🔄 Data Flow

```
Application::run()
├── Capture frame (VideoCapture)
├── Process frame (FrameProcessor)
├── Apply shader (ShaderEngine)
├── Render (OpenGLRenderer)
│
├── [If recording active]
│   ├── Capture rendered frame (glReadPixels)
│   └── RecordingManager::pushFrame()
│       └── FileRecorder (via encoding thread)
│
└── [If recording active]
    ├── Capture audio (AudioCapture)
    └── RecordingManager::pushAudio()
        └── FileRecorder (via encoding thread)

RecordingManager::encodingThread
├── StreamSynchronizer::calculateSyncZone()
├── Encode video frames
├── Encode audio chunks
└── FileRecorder::muxPacket() → Write to file
```

## 📝 Application Integration

### Modifications to Application.h/cpp

```cpp
class Application {
    // ...
    std::unique_ptr<RecordingManager> m_recordingManager;
    
    // Recording configuration
    void setRecordingSettings(const RecordingSettings& settings);
    RecordingSettings getRecordingSettings() const;
    bool startRecording();
    void stopRecording();
    bool isRecording() const;
    RecordingStatus getRecordingStatus() const;
    
    // Recording list
    std::vector<RecordingMetadata> listRecordings();
    bool deleteRecording(const std::string& recordingId);
    std::string getRecordingDownloadPath(const std::string& recordingId);
};
```

### Modifications to UIManager.h/cpp

```cpp
class UIManager {
    // ...
    std::unique_ptr<UIConfigurationRecording> m_recordingConfig;
    std::unique_ptr<UIRecordingsList> m_recordingsList;
    
    // Recording callbacks
    void setOnRecordingStart(std::function<void()> callback);
    void setOnRecordingStop(std::function<void()> callback);
    void setOnRecordingSettingsChanged(std::function<void(const RecordingSettings&)> callback);
};
```

## 🎨 UI/UX Design

### Recording Tab (Local)
- Layout similar to Streaming tab
- Codec selection dropdown
- Resolution/FPS dropdowns
- Bitrate sliders
- Container format selector
- Output path selector (with "Browse" button)
- Filename template input
- Max duration/size inputs
- Large visible Start/Stop button
- Status display:
  - Recording indicator (pulsing red dot)
  - Current duration (HH:MM:SS)
  - Current file size (MB/GB)
  - Current filename

### Recordings List Window (Local)
- Separate window (not a tab)
- Open via menu "View → Recordings" or dedicated button
- Grid/list view layout
- Thumbnail + metadata cards
- Context menu (right-click):
  - Play
  - Open folder
  - Delete
  - Copy path
- Toolbar:
  - Refresh
  - Open recordings folder
  - Filter dropdown
  - Sort dropdown

### Web Portal
- Same tab structure
- Responsive design
- Download progress indicators
- Toast notifications for actions

## 🔐 Security and Validation

- Validate output paths (prevent path traversal)
- Validate size/duration limits
- Check disk space before starting
- Validate write permissions
- Sanitize file names (remove invalid characters)
- Limit listing size (pagination if necessary)

## 📊 Persistence

### config/recordings.json
```json
{
  "recordings": [
    {
      "id": "abc123",
      "filename": "recording_20241215_143022.mp4",
      "filepath": "/absolute/path/to/recording_20241215_143022.mp4",
      "container": "mp4",
      "videoCodec": "h264",
      "audioCodec": "aac",
      "width": 1920,
      "height": 1080,
      "fps": 60,
      "fileSize": 104857600,
      "duration": 30000000,
      "createdAt": "2024-12-15T14:30:22Z",
      "thumbnailPath": "recordings/.thumbnails/abc123.jpg"
    }
  ],
  "settings": {
    "defaultOutputPath": "recordings/",
    "defaultFilenameTemplate": "recording_%Y%m%d_%H%M%S",
    "autoGenerateThumbnails": true,
    "maxRecordingsToKeep": 0
  }
}
```

## 🚀 Implementation Phases

### Phase 1: Core Recording (MVP)
- [ ] Basic FileRecorder
- [ ] Basic RecordingManager
- [ ] Application integration
- [ ] Basic UI (start/stop only)
- [ ] Basic API (start/stop)

### Phase 2: Complete Configuration
- [ ] Complete RecordingSettings
- [ ] Complete UIConfigurationRecording
- [ ] Configuration API
- [ ] Web portal recording tab

### Phase 3: Listing and Management
- [ ] RecordingMetadata and persistence
- [ ] UIRecordingsList
- [ ] Listing API
- [ ] Web portal recordings list
- [ ] Download via HTTP

### Phase 4: Improvements
- [ ] Thumbnail generation
- [ ] Filters and sorting
- [ ] Auto-cleanup (max recordings)
- [ ] Improved progress indicators
- [ ] Notifications

## 🧪 Testing

- [ ] Basic recording (H.264, MP4)
- [ ] Multiple codecs (H.265, VP8, VP9)
- [ ] Multiple containers (MP4, MKV, AVI)
- [ ] Recording with/without audio
- [ ] Duration/size limits
- [ ] Listing and download
- [ ] Recording deletion
- [ ] Complete REST API
- [ ] Complete web portal
- [ ] Performance (don't impact streaming)

## 📚 Dependencies

- FFmpeg (already exists)
- nlohmann/json (already exists)
- FilesystemCompat (already exists)
- Threading (already exists)

## ⚠️ Performance Considerations

### Impact Analysis

#### 1. **glReadPixels (Main Bottleneck)**
- **Current status**: Already executed once per frame when streaming is active
- **Additional impact**: **Minimal** - we can reuse the same buffer
- **Solution**: Share buffer between streaming and recording when both active
- **Cost**: ~5-15ms per frame at 1080p (depends on GPU)

#### 2. **Encoding (CPU)**
- **Current status**: Separate thread for streaming (doesn't block main thread)
- **Additional impact**: 
  - **Option A (Share encoder)**: Minimal - just write to two places
  - **Option B (Separate encoders)**: Medium - 2x CPU usage for encoding
- **Recommendation**: **Option A** - Share MediaEncoder when possible
- **Cost**: 
  - H.264 1080p@60fps: ~15-25% CPU (one encoder)
  - H.264 1080p@60fps: ~25-40% CPU (two separate encoders)

#### 3. **I/O (Disk vs Network)**
- **Streaming**: Write to memory (buffer) → network (HTTP)
- **Recording**: Write to memory (buffer) → disk (file)
- **Impact**: **Low to Medium** - depends on disk speed
- **SSD**: Practically imperceptible
- **HDD**: May cause micro-stutters if very slow
- **Cost**: ~1-5ms per frame (depends on disk)

#### 4. **Memory**
- **Current status**: Synchronization buffers for streaming
- **Additional impact**: **Low** - shared buffers when possible
- **Cost**: +50-100MB RAM (additional buffers if separate encoders)

### Usage Scenarios

#### Scenario 1: Recording Only (no streaming)
- **Impact**: **Low** - Similar to current streaming
- **CPU**: +15-25% (encoding)
- **GPU**: +5-15ms/frame (glReadPixels)
- **Disk**: +1-5ms/frame (write)
- **RAM**: +50MB (buffers)

#### Scenario 2: Recording + Streaming Simultaneous (Shared Encoders)
- **Impact**: **Low to Medium**
- **CPU**: +20-30% (shared encoding + overhead)
- **GPU**: +5-15ms/frame (glReadPixels - reused)
- **Disk**: +1-5ms/frame (write)
- **Network**: No additional impact
- **RAM**: +50MB (additional buffers)
- **Advantage**: Reuses encoding, only duplicates write

#### Scenario 3: Recording + Streaming Simultaneous (Separate Encoders)
- **Impact**: **Medium to High**
- **CPU**: +40-60% (two encoders)
- **GPU**: +5-15ms/frame (glReadPixels - reused)
- **Disk**: +1-5ms/frame (write)
- **Network**: No additional impact
- **RAM**: +100MB (two sets of buffers)
- **Disadvantage**: Much more CPU, but allows different codecs/resolutions

### Recommended Optimizations

#### 1. **Share glReadPixels**
```cpp
// In main loop
if (streaming || recording) {
    glReadPixels(...); // Once only
    if (streaming) streamManager->pushFrame(data, w, h);
    if (recording) recordingManager->pushFrame(data, w, h);
}
```

#### 2. **Share MediaEncoder (when possible)**
- Same codec, resolution, bitrate → share encoder
- Different codecs/resolutions → separate encoders
- **Benefit**: Reduces CPU by ~50% when shared

#### 3. **Smart Buffering**
- Use larger buffers for recording (less I/O)
- Async flush to disk
- **Benefit**: Reduces stutters on slow HDDs

#### 4. **Adaptive Resolution**
- Allow recording at lower resolution than capture
- **Benefit**: Significantly reduces encoding CPU

#### 5. **Codec Selection**
- H.264 "veryfast" for recording (faster than streaming)
- **Benefit**: Less CPU, larger files but acceptable

### Known Limitations

1. **glReadPixels is synchronous**: May block main thread at very high resolutions (4K+)
2. **Encoding is CPU-intensive**: On weaker systems, may cause frame drops
3. **Disk I/O**: Slow HDDs may cause micro-stutters
4. **Memory**: Multiple encoders consume more RAM

### Implementation Recommendations

1. **Phase 1 (MVP)**: Separate encoder, no optimizations
   - Functional, but may be heavy
   - Allows testing complete functionality

2. **Phase 2 (Optimization)**: Share encoder when possible
   - Significantly reduces CPU impact
   - Implement automatic detection logic

3. **Phase 3 (Advanced)**: PBO for async glReadPixels
   - Reduces main thread blocking
   - Improves performance at high resolutions

### Expected Metrics

**Reference System**: Modern CPU (6+ cores), dedicated GPU, SSD

| Scenario | CPU | GPU Impact | FPS Impact | Recommended |
|----------|-----|------------|------------|-------------|
| Recording Only (1080p@60) | +15-25% | +5-15ms | -2-5 FPS | ✅ Yes |
| Recording + Streaming (shared) | +20-30% | +5-15ms | -3-7 FPS | ✅ Yes |
| Recording + Streaming (separate) | +40-60% | +5-15ms | -5-15 FPS | ⚠️ Depends |
| Recording 4K@60 | +30-50% | +20-40ms | -10-20 FPS | ⚠️ Heavy |

### Conclusion

**Overall Impact**: **Low to Medium**, depending on configuration

- **Recording alone**: Impact similar to current streaming (acceptable)
- **Recording + Streaming (shared)**: Moderate impact, but manageable
- **Recording + Streaming (separate)**: High impact, only for specific cases

**Recommendation**: Implement with option to share encoder, allowing user to choose between performance and flexibility.

## ⚠️ Other Considerations

1. **Disk space**: Monitor usage and warn user
2. **Threading**: Separate encoding thread (similar to streaming)
3. **Synchronization**: Reuse existing StreamSynchronizer
4. **File format**: MP4 is more compatible, MKV supports more codecs
5. **Thumbnails**: Generate first frame as thumbnail (optional)
6. **Cleanup**: Implement auto-cleanup of old recordings (optional)

## 🎯 Priorities

1. **High**: Core recording, basic UI, basic API
2. **Medium**: Complete configuration, basic listing
3. **Low**: Thumbnails, advanced filters, auto-cleanup

## 📌 Implementation Notes

### Code Reuse

- **MediaEncoder**: Reuse completely (already supports all codecs)
- **MediaMuxer**: Adapt to support files (in addition to HTTP callback)
- **StreamSynchronizer**: Reuse completely
- **APIController**: Add new endpoints following existing pattern
- **UIConfigurationStreaming**: Use as reference for UIConfigurationRecording

### Differences from Streaming

- **Output**: File instead of HTTP
- **Container**: MP4/MKV instead of MPEG-TS
- **Metadata**: Need to store information about recordings
- **Persistence**: JSON for metadata, files on disk
- **Download**: Serve files via HTTP for web portal

### Application Integration

- Add `RecordingManager` as main component
- Capture frames same way as streaming (glReadPixels)
- Capture audio same way as streaming
- Allow recording + streaming simultaneous (share encoding if possible)

## 🔄 Next Steps

1. Create `src/recording/` directory structure
2. Implement `RecordingMetadata` (data structure)
3. Implement `FileRecorder` (based on MediaMuxer)
4. Implement `RecordingManager` (orchestration)
5. Integrate with `Application`
6. Create `UIConfigurationRecording`
7. Create `UIRecordingsList`
8. Add API endpoints
9. Add tabs to web portal
10. Testing and refinements
