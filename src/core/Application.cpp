#include "Application.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureFactory.h"
#include "../capture/VideoCaptureRemote.h"
#include "../streaming/RemoteMetaSync.h"
#include "../encoding/MediaEncoder.h"
#ifdef PLATFORM_LINUX
#include "../v4l2/V4L2ControlMapper.h"
#endif
// FrameProcessor and OpenGLRenderer work on all platforms
#include "../processing/FrameProcessor.h"
#include "../renderer/OpenGLRenderer.h"
#include "../renderer/PBOManager.h"
#ifdef USE_SDL2
#include "../output/WindowManagerSDL.h"
#else
#include "../output/WindowManager.h"
#endif
#include "../shader/ShaderEngine.h"
#include "../ui/UIManager.h"
#include "../osd/QuickActionsOverlay.h"
#include "../ui/UIRemoteConnection.h"
#include "../ui/UICapturePresets.h"
#include "../ui/UIRecordings.h"
#include "../renderer/glad_loader.h"
#include "../streaming/StreamManager.h"
#include "../streaming/DirectoryClient.h"
#include "../streaming/DirectoryBrowser.h"
#include "../ui/UIDirectoryBrowser.h"
#include "../streaming/CloudflaredManager.h"
#include "../utils/PasswordHash.h"

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif
#include "../streaming/HTTPTSStreamer.h"
#include "../audio/IAudioCapture.h"
#include "../audio/AudioCaptureFactory.h"
#ifdef __linux__
#include "../audio/AudioCapturePulse.h"
#endif
#include "../recording/RecordingManager.h"
#include "../recording/RecordingSettings.h"
#include "../recording/RecordingMetadata.h"
#include "../utils/PresetManager.h"
#include "../utils/ThumbnailGenerator.h"
#ifdef USE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_keyboard.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif
#ifdef PLATFORM_LINUX
#include <linux/videodev2.h>
#endif
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#ifdef PLATFORM_LINUX
#include <unistd.h>
#endif
#include "../utils/FilesystemCompat.h"
#include <time.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

// swscale removed - resize is now done in encoding (HTTPTSStreamer)

Application::Application()
{
}

Application::~Application()
{
    shutdown();
}

bool Application::init()
{
    LOG_INFO("Initializing Application...");

    // Migrar layouts antigos (~/.config/retrocapture/{assets,ssl} ou
    // %APPDATA%\RetroCapture\{assets,ssl}) pra novo XDG/Known-Folders
    // layout. Idempotente — vê marcador `MIGRATED.txt` no destino.
    Paths::migrateLegacyDataIfNeeded();

    if (!initWindow())
    {
        return false;
    }
    LOG_INFO("Window initialized");

    if (!initRenderer())
    {
        return false;
    }
    LOG_INFO("Renderer initialized");

    if (!initCapture())
    {
        LOG_WARN("Failed to initialize capture - continuing in dummy mode");
        // Don't return false, continue in dummy mode
    }
    else
    {
        LOG_INFO("Capture initialized");
    }

    if (!initUI())
    {
        return false;
    }
    LOG_INFO("UI initialized");

    // Connect ShaderEngine to UI for parameters
    if (m_ui && m_shaderEngine)
    {
        m_ui->setShaderEngine(m_shaderEngine.get());
    }

    // Initialize RecordingManager (independent of streaming/audio)
    LOG_INFO("Initializing RecordingManager...");
    m_recordingManager = std::make_unique<RecordingManager>();
    if (!m_recordingManager->initialize())
    {
        LOG_ERROR("Failed to initialize RecordingManager");
        m_recordingManager.reset();
        // Don't return false, continue without recording
    }
    else
    {
        LOG_INFO("RecordingManager initialized");
    }

    if (!initStreaming())
    {
        LOG_WARN("Failed to initialize streaming - continuing without streaming");
    }

    // Initialize audio capture (required for streaming and/or recording)
    // Audio is needed for recording even if streaming is not enabled
    if (m_streamingEnabled || m_recordingManager)
    {
        if (!initAudioCapture())
        {
            LOG_WARN("Failed to initialize audio capture - continuing without audio");
        }
        else
        {
            // Set audio format for RecordingManager if audio is available
            if (m_recordingManager && m_audioCapture)
            {
                m_recordingManager->setAudioFormat(m_audioCapture->getSampleRate(), m_audioCapture->getChannels());
            }
            
            // Restore saved audio device connections
            restoreAudioDeviceConnections();
        }
    }

    m_initialized = true;

    // Ensure viewport is updated after complete initialization (important for fullscreen)
    if (m_window && m_shaderEngine)
    {
        uint32_t currentWidth = m_window->getWidth();
        uint32_t currentHeight = m_window->getHeight();
        m_shaderEngine->setViewport(currentWidth, currentHeight);
    }

    // Apply the source-type-appropriate vsync mode on startup so a user
    // who saved Remote as their source gets the display-refresh-aligned
    // playback path immediately instead of only after the next source
    // change. setOnSourceTypeChanged handles all subsequent transitions.
    if (m_window && m_ui)
    {
        m_window->setVsync(m_ui->getSourceType() == UIManager::SourceType::Remote);
    }

    LOG_INFO("Application initialized successfully");
    return true;
}

void Application::updateCursorVisibility()
{
    // Keep the OS cursor visible whenever ANY interactive surface is on
    // screen — the full UI OR an interactive OSD (#68). Hiding it while
    // the quick-actions widget is showing would let the user click the
    // buttons but with no visible pointer, which feels broken even when
    // the underlying clicks register.
    if (m_ui && m_window)
    {
        const bool osdInteractive =
            m_ui->getQuickActionsOverlay() &&
            m_ui->getQuickActionsOverlay()->isVisible();
        m_window->setCursorVisible(m_ui->isVisible() || osdInteractive);
    }
}

bool Application::initWindow()
{
#ifdef USE_SDL2
    m_window = std::make_unique<WindowManagerSDL>();
#else
    m_window = std::make_unique<WindowManager>();
#endif

    WindowConfig config;
    config.width = m_windowWidth;
    config.height = m_windowHeight;
    config.title = "RetroCapture";
    config.fullscreen = m_fullscreen;
    config.monitorIndex = m_monitorIndex;
    // IMPORTANT: Disable VSync to avoid blocking when window is not focused
    // VSync can cause application pause when window is in background
    // This ensures capture and streaming continue working even when not focused
    config.vsync = false;

    if (!m_window->init(config))
    {
        LOG_ERROR("Failed to initialize window");
        m_window.reset();
        return false;
    }

    m_window->makeCurrent();

    // Store Application pointer in WindowManager for use in callbacks
    m_window->setUserData(this);

    // Configure resize callback to update viewport when window is resized
    // or enters fullscreen
    // IMPORTANT: This callback is called by GLFW when window changes size,
    // including when entering fullscreen
    // IMPORTANT: ShaderEngine is not yet initialized here, so we'll update
    // the callback after ShaderEngine is created
    // For now, we'll just store the pointer

    return true;
}

bool Application::initRenderer()
{
    LOG_INFO("Initializing renderer...");
    // Ensure OpenGL context is active
    if (m_window)
    {
        LOG_INFO("Making OpenGL context current...");
        m_window->makeCurrent();
        LOG_INFO("OpenGL context activated");
    }
    else
    {
        LOG_ERROR("Window not available to initialize renderer");
        return false;
    }

    LOG_INFO("Creating OpenGLRenderer...");
    m_renderer = std::make_unique<OpenGLRenderer>();
    LOG_INFO("OpenGLRenderer created");

    LOG_INFO("Initializing OpenGLRenderer...");
    if (!m_renderer->init())
    {
        LOG_ERROR("Failed to initialize renderer");
        m_renderer.reset();
        return false;
    }
    LOG_INFO("OpenGLRenderer initialized");

    // Initialize FrameProcessor
    LOG_INFO("Creating FrameProcessor...");
    m_frameProcessor = std::make_unique<FrameProcessor>();
    m_frameProcessor->init(m_renderer.get());
    // Aplicar configuração de texture filtering
    m_frameProcessor->setTextureFilterLinear(m_textureFilterLinear);
    LOG_INFO("FrameProcessor created");

    // Initialize PBO Manager for async pixel reading
    LOG_INFO("Creating PBOManager...");
    m_pboManager = std::make_unique<PBOManager>();
    LOG_INFO("PBOManager created");

    // Initialize ShaderEngine
    LOG_INFO("Creating ShaderEngine...");
    m_shaderEngine = std::make_unique<ShaderEngine>();
    LOG_INFO("ShaderEngine created, initializing...");
    if (!m_shaderEngine->init())
    {
        LOG_ERROR("Failed to initialize ShaderEngine");
        m_shaderEngine.reset();
        // Not critical, we can continue without shaders
    }
    else
    {
        LOG_INFO("ShaderEngine initialized");
        // IMPORTANT: Update ShaderEngine viewport with current window dimensions
        // This is especially important when window is created in fullscreen
        // The resize callback may not be called immediately on creation
        if (m_window)
        {
            uint32_t currentWidth = m_window->getWidth();
            uint32_t currentHeight = m_window->getHeight();
            m_shaderEngine->setViewport(currentWidth, currentHeight);
        }

        // IMPORTANT: Now that ShaderEngine is initialized, configure resize callback
        // to update viewport when window is resized or enters fullscreen
        if (m_window)
        {
            Application *appPtr = this;
            m_window->setResizeCallback([appPtr](int width, int height)
                                        {
                // IMPORTANT: Update ShaderEngine viewport immediately when resize happens
                // This is especially critical when entering fullscreen
                // IMPORTANT: Validate dimensions before updating to avoid issues
                if (appPtr && appPtr->m_shaderEngine && width > 0 && height > 0 && 
                    width <= 7680 && height <= 4320) {
                    appPtr->m_isResizing = true;
                    {
                        std::lock_guard<std::mutex> lock(appPtr->m_resizeMutex);
                        appPtr->m_shaderEngine->setViewport(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                    }
// Small delay to ensure ShaderEngine finished recreating framebuffers
#ifdef PLATFORM_LINUX
                    usleep(10000); // 10ms
#else
                    Sleep(10); // 10ms
#endif
                    appPtr->m_isResizing = false;
                } });
        }

        // Load shader or preset if specified
        if (!m_presetPath.empty())
        {
            if (m_shaderEngine->loadPreset(m_presetPath))
            {
                LOG_INFO("Preset loaded: " + m_presetPath);
            }
            else
            {
                LOG_ERROR("Failed to load preset: " + m_presetPath);
            }
        }
        else if (!m_shaderPath.empty())
        {
            if (m_shaderEngine->loadShader(m_shaderPath))
            {
                LOG_INFO("Shader loaded: " + m_shaderPath);
            }
            else
            {
                LOG_ERROR("Failed to load shader: " + m_shaderPath);
            }
        }
    }

    return true;
}

bool Application::initCapture()
{
    LOG_INFO("Creating VideoCapture...");

    // Phase 3 of #47: if the UIManager already has the source type set to
    // Remote (CLI selected --source remote), build the remote capture
    // instead of the platform-default V4L2 / DirectShow factory.
    if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote)
    {
        LOG_INFO("Source is remote — creating VideoCaptureRemote");
        auto remote = std::make_unique<VideoCaptureRemote>();
        VideoCaptureRemote::InterpolationMode imode = VideoCaptureRemote::InterpolationMode::Linear;
        if (m_remoteInterpolation == "nearest") imode = VideoCaptureRemote::InterpolationMode::Nearest;
        else if (m_remoteInterpolation == "off") imode = VideoCaptureRemote::InterpolationMode::Off;
        remote->setInterpolationMode(imode);
        m_capture = std::move(remote);
    }
    else
    {
        m_capture = VideoCaptureFactory::create();
    }
    if (!m_capture)
    {
        LOG_ERROR("Failed to create VideoCapture for this platform");
        return false;
    }
    LOG_INFO("VideoCapture created successfully");

    // Try to open specified device
    // On Windows, m_devicePath can be empty or be a MF device index
    // On Linux, m_devicePath is the V4L2 device path (e.g., /dev/video0)
    // If it fails, activate dummy mode (generates black frames)
    if (m_devicePath.empty())
    {
#ifdef _WIN32
        LOG_INFO("No device specified - activating dummy mode directly");
        // Go directly to dummy mode without trying to open device
        m_capture->setDummyMode(true);
        if (!m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
        {
            LOG_ERROR("Failed to configure dummy format");
            return false;
        }
        if (!m_capture->startCapture())
        {
            LOG_ERROR("Failed to start dummy capture");
            return false;
        }
        LOG_INFO("Dummy mode activated: " + std::to_string(m_capture->getWidth()) + "x" +
                 std::to_string(m_capture->getHeight()));
        return true;
#else
        LOG_INFO("No device specified - using default /dev/video0");
        m_devicePath = "/dev/video0";
#endif
    }

    if (!m_capture->open(m_devicePath))
    {
        LOG_WARN("Failed to open capture device: " + (m_devicePath.empty() ? "(none)" : m_devicePath));

        // Phase 5b/#47: Remote source has no "dummy fallback" — a remote
        // stream is either reachable or not. Falling back to dummy here is
        // what made a failed `--source remote` look like a silent green
        // screen. Leave m_capture closed and surface the state via the UI;
        // the user can re-enter a URL and click Connect, which fires the
        // OnDeviceChanged handler and retries.
        if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote)
        {
            LOG_INFO("Remote source could not connect. Enter a base URL in the Source tab and click Connect.");
            if (m_ui)
            {
                m_ui->setCaptureInfo(0, 0, 0, "Remote (not connected)");
            }
            return true; // Not a fatal initialization error.
        }

        LOG_INFO("Activating dummy mode: generating black frames at specified resolution.");
#ifdef __linux__
        LOG_INFO("Select a device in the V4L2 tab to use real capture.");
#elif defined(_WIN32)
        LOG_INFO("Select a device in the DirectShow tab to use real capture.");
#endif

        // Activate dummy mode
        m_capture->setDummyMode(true);

        // Configure dummy format with default resolution
        // Note: V4L2_PIX_FMT_YUYV is V4L2-specific, but interface accepts 0 for default
        if (!m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
        {
            LOG_ERROR("Failed to configure dummy format");
            return false;
        }

        // Start dummy capture
        if (!m_capture->startCapture())
        {
            LOG_ERROR("Failed to start dummy capture");
            return false;
        }

        LOG_INFO("Dummy mode activated: " + std::to_string(m_capture->getWidth()) + "x" +
                 std::to_string(m_capture->getHeight()));
        return true;
    }

    // Configure format with configurable parameters
    LOG_INFO("Configuring capture: " + std::to_string(m_captureWidth) + "x" +
             std::to_string(m_captureHeight) + " @ " + std::to_string(m_captureFps) + "fps");

    if (!m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
    {
        LOG_ERROR("Failed to configure capture format");
        LOG_WARN("Requested resolution may not be supported by device");

        // If not in dummy mode, try to activate dummy mode as fallback
        if (!m_capture->isDummyMode())
        {
            LOG_INFO("Attempting to activate dummy mode as fallback...");
            m_capture->close();
            m_capture->setDummyMode(true);

            if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
            {
                if (m_capture->startCapture())
                {
                    LOG_INFO("Dummy mode activated as fallback: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                    return true;
                }
            }
        }

        m_capture->close();
        // Don't reset m_capture - allow trying again later
        LOG_INFO("Device closed. Select another device in the V4L2 tab.");
        return true; // Continue without device
    }

    // Try to configure framerate (not critical if it fails)
    // In dummy mode, this only logs but doesn't configure real device
    if (!m_capture->setFramerate(m_captureFps))
    {
        if (!m_capture->isDummyMode())
        {
            LOG_WARN("Could not configure framerate to " + std::to_string(m_captureFps) + "fps");
            LOG_INFO("Using device default framerate");
        }
    }

    // Configure hardware controls if specified (using generic interface)
    if (m_v4l2Brightness >= 0)
    {
        m_capture->setControl("Brightness", m_v4l2Brightness);
    }
    if (m_v4l2Contrast >= 0)
    {
        m_capture->setControl("Contrast", m_v4l2Contrast);
    }
    if (m_v4l2Saturation >= 0)
    {
        if (m_capture->setControl("Saturation", m_v4l2Saturation))
        {
            LOG_INFO("Saturation configured: " + std::to_string(m_v4l2Saturation));
        }
    }
    if (m_v4l2Hue >= 0)
    {
        m_capture->setControl("Hue", m_v4l2Hue);
    }
    if (m_v4l2Gain >= 0)
    {
        m_capture->setControl("Gain", m_v4l2Gain);
    }
    if (m_v4l2Exposure >= 0)
    {
        if (m_capture->setControl("Exposure", m_v4l2Exposure))
        {
            LOG_INFO("Exposure configured: " + std::to_string(m_v4l2Exposure));
        }
    }
    if (m_v4l2Sharpness >= 0)
    {
        if (m_capture->setControl("Sharpness", m_v4l2Sharpness))
        {
            LOG_INFO("Sharpness configured: " + std::to_string(m_v4l2Sharpness));
        }
    }
    if (m_v4l2Gamma >= 0)
    {
        m_capture->setControl("Gamma", m_v4l2Gamma);
    }
    if (m_v4l2WhiteBalance >= 0)
    {
        m_capture->setControl("White Balance", m_v4l2WhiteBalance);
    }

    if (!m_capture->startCapture())
    {
        LOG_ERROR("Failed to start capture");

        // If not in dummy mode, try to activate dummy mode as fallback
        if (!m_capture->isDummyMode())
        {
            LOG_INFO("Attempting to activate dummy mode as fallback...");
            m_capture->close();
            m_capture->setDummyMode(true);

            if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
            {
                if (m_capture->startCapture())
                {
                    LOG_INFO("Dummy mode activated as fallback: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                    return true;
                }
            }
        }

        m_capture->close();
        // Don't reset m_capture - allow trying again later
        LOG_INFO("Device closed. Select another device in the V4L2 tab.");
        return true; // Continue without device
    }

    // Only log dimensions if device is open
    if (m_capture->isOpen())
    {
        LOG_INFO("Capture initialized: " +
                 std::to_string(m_capture->getWidth()) + "x" +
                 std::to_string(m_capture->getHeight()));

        // Sync m_captureWidth / m_captureHeight with what the device
        // actually produces. For V4L2 / DS this is usually a no-op (we
        // already asked for those dims via setFormat); for Remote the
        // server picks the dimensions, and without this sync the render
        // pipeline keeps using the local config defaults — leaving the
        // image filling only a fraction of the window.
        const uint32_t actualW = m_capture->getWidth();
        const uint32_t actualH = m_capture->getHeight();
        if (actualW > 0 && actualH > 0 && (actualW != m_captureWidth || actualH != m_captureHeight))
        {
            LOG_INFO("Adjusting capture dims to actual " +
                     std::to_string(actualW) + "x" + std::to_string(actualH) +
                     " (was " + std::to_string(m_captureWidth) + "x" + std::to_string(m_captureHeight) + ")");
            m_captureWidth  = actualW;
            m_captureHeight = actualH;
        }
    }

    // Phase 4 of #47: when source is Remote, also poll the host's /meta to
    // mirror the active shader + parameters locally. Snapshot deltas are
    // staged on m_pendingRemote* and consumed on the GL thread inside the
    // main loop (see applyPendingRemoteMeta()).
    if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote && !m_devicePath.empty())
    {
        m_remoteMetaSync = std::make_unique<RemoteMetaSync>();
        m_remoteMetaSync->start(m_devicePath,
            [this](const RemoteMetaSync::Snapshot &snap)
            {
                std::lock_guard<std::mutex> lock(m_pendingRemoteMutex);
                m_pendingRemotePreset          = snap.preset;
                m_pendingRemotePresetHash      = snap.presetHash;
                m_pendingRemotePipelineEnabled = snap.pipelineEnabled;
                m_pendingRemoteParams.clear();
                m_pendingRemoteParams.reserve(snap.parameters.size());
                for (const auto &p : snap.parameters)
                {
                    m_pendingRemoteParams.emplace_back(p.name, p.value);
                }
                m_pendingRemoteSourceWidth  = snap.sourceWidth;
                m_pendingRemoteSourceHeight = snap.sourceHeight;
                // Image-tab values: seed only once per connection — see
                // applyPendingRemoteMeta for the gate. Stash the values
                // from this snapshot; if it's the first one we'll apply
                // them on the GL thread, otherwise the apply gate skips.
                if (!m_remoteImageSeeded)
                {
                    m_pendingRemoteImageBrightness     = snap.imageBrightness;
                    m_pendingRemoteImageContrast       = snap.imageContrast;
                    m_pendingRemoteImageMaintainAspect = snap.imageMaintainAspect;
                    m_pendingRemoteImageOutputWidth    = snap.imageOutputWidth;
                    m_pendingRemoteImageOutputHeight   = snap.imageOutputHeight;
                    m_hasPendingRemoteImageSeed        = true;
                }
                // Plain assignment — uint32_t is atomic on every platform
                // we target, and a momentarily stale read in the main
                // loop is harmless (just one extra render iteration at
                // most).
                if (snap.sourceFps > 0) m_remoteSourceFps = snap.sourceFps;
                m_hasPendingRemoteMeta.store(true);
                // Push the host's viewer count straight onto UIManager
                // (#68) — the OSD quick-actions widget reads it every
                // frame to render "watching with N others" in client
                // mode. Bypasses the pending-snapshot apply path
                // because there's no GL state involved.
                if (m_ui) m_ui->setRemoteUpstreamClientCount(snap.upstreamClientCount);
            });
    }

    return true;
}

bool Application::reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps)
{
    if (!m_capture || !m_capture->isOpen())
    {
        LOG_ERROR("Capture is not open, cannot reconfigure");
        return false;
    }

    LOG_INFO("Reconfiguring capture: " + std::to_string(width) + "x" +
             std::to_string(height) + " @ " + std::to_string(fps) + "fps");

    // Guardamos a resolução lógica (o que o usuário pediu) antes do V4L2
    // possivelmente ajustar pra mais próxima suportada pelo dispositivo.
    m_logicalCaptureWidth = width;
    m_logicalCaptureHeight = height;

    // Set reconfiguration flag to prevent frame processing
    m_isReconfiguring = true;

    // Small delay to ensure any ongoing frame processing completes
    // This prevents race conditions where processFrame is accessing the device
#ifdef PLATFORM_LINUX
    usleep(50000); // 50ms to let current frame processing finish
#else
    Sleep(50); // 50ms to let current frame processing finish
#endif

    // Delete texture BEFORE closing device to avoid accessing invalid resources
    if (m_frameProcessor)
    {
        m_frameProcessor->deleteTexture();
    }

    // Save current values for rollback if needed
    uint32_t oldWidth = m_captureWidth;
    uint32_t oldHeight = m_captureHeight;
    uint32_t oldFps = m_captureFps;
    std::string devicePath = m_devicePath;

    // IMPORTANT: Close and reopen device completely
    // This is necessary because some V4L2 drivers don't allow changing
    // resolution without closing the device
    LOG_INFO("Closing device for reconfiguration...");
    m_capture->stopCapture();
    m_capture->close();

// Small delay to ensure device was released
#ifdef PLATFORM_LINUX
    usleep(100000); // 100ms
#else
    Sleep(100); // 100ms
#endif

    // Reopen device
    LOG_INFO("Reopening device...");
    if (!m_capture->open(devicePath))
    {
        LOG_ERROR("Failed to reopen device after reconfiguration");
        m_isReconfiguring = false; // Reset flag on failure
        return false;
    }

    // Configure new format
    if (!m_capture->setFormat(width, height, 0))
    {
        LOG_ERROR("Failed to configure new capture format");
        // Try rollback: reopen with previous format
        m_capture->close();
#ifdef PLATFORM_LINUX
        usleep(100000); // 100ms
#else
        Sleep(100); // 100ms
#endif
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, 0);
            m_capture->setFramerate(oldFps);
            m_capture->startCapture();
        }
        // Reset reconfiguration flag on failure
        m_isReconfiguring = false;
        return false;
    }

    // Get actual dimensions (driver may have adjusted)
    uint32_t actualWidth = m_capture->getWidth();
    uint32_t actualHeight = m_capture->getHeight();

    // Configure framerate
    if (!m_capture->setFramerate(fps))
    {
        LOG_WARN("Could not configure framerate to " + std::to_string(fps) + "fps");
        LOG_INFO("Using device default framerate");
    }

    // Restart capture (this creates buffers with new format)
    if (!m_capture->startCapture())
    {
        LOG_ERROR("Failed to restart capture after reconfiguration");
        // Tentar rollback
        m_capture->stopCapture();
        m_capture->close();
#ifdef PLATFORM_LINUX
        usleep(100000); // 100ms
#else
        Sleep(100); // 100ms
#endif
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, 0);
            m_capture->setFramerate(oldFps);
            m_capture->startCapture();
        }
        m_isReconfiguring = false; // Reset flag on failure
        return false;
    }

    // Update internal dimensions with actual values
    m_captureWidth = actualWidth;
    m_captureHeight = actualHeight;
    m_captureFps = fps;

    // IMPORTANT: Discard some initial frames after reconfiguration
    // First frames may have old or invalid data
    // This ensures that when main loop tries to process, we already have valid frames
    LOG_INFO("Discarding initial frames after reconfiguration...");
    Frame dummyFrame;
    for (int i = 0; i < 5; ++i)
    {
        m_capture->captureLatestFrame(dummyFrame);
#ifdef PLATFORM_LINUX
        usleep(10000); // 10ms entre tentativas
#else
        Sleep(10); // 10ms entre tentativas
#endif
    }

    LOG_INFO("Capture reconfigured successfully: " +
             std::to_string(actualWidth) + "x" +
             std::to_string(actualHeight) + " @ " + std::to_string(fps) + "fps");

    // Reset reconfiguration flag
    m_isReconfiguring = false;

    return true;
}

bool Application::initUI()
{
    // IMPORTANT: Ensure OpenGL context is active before initializing ImGui
    // ImGui needs a valid OpenGL context to initialize correctly
    if (m_window)
    {
        m_window->makeCurrent();
    }
    else
    {
        LOG_ERROR("Window not available to initialize UI");
        return false;
    }

    m_ui = std::make_unique<UIManager>();

    // Get window pointer from WindowManager (GLFW or SDL2)
    void *window = m_window->getWindow();
    if (!window)
    {
        LOG_ERROR("Failed to get window pointer for ImGui");
        m_ui.reset();
        return false;
    }

    if (!m_ui->init(window))
    {
        LOG_ERROR("Failed to initialize UIManager");
        m_ui.reset();
        return false;
    }

    // Configure callbacks
    m_ui->setOnVisibilityChanged([this](bool /* visible */)
                                  {
        updateCursorVisibility();
    });

    // Set initial cursor visibility based on UI visibility
    // This ensures cursor is hidden if UI starts hidden (e.g., --hide-ui flag)
    updateCursorVisibility();

    m_ui->setOnShaderChanged([this](const std::string &shaderPath)
                             {
        if (!m_shaderEngine) return;

        if (shaderPath.empty()) {
            m_shaderEngine->disableShader();
            LOG_INFO("Shader disabled");
            return;
        }

        std::string fullPath = resolveShaderPath(shaderPath);

        // Idempotência: se o shader pedido já é o ativo, não recarrega.
        // `applyPreset` aplica params custom *antes* de sincronizar a UI;
        // recarregar aqui zeraria m_customParameters do ShaderEngine
        // (vide ShaderEngine::loadPreset) e os overrides do capture preset
        // se perderiam silenciosamente.
        if (m_shaderEngine->isShaderActive() &&
            m_shaderEngine->getPresetPath() == fullPath) {
            return;
        }

        if (m_shaderEngine->loadPreset(fullPath)) {
            LOG_INFO("Shader loaded via UI: " + shaderPath);
        } else {
            LOG_ERROR("Failed to load shader via UI: " + shaderPath);
        } });

    m_ui->setOnBrightnessChanged([this](float brightness)
                                 { m_brightness = brightness; });

    m_ui->setOnContrastChanged([this](float contrast)
                               { m_contrast = contrast; });

    m_ui->setOnMaintainAspectChanged([this](bool maintain)
                                     { m_maintainAspect = maintain; });

    m_ui->setOnFullscreenChanged([this](bool fullscreen)
                                 {
        LOG_INFO("Fullscreen toggle requested: " + std::string(fullscreen ? "ON" : "OFF"));
        // IMPORTANT: Make fullscreen change asynchronously to avoid freezing
        // The resize callback will be called automatically by GLFW when window changes
        // Don't do blocking operations here
        if (m_window) {
            m_fullscreen = fullscreen;
            // The fullscreen change will be done in the next frame of main loop
            // to avoid deadlocks and freezing
            m_pendingFullscreenChange = true;
        } });

    m_ui->setOnMonitorIndexChanged([this](int monitorIndex)
                                   {
        LOG_INFO("Monitor index changed: " + std::to_string(monitorIndex));
        m_monitorIndex = monitorIndex;
        // If in fullscreen, update to use new monitor
        if (m_fullscreen && m_window) {
            m_window->setFullscreen(true, monitorIndex);
            
            // Update shader engine viewport after monitor change
            if (m_shaderEngine) {
                uint32_t currentWidth = m_window->getWidth();
                uint32_t currentHeight = m_window->getHeight();
                m_shaderEngine->setViewport(currentWidth, currentHeight);
            }
        } });

    m_ui->setOnOutputResolutionChanged([this](uint32_t width, uint32_t height)
                                       {
        LOG_INFO("Output resolution changed: " + std::to_string(width) + "x" + std::to_string(height) + 
                 (width == 0 && height == 0 ? " (automatic)" : ""));
        m_outputWidth = width;
        m_outputHeight = height; });

    m_ui->setOnV4L2ControlChanged([this](const std::string &name, int32_t value)
                                  {
        if (!m_capture) return;
        
        // Use generic interface to set control
        int32_t minVal, maxVal;
        if (m_capture->getControlMin(name, minVal) && 
            m_capture->getControlMax(name, maxVal)) {
            // Clamp ao range
            value = std::max(minVal, std::min(maxVal, value));
        }
        
        m_capture->setControl(name, value); });

    m_ui->setOnResolutionChanged([this](uint32_t width, uint32_t height)
                                 {
        // Schedule resolution change for main thread (thread-safe)
        // This is necessary because the callback may be called from API threads
        scheduleResolutionChange(width, height); });

    m_ui->setOnFramerateChanged([this](uint32_t fps)
                                {
        LOG_INFO("Framerate changed via UI: " + std::to_string(fps) + "fps");
        // Update FPS in configuration
        m_captureFps = fps;
        
        // If no device is open, just update configuration (dummy mode doesn't need reconfiguration)
        if (!m_capture || !m_capture->isOpen()) {
            if (m_capture && m_capture->isDummyMode()) {
                LOG_INFO("Framerate updated for dummy mode: " + std::to_string(fps) + "fps");
            } else {
                LOG_WARN("No device open. FPS will be applied when a device is selected.");
            }
            return;
        }
        if (reconfigureCapture(m_captureWidth, m_captureHeight, fps)) {
            m_captureFps = fps;
            // Update UI information
            if (m_ui && m_capture) {
                m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                    m_captureFps, m_devicePath);
            }
        } });

    // IMPORTANT: UIManager has already loaded saved configurations in its constructor
    // So we should read FROM UI first, then set callbacks
    // This ensures saved values are not overwritten by default values

    // Read saved values from UI (loaded from config file)
    m_brightness = m_ui->getBrightness();
    m_contrast = m_ui->getContrast();
    m_maintainAspect = m_ui->getMaintainAspect();
    m_fullscreen = m_ui->getFullscreen();
    m_monitorIndex = m_ui->getMonitorIndex();

    // Read saved capture resolution from UI (loaded from config file)
    // The UIManager loads config in its constructor, so values are available here
    uint32_t savedWidth = m_ui->getCaptureWidth();
    uint32_t savedHeight = m_ui->getCaptureHeight();
    uint32_t savedFps = m_ui->getCaptureFps();
    std::string savedDevice = m_ui->getCurrentDevice();

    // Use saved values if they exist (savedWidth/Height > 0 means config was loaded)
    // Only override if we have valid saved values
    bool useSavedResolution = (savedWidth > 0 && savedHeight > 0);

    // Check if current values are the defaults (1920x1080) - if so, likely not set via command line
    bool isDefaultResolution = (m_captureWidth == 1920 && m_captureHeight == 1080);

    if (useSavedResolution && ((m_captureWidth == 0 && m_captureHeight == 0) || isDefaultResolution))
    {
        LOG_INFO("Using saved capture resolution: " +
                 std::to_string(savedWidth) + "x" + std::to_string(savedHeight) +
                 " @ " + std::to_string(savedFps) + "fps");
        m_captureWidth = savedWidth;
        m_captureHeight = savedHeight;
        if (savedFps > 0)
        {
            m_captureFps = savedFps;
        }

        // If capture is already initialized, reconfigure it with saved resolution
        if (m_capture && (m_capture->isOpen() || m_capture->isDummyMode()))
        {
            LOG_INFO("Reconfiguring capture with saved resolution...");
            if (m_capture->isDummyMode() || !m_capture->isOpen())
            {
                // For dummy mode or closed device, just reconfigure
                m_capture->stopCapture();
                m_capture->close();
                m_capture->setDummyMode(true);
                if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                {
                    m_capture->startCapture();
                    if (m_ui)
                    {
                        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                             m_captureFps, "None (Dummy)");
                    }
                }
            }
            else
            {
                // For real device, use reconfigureCapture
                reconfigureCapture(m_captureWidth, m_captureHeight, m_captureFps);
            }
        }
    }

    // Trocar pro dispositivo salvo se diferente do atual. initCapture já abriu
    // o default (/dev/video0 no Linux ou primeiro DirectShow no Windows). Só
    // mexemos se o config tem um valor não-vazio diferente.
    if (m_capture && !savedDevice.empty() && savedDevice != m_devicePath)
    {
        LOG_INFO("Switching to saved device: " + savedDevice);
        m_capture->stopCapture();
        m_capture->close();
        if (m_capture->open(savedDevice))
        {
            m_devicePath = savedDevice;
            if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
            {
                m_capture->setFramerate(m_captureFps);
                if (m_capture->startCapture())
                {
                    if (m_ui)
                    {
                        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                             m_captureFps, m_devicePath);
                        m_ui->setCurrentDevice(m_devicePath);
                    }
                    LOG_INFO("Saved device opened: " + savedDevice);
                }
            }
        }
        else
        {
            LOG_WARN("Failed to open saved device " + savedDevice + ", reverting to default.");
            // Reabre o default que estava antes — best-effort.
            if (m_capture->open(m_devicePath))
            {
                m_capture->setFormat(m_captureWidth, m_captureHeight, 0);
                m_capture->setFramerate(m_captureFps);
                m_capture->startCapture();
            }
        }
    }

    // Now set callbacks so future changes are synchronized
    // (Values are already set above, so this won't overwrite saved config)

    // Check initial source type and configure appropriately
    if (m_ui->getSourceType() == UIManager::SourceType::None)
    {
        // If None is selected, ensure dummy mode is active
        if (m_capture)
        {
            if (!m_capture->isDummyMode() || !m_capture->isOpen())
            {
                m_capture->stopCapture();
                m_capture->close();
                m_capture->setDummyMode(true);
                if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                {
                    if (m_capture->startCapture())
                    {
                        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                             m_captureFps, "None (Dummy)");
                    }
                }
            }
        }
        m_ui->setCaptureControls(nullptr);
    }
    else
    {
        // Configure V4L2 controls if device is open
        // IMPORTANT: Always pass m_capture to UIManager, even if not open,
        // to allow device enumeration (especially for DirectShow)
        if (m_capture)
        {
            m_ui->setCaptureControls(m_capture.get());
        }
        else
        {
            // Without capture, don't allow selection
            m_ui->setCaptureControls(nullptr);
        }
    }

    // Configure capture information
    if (m_capture && m_capture->isOpen())
    {
        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                             m_captureFps, m_devicePath);
        m_ui->setCurrentDevice(m_devicePath);
    }
    else
    {
        // No device - show "None"
        m_ui->setCaptureInfo(0, 0, 0, "None");
        m_ui->setCurrentDevice(""); // Empty string = None
    }

    // Connect Application to UICapturePresets
    if (m_ui->getCapturePresetsWindow())
    {
        m_ui->getCapturePresetsWindow()->setApplication(this);
    }

    if (m_ui->getRecordingsWindow())
    {
        m_ui->getRecordingsWindow()->setApplication(this);
    }

    // IMPORTANT: After init(), UIManager has already loaded saved configurations
    // Synchronize Application values with values loaded from UI
    // This ensures saved configurations are applied
    m_streamingPort = m_ui->getStreamingPort();
    m_streamingWidth = m_ui->getStreamingWidth();
    m_streamingHeight = m_ui->getStreamingHeight();
    m_streamingFps = m_ui->getStreamingFps();
    m_streamingBitrate = m_ui->getStreamingBitrate();
    m_streamingAudioBitrate = m_ui->getStreamingAudioBitrate();
    m_streamingVideoCodec = m_ui->getStreamingVideoCodec();
    m_streamingAudioCodec = m_ui->getStreamingAudioCodec();
    m_streamingH264Preset = m_ui->getStreamingH264Preset();
    m_streamingHardwareEncoder = m_ui->getStreamingHardwareEncoder();
    m_streamingNvencPreset = m_ui->getStreamingNvencPreset();
    m_streamingVaapiRcMode = m_ui->getStreamingVaapiRcMode();
    m_streamingQsvPreset   = m_ui->getStreamingQsvPreset();
    m_streamingAmfQuality  = m_ui->getStreamingAmfQuality();
    m_remoteInterpolation  = m_ui->getRemoteInterpolation();
    m_streamingH265Preset = m_ui->getStreamingH265Preset();
    m_streamingH265Profile = m_ui->getStreamingH265Profile();
    m_streamingH265Level = m_ui->getStreamingH265Level();
    m_streamingVP8Speed = m_ui->getStreamingVP8Speed();
    m_streamingVP9Speed = m_ui->getStreamingVP9Speed();

    // Load streaming buffer parameters
    m_streamingMaxVideoBufferSize = m_ui->getStreamingMaxVideoBufferSize();
    m_streamingMaxAudioBufferSize = m_ui->getStreamingMaxAudioBufferSize();
    m_streamingMaxBufferTimeSeconds = m_ui->getStreamingMaxBufferTimeSeconds();
    m_streamingAVIOBufferSize = m_ui->getStreamingAVIOBufferSize();

    // Load recording settings from UI
    if (m_recordingManager)
    {
        RecordingSettings settings;
        settings.width = m_ui->getRecordingWidth();
        settings.height = m_ui->getRecordingHeight();
        settings.fps = m_ui->getRecordingFps();
        settings.bitrate = m_ui->getRecordingBitrate();
        settings.codec = m_ui->getRecordingVideoCodec();
        settings.preset = (settings.codec == "h264") ? m_ui->getRecordingH264Preset() : m_ui->getRecordingH265Preset();
        settings.h265Profile = m_ui->getRecordingH265Profile();
        settings.h265Level = m_ui->getRecordingH265Level();
        settings.vp8Speed = m_ui->getRecordingVP8Speed();
        settings.vp9Speed = m_ui->getRecordingVP9Speed();
        settings.audioBitrate = m_ui->getRecordingAudioBitrate();
        settings.audioCodec = m_ui->getRecordingAudioCodec();
        settings.container = m_ui->getRecordingContainer();
        settings.outputPath = m_ui->getRecordingOutputPath();
        settings.filenameTemplate = m_ui->getRecordingFilenameTemplate();
        settings.includeAudio = m_ui->getRecordingIncludeAudio();
        // Hardware encoder + backend-specific preset (#59) — resolved
        // from the UI's per-backend preset fields based on the user's
        // selected backend. Auto/Software leave hwPreset empty so
        // MediaEncoder uses its compiled-in default.
        settings.hardwareEncoder = m_ui->getRecordingHardwareEncoder();
        switch (settings.hardwareEncoder)
        {
            case 2: settings.hwPreset = m_ui->getRecordingNvencPreset(); break; // NVENC
            case 3: settings.hwPreset = m_ui->getRecordingVaapiRcMode(); break; // VAAPI
            case 4: settings.hwPreset = m_ui->getRecordingQsvPreset();   break; // QSV
            case 5: settings.hwPreset = m_ui->getRecordingAmfQuality();  break; // AMF
            default: settings.hwPreset.clear(); break;                          // Auto / Software
        }
        m_recordingManager->setRecordingSettings(settings);
    }

    // Load buffer settings (already loaded by UIManager from config file)

    // Load Web Portal settings
    m_webPortalEnabled = m_ui->getWebPortalEnabled();
    m_webPortalHTTPSEnabled = m_ui->getWebPortalHTTPSEnabled();
    m_webPortalSSLCertPath = m_ui->getWebPortalSSLCertPath();
    m_webPortalSSLKeyPath = m_ui->getWebPortalSSLKeyPath();
    m_webPortalTitle = m_ui->getWebPortalTitle();
    m_webPortalSubtitle = m_ui->getWebPortalSubtitle();
    m_webPortalImagePath = m_ui->getWebPortalImagePath();
    m_webPortalBackgroundImagePath = m_ui->getWebPortalBackgroundImagePath();

    // Load editable texts
    m_webPortalTextStreamInfo = m_ui->getWebPortalTextStreamInfo();
    m_webPortalTextQuickActions = m_ui->getWebPortalTextQuickActions();
    m_webPortalTextCompatibility = m_ui->getWebPortalTextCompatibility();
    m_webPortalTextStatus = m_ui->getWebPortalTextStatus();
    m_webPortalTextCodec = m_ui->getWebPortalTextCodec();
    m_webPortalTextResolution = m_ui->getWebPortalTextResolution();
    m_webPortalTextStreamUrl = m_ui->getWebPortalTextStreamUrl();
    m_webPortalTextCopyUrl = m_ui->getWebPortalTextCopyUrl();
    m_webPortalTextOpenNewTab = m_ui->getWebPortalTextOpenNewTab();
    m_webPortalTextSupported = m_ui->getWebPortalTextSupported();
    m_webPortalTextFormat = m_ui->getWebPortalTextFormat();
    m_webPortalTextCodecInfo = m_ui->getWebPortalTextCodecInfo();
    m_webPortalTextSupportedBrowsers = m_ui->getWebPortalTextSupportedBrowsers();
    m_webPortalTextFormatInfo = m_ui->getWebPortalTextFormatInfo();
    m_webPortalTextCodecInfoValue = m_ui->getWebPortalTextCodecInfoValue();
    m_webPortalTextConnecting = m_ui->getWebPortalTextConnecting();

    // Load colors (with safety check)
    const float *bg = m_ui->getWebPortalColorBackground();
    if (bg)
    {
        memcpy(m_webPortalColorBackground, bg, 4 * sizeof(float));
    }
    const float *txt = m_ui->getWebPortalColorText();
    if (txt)
    {
        memcpy(m_webPortalColorText, txt, 4 * sizeof(float));
    }
    const float *prim = m_ui->getWebPortalColorPrimary();
    if (prim)
    {
        memcpy(m_webPortalColorPrimary, prim, 4 * sizeof(float));
    }
    const float *primLight = m_ui->getWebPortalColorPrimaryLight();
    if (primLight)
    {
        memcpy(m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
    }
    const float *primDark = m_ui->getWebPortalColorPrimaryDark();
    if (primDark)
    {
        memcpy(m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
    }
    const float *sec = m_ui->getWebPortalColorSecondary();
    if (sec)
    {
        memcpy(m_webPortalColorSecondary, sec, 4 * sizeof(float));
    }
    const float *secHighlight = m_ui->getWebPortalColorSecondaryHighlight();
    if (secHighlight)
    {
        memcpy(m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
    }
    const float *ch = m_ui->getWebPortalColorCardHeader();
    if (ch)
    {
        memcpy(m_webPortalColorCardHeader, ch, 4 * sizeof(float));
    }
    const float *bord = m_ui->getWebPortalColorBorder();
    if (bord)
    {
        memcpy(m_webPortalColorBorder, bord, 4 * sizeof(float));
    }
    const float *succ = m_ui->getWebPortalColorSuccess();
    if (succ)
    {
        memcpy(m_webPortalColorSuccess, succ, 4 * sizeof(float));
    }
    const float *warn = m_ui->getWebPortalColorWarning();
    if (warn)
    {
        memcpy(m_webPortalColorWarning, warn, 4 * sizeof(float));
    }
    const float *dang = m_ui->getWebPortalColorDanger();
    if (dang)
    {
        memcpy(m_webPortalColorDanger, dang, 4 * sizeof(float));
    }
    const float *inf = m_ui->getWebPortalColorInfo();
    if (inf)
    {
        memcpy(m_webPortalColorInfo, inf, 4 * sizeof(float));
    }

    // Also synchronize image settings
    m_brightness = m_ui->getBrightness();
    m_contrast = m_ui->getContrast();
    m_maintainAspect = m_ui->getMaintainAspect();
    m_fullscreen = m_ui->getFullscreen();
    m_monitorIndex = m_ui->getMonitorIndex();

    // Apply loaded shader if available
    std::string loadedShader = m_ui->getCurrentShader();
    if (!loadedShader.empty() && m_shaderEngine)
    {
        // Use centralized shader path resolution
        std::string fullPath = resolveShaderPath(loadedShader);
        if (m_shaderEngine->loadPreset(fullPath))
        {
            LOG_INFO("Shader loaded from configuration: " + loadedShader);
        }
    }

    // Apply image settings
    // FrameProcessor applies brightness/contrast during processing, no need to set here

    // Apply fullscreen if needed
    if (m_fullscreen && m_window)
    {
        m_window->setFullscreen(m_fullscreen, m_monitorIndex);
    }

    m_ui->setOnStreamingStartStop([this](bool start)
                                  {
        // CRITICAL: This callback runs in main thread (ImGui render thread)
        // DO NOT do ANY blocking operations here - just set flag and create thread
        // DO NOT access m_streamManager or other shared resources here
        
        LOG_INFO("[CALLBACK] Streaming " + std::string(start ? "START" : "STOP") + " - creating thread...");
        
        if (start) {
            // Check if can start (not in cooldown)
            if (m_ui && !m_ui->canStartStreaming()) {
                int64_t cooldownMs = m_ui->getStreamingCooldownRemainingMs();
                int cooldownSeconds = static_cast<int>(cooldownMs / 1000);
                LOG_WARN("Streaming start attempt blocked - still in cooldown. Wait " + 
                         std::to_string(cooldownSeconds) + " seconds");
                if (m_ui) {
                    m_ui->setStreamingProcessing(false); // Resetar flag se bloqueado
                }
                return; // Don't start if in cooldown
            }
            
            // Just set flag - separate thread will do all the work
            m_streamingEnabled = true;
            
            // Update status immediately to "starting" (will be updated again when actually starting)
            if (m_ui) {
                m_ui->setStreamingActive(false); // Not active yet, but starting
            }
            
            // Create separate thread immediately - don't wait
            std::thread([this]() {
                // All blocking operations should be here, in the separate thread
                bool success = false;
                try {
                    if (initStreaming()) {
                        // Initialize audio capture (always required for streaming)
                        if (!m_audioCapture) {
                            if (!initAudioCapture()) {
                                LOG_WARN("Failed to initialize audio capture - continuing without audio");
                            }
                        }
                        success = true;
                    } else {
                        LOG_ERROR("Failed to start streaming");
                        m_streamingEnabled = false;
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception starting streaming: " + std::string(e.what()));
                    m_streamingEnabled = false;
                }
                
                // Update UI after initialization (can be called from any thread)
                // IMPORTANT: Check if m_streamManager exists before calling isActive()
                if (m_ui) {
                    bool active = success && m_streamManager && m_streamManager->isActive();
                    m_ui->setStreamingActive(active);
                    m_ui->setStreamingProcessing(false); // Processing completed
                    // initStreaming() calls stopWebPortal() before starting, which clears
                    // setWebPortalActive(false). When the streaming server itself hosts the
                    // portal (m_webPortalEnabled), it's effectively active again — reflect
                    // that back in the native UI so it doesn't read "Portal Web Inativo"
                    // while the portal is reachable through the stream port.
                    if (active && m_webPortalEnabled) {
                        m_ui->setWebPortalActive(true);
                    }
                }
            }).detach();
        } else {
            // Stop streaming also in separate thread to not block UI
            m_streamingEnabled = false;
            
            // Update status immediately when stopping
            if (m_ui) {
                m_ui->setStreamingActive(false);
                m_ui->setStreamUrl("");
                m_ui->setStreamClientCount(0);
            }
            
            // Create separate thread immediately - don't wait
            std::thread([this]() {
                try {
                    if (m_streamManager) {
                        // Ordem correta: stop() primeiro, depois cleanup()
                        m_streamManager->stop();
                        m_streamManager->cleanup();
                        m_streamManager.reset();
                        m_currentStreamer = nullptr; // Clear streamer reference
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception stopping streaming: " + std::string(e.what()));
                }
                
                // Ensure status is updated after stopping
                if (m_ui) {
                    m_ui->setStreamingActive(false);
                    m_ui->setStreamUrl("");
                    m_ui->setStreamClientCount(0);
                    m_ui->setStreamingProcessing(false); // Processing completed
                    // Streaming was hosting the portal — once it stops, the portal
                    // isn't actually reachable anymore. Mirror that in the UI so the
                    // "Portal Web Ativo" badge doesn't lie.
                    m_ui->setWebPortalActive(false);
                }

                // DO NOT restart web portal automatically when streaming stops.
                // Web portal can be enabled but doesn't necessarily need
                // to be running independently. If user wants portal active,
                // they can start it manually via UI.
                // Automatic restart caused problems when portal
                // was not active before streaming started.
            }).detach();
        }
        
        LOG_INFO("[CALLBACK] Thread criada, retornando (thread principal continua)"); });

    m_ui->setOnStreamingPortChanged([this](uint16_t port)
                                    {
        m_streamingPort = port;
        // If streaming is active, restart
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    // Resolution / FPS changes need the same restart-if-streaming policy
    // as bitrate/preset — initStreaming() bakes width/height/fps into the
    // MediaEncoder + MediaMuxer at construction time, so just storing the
    // new value on the field has no effect on a live stream.
    m_ui->setOnStreamingWidthChanged([this](uint32_t width) {
        m_streamingWidth = width;
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        }
    });

    m_ui->setOnStreamingHeightChanged([this](uint32_t height) {
        m_streamingHeight = height;
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        }
    });

    m_ui->setOnStreamingFpsChanged([this](uint32_t fps) {
        m_streamingFps = fps;
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        }
    });

    m_ui->setOnStreamingBitrateChanged([this](uint32_t bitrate)
                                       {
        m_streamingBitrate = bitrate;
        // Update streamer bitrate if active
        if (m_streamManager && m_streamManager->isActive()) {
            // Restart streaming with new bitrate
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingAudioBitrateChanged([this](uint32_t bitrate)
                                            {
        m_streamingAudioBitrate = bitrate;
        // If streaming is active, restart
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVideoCodecChanged([this](const std::string &codec)
                                          {
        m_streamingVideoCodec = codec;
        // If streaming is active, restart
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingAudioCodecChanged([this](const std::string &codec)
                                          {
        m_streamingAudioCodec = codec;
        // If streaming is active, restart
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH264PresetChanged([this](const std::string &preset)
                                          {
        m_streamingH264Preset = preset;
        // If streaming is active, restart to apply new preset
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265PresetChanged([this](const std::string &preset)
                                          {
        m_streamingH265Preset = preset;
        // If streaming is active, restart to apply new preset
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265ProfileChanged([this](const std::string &profile)
                                           {
        m_streamingH265Profile = profile;
        // If streaming is active, restart to apply new profile
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265LevelChanged([this](const std::string &level)
                                         {
        m_streamingH265Level = level;
        // If streaming is active, restart to apply new level
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVP8SpeedChanged([this](int speed)
                                        {
        m_streamingVP8Speed = speed;
        // If streaming is active, restart to apply new speed
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVP9SpeedChanged([this](int speed)
                                        {
        m_streamingVP9Speed = speed;
        // If streaming is active, restart to apply new speed
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingHardwareEncoderChanged([this](int v)
                                               {
        m_streamingHardwareEncoder = v;
        // Encoder backend is set at codec-init time inside MediaEncoder;
        // a live stream needs a restart for the new selection to take
        // effect (just like a preset / bitrate change does).
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    // Per-backend preset/quality strings — same restart-on-change
    // policy as above, since these all end up in opts that are passed
    // to avcodec_open2 at MediaEncoder::initializeHardwareVideoCodec.
    auto restartIfStreaming = [this] {
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        }
    };
    m_ui->setOnStreamingNvencPresetChanged([this, restartIfStreaming](const std::string &v) {
        m_streamingNvencPreset = v; restartIfStreaming();
    });
    m_ui->setOnStreamingVaapiRcModeChanged([this, restartIfStreaming](const std::string &v) {
        m_streamingVaapiRcMode = v; restartIfStreaming();
    });
    m_ui->setOnStreamingQsvPresetChanged([this, restartIfStreaming](const std::string &v) {
        m_streamingQsvPreset = v; restartIfStreaming();
    });
    m_ui->setOnStreamingAmfQualityChanged([this, restartIfStreaming](const std::string &v) {
        m_streamingAmfQuality = v; restartIfStreaming();
    });

    // Remote interpolation mode — applied immediately to the active
    // VideoCaptureRemote (if any). No streaming restart required since
    // it's a client-side display decision.
    m_ui->setOnRemoteInterpolationChanged([this](const std::string &v) {
        m_remoteInterpolation = v;
        if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_capture.get()))
        {
            VideoCaptureRemote::InterpolationMode mode = VideoCaptureRemote::InterpolationMode::Linear;
            if (v == "nearest") mode = VideoCaptureRemote::InterpolationMode::Nearest;
            else if (v == "off") mode = VideoCaptureRemote::InterpolationMode::Off;
            remote->setInterpolationMode(mode);
        }
    });

    // Callbacks for buffer settings
    m_ui->setOnStreamingMaxVideoBufferSizeChanged([this](size_t size)
                                                  {
        m_streamingMaxVideoBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingMaxAudioBufferSizeChanged([this](size_t size)
                                                  {
        m_streamingMaxAudioBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingMaxBufferTimeSecondsChanged([this](int64_t seconds)
                                                    {
        m_streamingMaxBufferTimeSeconds = seconds;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingAVIOBufferSizeChanged([this](size_t size)
                                              {
        m_streamingAVIOBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    // Recording callbacks
    m_ui->setOnRecordingStartStop([this](bool start)
                                  {
        if (start) {
            if (m_recordingManager) {
                RecordingSettings settings;
                // Use actual capture resolution and FPS, not UI settings
                // This ensures the recording matches what's being captured
                // CRITICAL: Use actual capture FPS to prevent video appearing sped up
                settings.width = m_captureWidth;
                settings.height = m_captureHeight;
                settings.fps = m_captureFps; // Use actual capture FPS, not UI setting
                settings.bitrate = m_ui->getRecordingBitrate();
                settings.codec = m_ui->getRecordingVideoCodec();
                settings.preset = (settings.codec == "h264") ? m_ui->getRecordingH264Preset() : m_ui->getRecordingH265Preset();
                settings.h265Profile = m_ui->getRecordingH265Profile();
                settings.h265Level = m_ui->getRecordingH265Level();
                settings.vp8Speed = m_ui->getRecordingVP8Speed();
                settings.vp9Speed = m_ui->getRecordingVP9Speed();
                settings.audioBitrate = m_ui->getRecordingAudioBitrate();
                settings.audioCodec = m_ui->getRecordingAudioCodec();
                settings.container = m_ui->getRecordingContainer();
                settings.outputPath = m_ui->getRecordingOutputPath();
                settings.filenameTemplate = m_ui->getRecordingFilenameTemplate();
                settings.includeAudio = m_ui->getRecordingIncludeAudio();
                settings.hardwareEncoder = m_ui->getRecordingHardwareEncoder();
                switch (settings.hardwareEncoder)
                {
                    case 2: settings.hwPreset = m_ui->getRecordingNvencPreset(); break;
                    case 3: settings.hwPreset = m_ui->getRecordingVaapiRcMode(); break;
                    case 4: settings.hwPreset = m_ui->getRecordingQsvPreset();   break;
                    case 5: settings.hwPreset = m_ui->getRecordingAmfQuality();  break;
                    default: settings.hwPreset.clear(); break;
                }

                if (!m_recordingManager) {
                    LOG_ERROR("Application: RecordingManager not initialized. Cannot start recording.");
                    m_ui->setRecordingActive(false);
                } else {
                    // Snapshot the shader/source state at click time so
                    // the recording's embedded metadata reflects exactly
                    // what was active when the session started (#59).
                    populateRecordingContext();
                    if (m_recordingManager->startRecording(settings)) {
                        LOG_INFO("Application: Recording started successfully");
                        m_ui->setRecordingActive(true);
                    } else {
                        LOG_ERROR("Application: Failed to start recording. Check logs for details.");
                        m_ui->setRecordingActive(false);
                    }
                }
            }
        } else {
            if (m_recordingManager) {
                m_recordingManager->stopRecording();
                m_ui->setRecordingActive(false);
            }
        } });

    m_ui->setOnRecordingWidthChanged([this](uint32_t width)
                                     { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.width = width;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingHeightChanged([this](uint32_t height)
                                      { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.height = height;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingFpsChanged([this](uint32_t fps)
                                   { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.fps = fps;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingBitrateChanged([this](uint32_t bitrate)
                                       { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.bitrate = bitrate;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingAudioBitrateChanged([this](uint32_t bitrate)
                                            { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.audioBitrate = bitrate;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingVideoCodecChanged([this](const std::string &codec)
                                          { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.codec = codec;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingAudioCodecChanged([this](const std::string &codec)
                                          { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.audioCodec = codec;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingH264PresetChanged([this](const std::string &preset)
                                          { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.preset = preset;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingH265PresetChanged([this](const std::string &preset)
                                          { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.preset = preset;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingH265ProfileChanged([this](const std::string &profile)
                                           { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.h265Profile = profile;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingH265LevelChanged([this](const std::string &level)
                                         { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.h265Level = level;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingVP8SpeedChanged([this](int speed)
                                        { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.vp8Speed = speed;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingVP9SpeedChanged([this](int speed)
                                        { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.vp9Speed = speed;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingContainerChanged([this](const std::string &container)
                                         { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.container = container;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingOutputPathChanged([this](const std::string &path)
                                          { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.outputPath = path;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingFilenameTemplateChanged([this](const std::string &template_)
                                                { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.filenameTemplate = template_;
            m_recordingManager->setRecordingSettings(settings);
        } });

    m_ui->setOnRecordingIncludeAudioChanged([this](bool include)
                                            { 
        if (m_recordingManager) {
            RecordingSettings settings = m_recordingManager->getRecordingSettings();
            settings.includeAudio = include;
            m_recordingManager->setRecordingSettings(settings);
        } });

    // Web Portal callbacks
    m_ui->setOnWebPortalEnabledChanged([this](bool enabled)
                                       {
        m_webPortalEnabled = enabled;
        // If Web Portal is disabled, also disable HTTPS
        if (!enabled && m_webPortalHTTPSEnabled) {
            m_webPortalHTTPSEnabled = false;
            // Update UI to reflect the change
            if (m_ui) {
                m_ui->setWebPortalHTTPSEnabled(false);
            }
        }
        // Update in real-time if streaming is active (without restarting)
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalEnabled(enabled);
        } });

    m_ui->setOnWebPortalHTTPSChanged([this](bool enabled)
                                     {
        m_webPortalHTTPSEnabled = enabled;
        // If streaming is active, restart to apply HTTPS
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalSSLCertPathChanged([this](const std::string &path)
                                           {
        m_webPortalSSLCertPath = path;
        // If streaming is active, restart to apply new certificate
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalSSLKeyPathChanged([this](const std::string &path)
                                          {
        m_webPortalSSLKeyPath = path;
        // If streaming is active, restart to apply new key
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalTitleChanged([this](const std::string &title)
                                     {
        m_webPortalTitle = title;
        // Update in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalTitle(title);
        } });

    m_ui->setOnWebPortalSubtitleChanged([this](const std::string &subtitle)
                                        {
        m_webPortalSubtitle = subtitle;
        // Update in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalSubtitle(subtitle);
        } });

    m_ui->setOnWebPortalImagePathChanged([this](const std::string &path)
                                         {
        m_webPortalImagePath = path;
        // Update in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalImagePath(path);
        } });

    m_ui->setOnWebPortalBackgroundImagePathChanged([this](const std::string &path)
                                                   {
        m_webPortalBackgroundImagePath = path;
        // Update in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalBackgroundImagePath(path);
        } });

    m_ui->setOnWebPortalColorsChanged([this]()
                                      {
        // Update colors in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager && m_ui) {
            // Synchronize colors from UI to Application (with safety check)
            const float* bg = m_ui->getWebPortalColorBackground();
            if (bg) {
                memcpy(m_webPortalColorBackground, bg, 4 * sizeof(float));
            }
            const float* txt = m_ui->getWebPortalColorText();
            if (txt) {
                memcpy(m_webPortalColorText, txt, 4 * sizeof(float));
            }
            const float* prim = m_ui->getWebPortalColorPrimary();
            if (prim) {
                memcpy(m_webPortalColorPrimary, prim, 4 * sizeof(float));
            }
            const float* primLight = m_ui->getWebPortalColorPrimaryLight();
            if (primLight) {
                memcpy(m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
            }
            const float* primDark = m_ui->getWebPortalColorPrimaryDark();
            if (primDark) {
                memcpy(m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
            }
            const float* sec = m_ui->getWebPortalColorSecondary();
            if (sec) {
                memcpy(m_webPortalColorSecondary, sec, 4 * sizeof(float));
            }
            const float* secHighlight = m_ui->getWebPortalColorSecondaryHighlight();
            if (secHighlight) {
                memcpy(m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
            }
            const float* ch = m_ui->getWebPortalColorCardHeader();
            if (ch) {
                memcpy(m_webPortalColorCardHeader, ch, 4 * sizeof(float));
            }
            const float* bord = m_ui->getWebPortalColorBorder();
            if (bord) {
                memcpy(m_webPortalColorBorder, bord, 4 * sizeof(float));
            }
            const float* succ = m_ui->getWebPortalColorSuccess();
            if (succ) {
                memcpy(m_webPortalColorSuccess, succ, 4 * sizeof(float));
            }
            const float* warn = m_ui->getWebPortalColorWarning();
            if (warn) {
                memcpy(m_webPortalColorWarning, warn, 4 * sizeof(float));
            }
            const float* dang = m_ui->getWebPortalColorDanger();
            if (dang) {
                memcpy(m_webPortalColorDanger, dang, 4 * sizeof(float));
            }
            const float* inf = m_ui->getWebPortalColorInfo();
            if (inf) {
                memcpy(m_webPortalColorInfo, inf, 4 * sizeof(float));
            }
            
            // Update in StreamManager
            m_streamManager->setWebPortalColors(
                m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
                m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
                m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
                m_webPortalColorCardHeader, m_webPortalColorBorder,
                m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
        } });

    m_ui->setOnWebPortalTextsChanged([this]()
                                     {
        // Update texts in real-time if streaming is active
        if (m_streamingEnabled && m_streamManager && m_ui) {
            // Synchronize texts from UI to Application
            m_webPortalTextStreamInfo = m_ui->getWebPortalTextStreamInfo();
            m_webPortalTextQuickActions = m_ui->getWebPortalTextQuickActions();
            m_webPortalTextCompatibility = m_ui->getWebPortalTextCompatibility();
            m_webPortalTextStatus = m_ui->getWebPortalTextStatus();
            m_webPortalTextCodec = m_ui->getWebPortalTextCodec();
            m_webPortalTextResolution = m_ui->getWebPortalTextResolution();
            m_webPortalTextStreamUrl = m_ui->getWebPortalTextStreamUrl();
            m_webPortalTextCopyUrl = m_ui->getWebPortalTextCopyUrl();
            m_webPortalTextOpenNewTab = m_ui->getWebPortalTextOpenNewTab();
            m_webPortalTextSupported = m_ui->getWebPortalTextSupported();
            m_webPortalTextFormat = m_ui->getWebPortalTextFormat();
            m_webPortalTextCodecInfo = m_ui->getWebPortalTextCodecInfo();
            m_webPortalTextSupportedBrowsers = m_ui->getWebPortalTextSupportedBrowsers();
            m_webPortalTextFormatInfo = m_ui->getWebPortalTextFormatInfo();
            m_webPortalTextCodecInfoValue = m_ui->getWebPortalTextCodecInfoValue();
            m_webPortalTextConnecting = m_ui->getWebPortalTextConnecting();
            
            // Update in StreamManager
            m_streamManager->setWebPortalTexts(
                m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
                m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
                m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
                m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
                m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
                m_webPortalTextConnecting);
        } });

    // Web Portal Start/Stop callback (independent from streaming)
    m_ui->setOnWebPortalStartStop([this](bool start)
                                  {
        LOG_INFO("[CALLBACK] Web Portal " + std::string(start ? "START" : "STOP") + " - creating thread...");
        
        if (start) {
            // Create separate thread to start portal
            std::thread([this]() {
                try {
                    if (!initWebPortal()) {
                        LOG_ERROR("Failed to start web portal");
                        if (m_ui) {
                            m_ui->setWebPortalActive(false);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception starting web portal: " + std::string(e.what()));
                    if (m_ui) {
                        m_ui->setWebPortalActive(false);
                    }
                }
            }).detach();
        } else {
            // Stop portal in separate thread
            std::thread([this]() {
                try {
                    stopWebPortal();
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception stopping web portal: " + std::string(e.what()));
                }
                
                // Update UI after stopping
                if (m_ui) {
                    m_ui->setWebPortalActive(false);
                }
            }).detach();
        }
        
        LOG_INFO("[CALLBACK] Thread criada, retornando (thread principal continua)"); });

    // Callback for source type change
    m_ui->setOnSourceTypeChanged([this](UIManager::SourceType sourceType)
                                 {
                                     LOG_INFO("Source type changed via UI");

                                     // Remote-source playback wants vsync ON so frame
                                     // changes land on display refresh boundaries — this
                                     // is what eliminated the 'stuttering at 60 fps but
                                     // smooth at 120 fps' artefact: without vsync our
                                     // ~60 Hz pacing drifts relative to the panel's own
                                     // 60 Hz refresh, dropping the occasional frame across
                                     // a refresh boundary. With vsync the loop's effective
                                     // rate IS the panel's rate, and the strict PTS gate
                                     // hands the queue's current head out at exactly the
                                     // next refresh.
                                     //
                                     // Local capture keeps vsync OFF — capture/encoder
                                     // threads can't tolerate the main loop stalling when
                                     // the window is minimized (compositors sometimes
                                     // park vsync at 0 Hz for backgrounded windows).
                                     if (m_window)
                                     {
                                         m_window->setVsync(sourceType == UIManager::SourceType::Remote);
                                     }

                                     if (sourceType == UIManager::SourceType::None)
                                     {
                                         LOG_INFO("None source selected - activating dummy mode");

                                         // Close current device if any
                                         if (m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             // Activate dummy mode
                                             m_capture->setDummyMode(true);

                                             // Configure dummy format with current resolution
                                             if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                             {
                                                 if (m_capture->startCapture())
                                                 {
                                                     LOG_INFO("Dummy mode activated: " + std::to_string(m_capture->getWidth()) + "x" +
                                                              std::to_string(m_capture->getHeight()));

                                                     // Update UI information
                                                     if (m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setCurrentDevice("");        // String vazia = None
                                                         m_ui->setCaptureControls(nullptr); // No V4L2 controls when None
                                                     }
                                                 }
                                             }
                                         }
                                     }
#ifdef __linux__
                                     if (sourceType == UIManager::SourceType::V4L2)
                                     {
                                         LOG_INFO("V4L2 source selected");
                                         // If device is already selected, try to reopen
                                         if (!m_devicePath.empty() && m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             m_capture->setDummyMode(false);

                                             if (m_capture->open(m_devicePath))
                                             {
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     m_capture->setFramerate(m_captureFps);
                                                     if (m_capture->startCapture())
                                                     {
                                                         if (m_ui)
                                                         {
                                                             m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                                  m_captureFps, m_devicePath);
                                                             m_ui->setCaptureControls(m_capture.get());
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // If failed to open device, return to dummy mode
                                                 LOG_WARN("Failed to open V4L2 device - activating dummy mode");
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setCaptureControls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_capture)
                                         {
                                             // If no device selected but V4L2 was chosen, keep in dummy mode
                                             LOG_INFO("No V4L2 device selected - keeping dummy mode");
                                         }
                                     }
#endif
#ifdef _WIN32
                                     if (sourceType == UIManager::SourceType::DS)
                                     {
                                         LOG_INFO("DirectShow source selected");

                                         // On Windows, m_devicePath can be empty or contain Linux path (/dev/video0)
                                         // Clear if it's a Linux path
                                         std::string devicePath = m_devicePath;
                                         if (!devicePath.empty() && devicePath.find("/dev/video") == 0)
                                         {
                                             LOG_INFO("Clearing Linux device path: " + devicePath);
                                             devicePath.clear();
                                             m_devicePath.clear(); // Also update class member
                                         }

                                         // Try to get current device from UIManager if m_devicePath is empty
                                         if (devicePath.empty() && m_ui)
                                         {
                                             devicePath = m_ui->getCurrentDevice();
                                             LOG_INFO("Getting current device from UIManager: " + devicePath);
                                         }

                                         // If device is already selected, try to reopen
                                         if (!devicePath.empty() && m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             m_capture->setDummyMode(false);

                                             if (m_capture->open(devicePath))
                                             {
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     m_capture->setFramerate(m_captureFps);
                                                     if (m_capture->startCapture())
                                                     {
                                                         if (m_ui)
                                                         {
                                                             m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                                  m_captureFps, devicePath);
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // If failed to open device, return to dummy mode
                                                 LOG_WARN("Failed to open DirectShow device - activating dummy mode");
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setCurrentDevice(""); // Empty string = None
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_capture)
                                         {
                                             // If no device selected but DS was chosen, keep in dummy mode
                                             LOG_INFO("No DirectShow device selected - keeping dummy mode");
                                             if (!m_capture->isOpen() || !m_capture->isDummyMode())
                                             {
                                                 m_capture->stopCapture();
                                                 m_capture->close();
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setCurrentDevice(""); // Empty string = None
                                                         m_ui->setCaptureControls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                     }
#endif

                                     // Phase 5b/#47: switching INTO Remote — close the
                                     // current capture (V4L2/DS/None) and stand up an
                                     // empty VideoCaptureRemote that waits for the
                                     // user to type a URL + click Connect.
                                     if (sourceType == UIManager::SourceType::Remote)
                                     {
                                         LOG_INFO("Remote source selected — close any active capture, await Connect");
                                         if (m_remoteMetaSync)
                                         {
                                             m_remoteMetaSync->stop();
                                             m_remoteMetaSync.reset();
                                         }
                                         if (m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                         }
                                         {
                                             auto remote = std::make_unique<VideoCaptureRemote>();
                                             VideoCaptureRemote::InterpolationMode imode = VideoCaptureRemote::InterpolationMode::Linear;
                                             if (m_remoteInterpolation == "nearest") imode = VideoCaptureRemote::InterpolationMode::Nearest;
                                             else if (m_remoteInterpolation == "off") imode = VideoCaptureRemote::InterpolationMode::Off;
                                             remote->setInterpolationMode(imode);
                                             m_capture = std::move(remote);
                                         }
                                         m_devicePath.clear();
                                         if (m_ui)
                                         {
                                             m_ui->setCaptureInfo(0, 0, 0, "Remote (not connected)");
                                             m_ui->setCurrentDevice("");
                                             m_ui->setCaptureControls(nullptr);
                                         }
                                     }
                                 });

    m_ui->setOnDeviceChanged([this](const std::string &devicePath)
                             {
        // Avoid infinite loop: if already processing "None", do nothing
        static bool processingDeviceChange = false;
        if (processingDeviceChange) {
            return;
        }
        processingDeviceChange = true;

        // Phase 5b/#47: when the active source is Remote, this callback
        // carries the remote BASE URL — not a V4L2 / DirectShow device
        // path. Spin up (or replace) the VideoCaptureRemote and
        // RemoteMetaSync to point at the new host. Empty URL == disconnect.
        if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote)
        {
            LOG_INFO("Remote URL change: " + (devicePath.empty() ? "(disconnect)" : devicePath));

            // Disconnect path optimisation: drop the on-screen frame
            // BEFORE waiting on the decode thread's join. The texture
            // upload happens on the main thread, so by the time we get
            // here we own m_frameProcessor and can wipe it immediately;
            // otherwise the user sees the last received frame frozen on
            // screen for the full ~50-100 ms it can take stopCapture to
            // unwind the interrupted I/O. Combined with the new
            // 'Disconnecting...' status the window shows, the perceived
            // freeze on Disconnect goes away.
            if (devicePath.empty() && m_frameProcessor)
            {
                m_frameProcessor->deleteTexture();
            }

            // Tear down any existing remote pipeline on a background
            // thread so the main loop doesn't stall while
            // VideoCaptureRemote joins its decode thread (up to ~1.5 s
            // with the interrupt callback). We move ownership of both
            // objects into a detached worker; the main thread
            // continues immediately to spin up the new capture.
            //
            // Switching from one remote URL straight to another used
            // to freeze the UI for the full join duration; this is
            // what made "click another stream while connected" feel
            // unresponsive even though the new connection itself was
            // already fast.
            if (m_remoteMetaSync || m_capture)
            {
                auto deadCapture = std::move(m_capture);
                auto deadMeta    = std::move(m_remoteMetaSync);
                std::thread([cap = std::move(deadCapture),
                             meta = std::move(deadMeta)]() mutable {
                    if (meta)
                    {
                        meta->stop();
                        meta.reset();
                    }
                    if (cap)
                    {
                        cap->stopCapture();
                        cap->close();
                        cap.reset();
                    }
                }).detach();
            }

            m_devicePath = devicePath;

            if (devicePath.empty())
            {
                // Mark the UI as cleanly disconnected so the connection
                // window stops showing 'Stream: WxH' from the last
                // session.
                if (m_ui)
                {
                    m_ui->setCaptureInfo(0, 0, 0, "");
                }
                // Re-arm the Image seed for the next connection — each
                // session gets one seed from /meta, after which the user
                // owns the values locally.
                m_remoteImageSeeded = false;
                processingDeviceChange = false;
                return;
            }

            // #49 Phase 3 — pull the bearer token the connection UI
            // (or password modal) stashed for this connect. Consumed
            // exactly once: read here, cleared immediately so a
            // subsequent unprotected connect doesn't accidentally
            // inherit the previous stream's token.
            std::string authToken;
            if (m_ui)
            {
                authToken = m_ui->getRemoteAuthToken();
                m_ui->setRemoteAuthToken("");
            }

            // Recreate the capture as Remote (the existing instance might be
            // V4L2/DS if the user just flipped source type).
            {
                auto remote = std::make_unique<VideoCaptureRemote>();
                VideoCaptureRemote::InterpolationMode imode = VideoCaptureRemote::InterpolationMode::Linear;
                if (m_remoteInterpolation == "nearest") imode = VideoCaptureRemote::InterpolationMode::Nearest;
                else if (m_remoteInterpolation == "off") imode = VideoCaptureRemote::InterpolationMode::Off;
                remote->setInterpolationMode(imode);
                remote->setAuthToken(authToken);
                m_capture = std::move(remote);
            }
            if (m_capture->open(devicePath))
            {
                m_capture->startCapture();

                // Pull the actual stream dimensions from the freshly-opened
                // decoder and sync the application's idea of capture size.
                // Without this the rest of the render pipeline keeps using
                // m_captureWidth / m_captureHeight from the local config
                // (1920x1080 by default), producing the "image fills only
                // half the window" symptom when the host streams smaller.
                const uint32_t actualW = m_capture->getWidth();
                const uint32_t actualH = m_capture->getHeight();
                if (actualW > 0 && actualH > 0)
                {
                    m_captureWidth  = actualW;
                    m_captureHeight = actualH;
                    LOG_INFO("Remote stream dims: " + std::to_string(actualW) + "x" + std::to_string(actualH));
                }

                if (m_ui)
                {
                    m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                         m_captureFps, devicePath);
                }
                // Re-start the /meta poller against the new host.
                m_remoteMetaSync = std::make_unique<RemoteMetaSync>();
                m_remoteMetaSync->setAuthToken(authToken);
                m_remoteMetaSync->start(devicePath,
                    [this](const RemoteMetaSync::Snapshot &snap)
                    {
                        std::lock_guard<std::mutex> lock(m_pendingRemoteMutex);
                        m_pendingRemotePreset          = snap.preset;
                        m_pendingRemotePresetHash      = snap.presetHash;
                        m_pendingRemotePipelineEnabled = snap.pipelineEnabled;
                        m_pendingRemoteParams.clear();
                        m_pendingRemoteParams.reserve(snap.parameters.size());
                        for (const auto &p : snap.parameters)
                        {
                            m_pendingRemoteParams.emplace_back(p.name, p.value);
                        }
                        m_pendingRemoteSourceWidth  = snap.sourceWidth;
                        m_pendingRemoteSourceHeight = snap.sourceHeight;
                        // Image-tab seed — first snapshot per connection only.
                        if (!m_remoteImageSeeded)
                        {
                            m_pendingRemoteImageBrightness     = snap.imageBrightness;
                            m_pendingRemoteImageContrast       = snap.imageContrast;
                            m_pendingRemoteImageMaintainAspect = snap.imageMaintainAspect;
                            m_pendingRemoteImageOutputWidth    = snap.imageOutputWidth;
                            m_pendingRemoteImageOutputHeight   = snap.imageOutputHeight;
                            m_hasPendingRemoteImageSeed        = true;
                        }
                        if (snap.sourceFps > 0) m_remoteSourceFps = snap.sourceFps;
                        m_hasPendingRemoteMeta.store(true);
                        // #68 — same upstream-client-count push the
                        // initCapture-side callback does. Without this
                        // copy, switching to Remote mid-session via
                        // Browse leaves the OSD's "watching with N
                        // others" stuck at 0 forever.
                        if (m_ui) m_ui->setRemoteUpstreamClientCount(snap.upstreamClientCount);
                    });
            }
            else
            {
                LOG_WARN("Failed to connect to remote host " + devicePath);
                if (m_ui)
                {
                    m_ui->setCaptureInfo(0, 0, 0, "Remote (failed)");
                    // Clear the current device so the connection window
                    // stops reporting 'connected'. Done via direct
                    // assignment because setCurrentDevice("") would
                    // re-enter this callback and we're still inside its
                    // processingDeviceChange guard.
                    m_ui->setCurrentDeviceSilent("");
                }
                m_devicePath.clear();
            }

            processingDeviceChange = false;
            return;
        }

        // If devicePath is empty, it means "None" - activate dummy mode
        if (devicePath.empty())
        {
            // Check if already in dummy mode to avoid loop
            if (m_devicePath.empty() && m_capture && m_capture->isDummyMode() && m_capture->isOpen()) {
                processingDeviceChange = false;
                return;
            }
            
            LOG_INFO("Disconnecting device (None selected) - activating dummy mode");
            
            // Close current device
            if (m_capture) {
                m_capture->stopCapture();
                m_capture->close();
                // Ativar modo dummy
                m_capture->setDummyMode(true);
                
                // Configure dummy format with current resolution
                if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                {
                    m_capture->startCapture();
                    LOG_INFO("Dummy mode activated: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                }
            }
            
            // Update device path
            m_devicePath = "";
            
            // Update UI information (without calling setCurrentDevice to avoid loop)
            if (m_ui) {
                if (m_capture && m_capture->isOpen()) {
                    m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                        m_captureFps, "None (Dummy)");
                    // Don't call setCurrentDevice here to avoid loop
                } else {
                    m_ui->setCaptureInfo(0, 0, 0, "None");
                    // Don't call setCurrentDevice here to avoid loop
                }
                m_ui->setCaptureControls(nullptr); // No V4L2 controls when no device
            }
            
            LOG_INFO("Dummy mode activated. Select a device to use real capture.");
            processingDeviceChange = false;
            return;
        }
        
        LOG_INFO("Changing device to: " + devicePath);
        
        // Save current settings
        uint32_t oldWidth = m_captureWidth;
        uint32_t oldHeight = m_captureHeight;
        uint32_t oldFps = m_captureFps;
        
        // Close current device (or dummy mode)
        if (m_capture) {
            m_capture->stopCapture();
            m_capture->close();
            // Disable dummy mode when trying to open real device
            m_capture->setDummyMode(false);
        }
        
        // Update device path
        m_devicePath = devicePath;
        
        // Clear FrameProcessor texture when changing device
        if (m_frameProcessor) {
            m_frameProcessor->deleteTexture();
        }
        
        // Reopen with new device
        if (m_capture && m_capture->open(devicePath)) {
            LOG_INFO("Device opened successfully, configuring format...");
            // Reconfigure format and framerate
            if (m_capture->setFormat(oldWidth, oldHeight, 0)) {
                LOG_INFO("Format configured, configuring framerate...");
                m_capture->setFramerate(oldFps);
                LOG_INFO("Framerate configured, starting capture (startCapture)...");
                if (m_capture->startCapture()) {
                    LOG_INFO("startCapture() returned true - device should be active (light on)");
                } else {
                    LOG_ERROR("startCapture() returned false - device was NOT activated!");
                }
                
                // Update UI information
                if (m_ui) {
                    m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                        m_captureFps, devicePath);
                    
                    // Reload V4L2 controls
                    m_ui->setCaptureControls(m_capture.get());
                }
                
                LOG_INFO("Device changed successfully");
                processingDeviceChange = false;
            } else {
                LOG_ERROR("Failed to configure format on new device");
                // Close device if failed
                m_capture->close();
                if (m_ui) {
                    m_ui->setCaptureInfo(0, 0, 0, "Error");
                }
                processingDeviceChange = false;
            }
        } else {
            LOG_ERROR("Failed to open new device: " + devicePath);
            processingDeviceChange = false;
            if (m_ui) {
                m_ui->setCaptureInfo(0, 0, 0, "Error");
            }
        } });

    // Configure current shader
    if (!m_presetPath.empty())
    {
        fs::path presetPath(m_presetPath);
        fs::path basePath = fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl";
        fs::path relativePath = fs::relative(presetPath, basePath);
        if (!relativePath.empty() && relativePath != presetPath)
        {
            m_ui->setCurrentShader(relativePath.string());
        }
        else
        {
            m_ui->setCurrentShader(m_presetPath);
        }

        // Callback to save preset
        m_ui->setOnSavePreset([this](const std::string &path, bool overwrite)
                              {
        if (!m_shaderEngine || !m_shaderEngine->isShaderActive()) {
            LOG_WARN("Nenhum preset carregado para salvar");
            return;
        }
        
        // Get custom parameters from ShaderEngine
        auto params = m_shaderEngine->getShaderParameters();
        std::unordered_map<std::string, float> customParams;
        for (const auto& param : params) {
            // Save all values (even if equal to default, to preserve configuration)
            customParams[param.name] = param.value;
        }
        
        // Save preset
        const ShaderPreset& preset = m_shaderEngine->getPreset();
        if (overwrite) {
            // Save overwriting
            if (preset.save(path, customParams)) {
                LOG_INFO("Preset saved: " + path);
            } else {
                LOG_ERROR("Failed to save preset: " + path);
            }
        } else {
            // Save as new file
            if (preset.saveAs(path, customParams)) {
                LOG_INFO("Preset saved as: " + path);
            } else {
                LOG_ERROR("Failed to salvar preset como: " + path);
            }
        } });
    }

    return true;
}

void Application::handleKeyInput()
{
    if (!m_ui || !m_window)
        return;

#ifdef USE_SDL2
    // SDL2: Check F12 key via WindowManagerSDL
    WindowManagerSDL *sdlWindow = static_cast<WindowManagerSDL *>(m_window.get());
    if (sdlWindow)
    {
        static bool f12Pressed = false;
        // SDLK_F12 está definido em SDL_keyboard.h (já incluído)
        bool f12CurrentlyPressed = sdlWindow->isKeyPressed(SDLK_F12);

        if (f12CurrentlyPressed && !f12Pressed)
        {
            m_ui->toggle();
            LOG_INFO("UI toggled: " + std::string(m_ui->isVisible() ? "VISIBLE" : "HIDDEN"));
            f12Pressed = true;
        }
        else if (!f12CurrentlyPressed)
        {
            f12Pressed = false;
        }

        // F11 toggles fullscreen. Edge-triggered like F12 so holding
        // the key down doesn't oscillate the window state.
        static bool f11Pressed = false;
        bool f11Now = sdlWindow->isKeyPressed(SDLK_F11);
        if (f11Now && !f11Pressed)
        {
            m_fullscreen = !m_fullscreen;
            m_window->setFullscreen(m_fullscreen, m_monitorIndex);
            if (m_ui) m_ui->setFullscreen(m_fullscreen);
            LOG_INFO(std::string("Fullscreen toggled: ") +
                     (m_fullscreen ? "ON" : "OFF"));
            f11Pressed = true;
        }
        else if (!f11Now)
        {
            f11Pressed = false;
        }
    }
#else
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window->getWindow());
    if (!window)
        return;

    // F12 to toggle UI
    static bool f12Pressed = false;
    if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS)
    {
        if (!f12Pressed)
        {
            m_ui->toggle();
            LOG_INFO("UI toggled: " + std::string(m_ui->isVisible() ? "VISIBLE" : "HIDDEN"));
            f12Pressed = true;
        }
    }
    else
    {
        f12Pressed = false;
    }

    // F11 toggles fullscreen.
    static bool f11Pressed = false;
    if (glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS)
    {
        if (!f11Pressed)
        {
            m_fullscreen = !m_fullscreen;
            m_window->setFullscreen(m_fullscreen, m_monitorIndex);
            if (m_ui) m_ui->setFullscreen(m_fullscreen);
            LOG_INFO(std::string("Fullscreen toggled: ") +
                     (m_fullscreen ? "ON" : "OFF"));
            f11Pressed = true;
        }
    }
    else
    {
        f11Pressed = false;
    }
#endif // USE_SDL2
}

bool Application::initStreaming()
{
    if (!m_streamingEnabled)
    {
        return true; // Streaming not enabled, not an error
    }

    // If web portal is active independently, stop it before starting streaming
    // Streaming will include web portal if enabled
    if (m_webPortalActive && m_webPortalServer)
    {
        LOG_INFO("Stopping independent Web Portal before starting streaming...");
        stopWebPortal();
    }

    // IMPORTANT: Clear existing streamManager BEFORE creating a new one
    // This prevents double free problems when there are configuration changes
    // CRITICAL: These operations are already in a separate thread, but can still block
    // Reduce to minimum necessary and avoid long waits
    if (m_streamManager)
    {
        LOG_INFO("Clearing existing StreamManager before reinitializing...");
        // Stop and clean up safely
        if (m_streamManager->isActive())
        {
            m_streamManager->stop();
        }
        m_streamManager->cleanup();
        m_streamManager.reset();
        m_currentStreamer = nullptr; // Clear streamer reference

        // IMPORTANT: Wait a bit to ensure all threads finished
        // and resources were released before creating a new StreamManager
        // Reduce wait time to minimum - detached threads should finish quickly
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Reduced to 10ms
    }

    m_streamManager = std::make_unique<StreamManager>();

    // IMPORTANT: Streaming resolution must be fixed, based on streaming tab settings
    // If not configured, use capture resolution (NEVER use window resolution which can change)
    // IMPORTANT: Check if device is open before accessing getWidth/getHeight
    uint32_t streamWidth = m_streamingWidth > 0 ? m_streamingWidth : (m_capture && m_capture->isOpen() ? m_capture->getWidth() : m_captureWidth);
    uint32_t streamHeight = m_streamingHeight > 0 ? m_streamingHeight : (m_capture && m_capture->isOpen() ? m_capture->getHeight() : m_captureHeight);
    uint32_t streamFps = m_streamingFps > 0 ? m_streamingFps : m_captureFps;

    LOG_INFO("initStreaming: Using resolution " + std::to_string(streamWidth) + "x" +
             std::to_string(streamHeight) + " @ " + std::to_string(streamFps) + "fps");
    LOG_INFO("initStreaming: m_streamingWidth=" + std::to_string(m_streamingWidth) +
             ", m_streamingHeight=" + std::to_string(m_streamingHeight));

    // Always use MPEG-TS streamer (audio + video required)
    auto tsStreamer = std::make_unique<HTTPTSStreamer>();

    // Configure video bitrate
    if (m_streamingBitrate > 0)
    {
        tsStreamer->setVideoBitrate(m_streamingBitrate * 1000); // Convert kbps to bps
    }

    // Configure audio bitrate
    if (m_streamingAudioBitrate > 0)
    {
        tsStreamer->setAudioBitrate(m_streamingAudioBitrate * 1000); // Convert kbps to bps
    }

    // Configure codecs
    tsStreamer->setVideoCodec(m_streamingVideoCodec);
    tsStreamer->setAudioCodec(m_streamingAudioCodec);

    // Configure H.264 preset (if applicable)
    if (m_streamingVideoCodec == "h264")
    {
        tsStreamer->setH264Preset(m_streamingH264Preset);
        auto hw = static_cast<MediaEncoder::HardwareEncoder>(m_streamingHardwareEncoder);
        tsStreamer->setHardwareEncoder(hw);
        // Pick the backend-specific preset string. For Auto we let
        // MediaEncoder hardcode its default (empty string) since the
        // actual backend isn't resolved until codec-open time.
        std::string backendPreset;
        switch (hw)
        {
            case MediaEncoder::HardwareEncoder::NVENC: backendPreset = m_streamingNvencPreset; break;
            case MediaEncoder::HardwareEncoder::VAAPI: backendPreset = m_streamingVaapiRcMode; break;
            case MediaEncoder::HardwareEncoder::QSV:   backendPreset = m_streamingQsvPreset;   break;
            case MediaEncoder::HardwareEncoder::AMF:   backendPreset = m_streamingAmfQuality;  break;
            default: backendPreset.clear(); break;
        }
        tsStreamer->setHardwareEncoderPreset(backendPreset);
    }
    // Configure H.265 preset, profile and level (if applicable)
    else if (m_streamingVideoCodec == "h265" || m_streamingVideoCodec == "hevc")
    {
        tsStreamer->setH265Preset(m_streamingH265Preset);
        tsStreamer->setH265Profile(m_streamingH265Profile);
        tsStreamer->setH265Level(m_streamingH265Level);
    }
    // Configure VP8 speed (if applicable)
    else if (m_streamingVideoCodec == "vp8")
    {
        tsStreamer->setVP8Speed(m_streamingVP8Speed);
    }
    // Configure VP9 speed (if applicable)
    else if (m_streamingVideoCodec == "vp9")
    {
        tsStreamer->setVP9Speed(m_streamingVP9Speed);
    }

    // Configure audio buffer size
    // setAudioBufferSize removed - buffer is managed automatically (OBS style)

    // Configure audio format to match AudioCapture
    if (m_audioCapture && m_audioCapture->isOpen())
    {
        tsStreamer->setAudioFormat(m_audioCapture->getSampleRate(), m_audioCapture->getChannels());
    }

    // Configure buffer parameters (loaded from configuration)
    tsStreamer->setBufferConfig(
        m_ui->getStreamingMaxVideoBufferSize(),
        m_ui->getStreamingMaxAudioBufferSize(),
        m_ui->getStreamingMaxBufferTimeSeconds(),
        m_ui->getStreamingAVIOBufferSize());

    // Configure Web Portal
    tsStreamer->enableWebPortal(m_webPortalEnabled);
    tsStreamer->setWebPortalTitle(m_webPortalTitle);

    // Configure API Controller
    tsStreamer->setApplicationForAPI(this);
    tsStreamer->setUIManagerForAPI(m_ui.get());
    tsStreamer->setWebPortalSubtitle(m_webPortalSubtitle);
    tsStreamer->setWebPortalImagePath(m_webPortalImagePath);
    tsStreamer->setWebPortalBackgroundImagePath(m_webPortalBackgroundImagePath);
    // IMPORTANT: Pass colors only if arrays are correctly initialized
    // Arrays are initialized with default values in constructor, so they are always valid
    tsStreamer->setWebPortalColors(
        m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
        m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
        m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
        m_webPortalColorCardHeader, m_webPortalColorBorder,
        m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
    // Configure editable texts
    tsStreamer->setWebPortalTexts(
        m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
        m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
        m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
        m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
        m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
        m_webPortalTextConnecting);

    // Configure Web Portal HTTPS
    if (m_webPortalHTTPSEnabled && !m_webPortalSSLCertPath.empty() && !m_webPortalSSLKeyPath.empty())
    {
        // Paths will be resolved in HTTPTSStreamer::start() which searches in multiple locations
        // Here we just pass paths as configured in UI
        tsStreamer->setSSLCertificatePath(m_webPortalSSLCertPath, m_webPortalSSLKeyPath);
        tsStreamer->enableHTTPS(true);
        LOG_INFO("HTTPS enabled in configuration. Certificates will be searched in execution directory.");
    }

    // Store streamer reference before moving it to StreamManager
    m_currentStreamer = tsStreamer.get();
    m_streamManager->addStreamer(std::move(tsStreamer));
    LOG_INFO("Using HTTP MPEG-TS streamer (audio + video)");

    if (!m_streamManager->initialize(m_streamingPort, streamWidth, streamHeight, streamFps))
    {
        LOG_ERROR("Failed to initialize StreamManager");
        m_streamManager.reset();
        return false;
    }

    if (!m_streamManager->start())
    {
        LOG_ERROR("Failed to start streaming");
        m_streamManager.reset();
        return false;
    }

    LOG_INFO("Streaming started on port " + std::to_string(m_streamingPort));
    auto urls = m_streamManager->getStreamUrls();
    for (const auto &url : urls)
    {
        LOG_INFO("Stream available: " + url);
    }

    // Clear found paths (will be updated when streaming actually starts)
    // Paths are found in HTTPTSStreamer::start(), but we don't have direct access here
    // We'll update UI periodically in main loop
    m_foundSSLCertPath.clear();
    m_foundSSLKeyPath.clear();

    // Initialize audio capture if not already initialized (always required for streaming)
    if (!m_audioCapture)
    {
        if (!initAudioCapture())
        {
            LOG_WARN("Failed to initialize audio capture - continuing without audio");
        }
    }

    return true;
}

bool Application::initWebPortal()
{
    if (m_webPortalActive && m_webPortalServer)
    {
        LOG_INFO("Web Portal is already active");
        return true;
    }

    if (!m_webPortalEnabled)
    {
        LOG_WARN("Web Portal is disabled in configuration");
        return false;
    }

    LOG_INFO("Starting independent Web Portal...");

    // Create HTTPTSStreamer only for portal (without streaming)
    m_webPortalServer = std::make_unique<HTTPTSStreamer>();

    // Configure Web Portal
    m_webPortalServer->enableWebPortal(true);
    m_webPortalServer->setWebPortalTitle(m_webPortalTitle);
    m_webPortalServer->setWebPortalSubtitle(m_webPortalSubtitle);
    m_webPortalServer->setWebPortalImagePath(m_webPortalImagePath);
    m_webPortalServer->setWebPortalBackgroundImagePath(m_webPortalBackgroundImagePath);
    m_webPortalServer->setWebPortalColors(
        m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
        m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
        m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
        m_webPortalColorCardHeader, m_webPortalColorBorder,
        m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
    m_webPortalServer->setWebPortalTexts(
        m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
        m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
        m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
        m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
        m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
        m_webPortalTextConnecting);

    // Configure API Controller
    m_webPortalServer->setApplicationForAPI(this);
    m_webPortalServer->setUIManagerForAPI(m_ui.get());

    // Configure HTTPS
    if (m_webPortalHTTPSEnabled && !m_webPortalSSLCertPath.empty() && !m_webPortalSSLKeyPath.empty())
    {
        m_webPortalServer->setSSLCertificatePath(m_webPortalSSLCertPath, m_webPortalSSLKeyPath);
        m_webPortalServer->enableHTTPS(true);
        LOG_INFO("HTTPS enabled for Web Portal. Certificates will be searched in execution directory.");
    }

    // Initialize with dummy dimensions (not used for portal without streaming)
    if (!m_webPortalServer->initialize(m_streamingPort, 640, 480, 30))
    {
        LOG_ERROR("Failed to initialize Web Portal");
        m_webPortalServer.reset();
        return false;
    }

    // Iniciar apenas o servidor HTTP (sem encoding thread)
    if (!m_webPortalServer->startWebPortalServer())
    {
        LOG_ERROR("Failed to start Web Portal HTTP server");
        m_webPortalServer.reset();
        return false;
    }

    m_webPortalActive = true;
    LOG_INFO("Web Portal started on port " + std::to_string(m_streamingPort));
    std::string portalUrl = (m_webPortalHTTPSEnabled ? "https://" : "http://") +
                            std::string("localhost:") + std::to_string(m_streamingPort);
    LOG_INFO("Web Portal available: " + portalUrl);

    // Update UI
    if (m_ui)
    {
        m_ui->setWebPortalActive(true);
    }

    return true;
}

void Application::stopWebPortal()
{
    if (!m_webPortalActive || !m_webPortalServer)
    {
        return;
    }

    LOG_INFO("Parando Portal Web...");

    // Stop HTTP server
    m_webPortalServer->stop();
    m_webPortalServer.reset();
    m_webPortalActive = false;

    LOG_INFO("Web Portal stopped");

    // Update UI
    if (m_ui)
    {
        m_ui->setWebPortalActive(false);
    }
}

bool Application::initAudioCapture()
{
    // Audio is needed for streaming or recording
    // If neither is enabled, we can skip audio initialization
    if (!m_streamingEnabled && !m_recordingManager)
    {
        return true; // Audio not needed, not an error
    }

    m_audioCapture = AudioCaptureFactory::create();
    if (!m_audioCapture)
    {
        LOG_ERROR("Failed to create AudioCapture for this platform");
        return false;
    }

    // RecordingManager is now initialized in Application::init() before audio capture
    // Just set audio format here if RecordingManager exists
    if (!m_recordingManager)
    {
        LOG_WARN("RecordingManager not initialized - recording will not be available");
    }

    // Use the saved input source if any (set via UI / web portal in a
    // previous session). Empty string falls back to PulseAudio's default
    // source. Replaces the old "open() creates a null-sink and a
    // separate restoreAudioDeviceConnections() loops the saved source
    // into it" two-step.
    std::string savedSource;
    if (m_ui)
    {
        savedSource = m_ui->getAudioInputSourceId();
    }

    if (!m_audioCapture->open(savedSource))
    {
        LOG_ERROR("Failed to open audio device");
        m_audioCapture.reset();
        return false;
    }

    // Start capturing
    if (!m_audioCapture->startCapture())
    {
        LOG_ERROR("Failed to start audio capture");
        m_audioCapture->close();
        m_audioCapture.reset();
        return false;
    }

    LOG_INFO("Audio capture started: " + std::to_string(m_audioCapture->getSampleRate()) +
             "Hz, " + std::to_string(m_audioCapture->getChannels()) + " channels");

    // Connect audio capture to UI
    if (m_ui && m_audioCapture)
    {
        m_ui->setAudioCapture(m_audioCapture.get());
    }

    // Audio format for RecordingManager is already set in init() after audio capture starts

    return true;
}

void Application::restoreAudioDeviceConnections()
{
    // Obsolete since 0.8.0-alpha: the saved input source is now passed
    // directly to AudioCapturePulse::open() during initAudioCapture(),
    // so there is no separate "load a loopback module into a null-sink"
    // step to restore. Kept as a no-op for callers that still invoke it.
}

void Application::run()
{
    if (!m_initialized)
    {
        LOG_ERROR("Application not initialized");
        return;
    }

    LOG_INFO("Starting main loop...");

    // IMPORTANT: Ensure viewport is updated before first frame
    // This is especially important when window is created in fullscreen
    if (m_shaderEngine)
    {
        uint32_t currentWidth = m_window->getWidth();
        uint32_t currentHeight = m_window->getHeight();
        m_shaderEngine->setViewport(currentWidth, currentHeight);
    }

    while (!m_window->shouldClose())
    {
        // Check if window is still valid before polling events
        // This prevents crashes when window is invalidated (e.g., KVM switch)
        if (!m_window)
        {
            LOG_WARN("Window is invalid - exiting main loop");
            break;
        }

        // Phase 4 of #47: drain pending remote /meta deltas onto this
        // thread before any GL work. Cheap when nothing pending.
        applyPendingRemoteMeta();

        // Phase 2 of #49: reconcile the public-directory publish state
        // with whatever the UI toggle currently says. Cheap when no
        // transition is needed (just compares two booleans and a few
        // strings).
        syncDirectoryClient();
        // Cursor visibility tracks BOTH UIManager::isVisible() and the
        // OSD overlay (#68). The setOnVisibilityChanged callback only
        // fires on F12 toggles — toggling the quick-actions widget via
        // View doesn't, and the desktop client mode flip doesn't either.
        // Running this every frame keeps cursor state in sync; the
        // window manager's own internal cache makes redundant calls
        // free.
        updateCursorVisibility();

        m_window->pollEvents();
        
        // Check again after polling events (window may have been invalidated)
        if (m_window->shouldClose())
        {
            break;
        }

        // Process pending preset applications (from API threads)
        {
            std::lock_guard<std::mutex> lock(m_presetQueueMutex);
            while (!m_pendingPresets.empty())
            {
                std::string presetName = m_pendingPresets.front();
                m_pendingPresets.pop();
                // Apply preset in main thread (safe to access OpenGL, capture, UI)
                applyPreset(presetName);
            }
        }

        // Process pending resolution changes (from API threads)
        {
            std::lock_guard<std::mutex> lock(m_resolutionQueueMutex);
            while (!m_pendingResolutionChanges.empty())
            {
                ResolutionChange change = m_pendingResolutionChanges.front();
                m_pendingResolutionChanges.pop();
                // Apply resolution change in main thread (safe to access OpenGL, capture, UI)
                applyResolutionChange(change.width, change.height);
            }
        }

        // Process pending fullscreen change (outside callback to avoid deadlock)
        if (m_pendingFullscreenChange && m_window)
        {
            m_pendingFullscreenChange = false;
            m_window->setFullscreen(m_fullscreen, m_monitorIndex);
            // The resize callback will be called automatically by GLFW
            // Don't do setViewport here to avoid blocking
        }

        // IMPORTANT: Capture, processing and streaming always continue,
        // regardless of window focus. This ensures streaming works
        // even when window is not focused.

        // OPTION A: Process audio continuously in main thread (independent of video frames)
        // Process ALL available samples in loop until exhausted
        // This ensures audio is processed continuously even if main loop doesn't run at 60 FPS
        // IMPORTANT: Process PulseAudio mainloop whenever audio is open
        // This is critical to prevent PulseAudio from freezing system audio
        // The mainloop needs to be processed regularly, even without active streaming
        if (m_audioCapture && m_audioCapture->isOpen())
        {
            // Process PulseAudio mainloop to avoid blocking system audio
            // This should be done always, not only when streaming is active
            if (m_streamManager && m_streamManager->isActive())
            {

                // Calculate buffer size based on time for synchronization
                uint32_t audioSampleRate = m_audioCapture->getSampleRate();
                uint32_t videoFps = m_streamingFps > 0 ? m_streamingFps : m_captureFps;

                // Calculate samples corresponding to 1 video frame
                // For 60 FPS and 44100Hz: 44100/60 = 735 samples per frame
                size_t samplesPerVideoFrame = (audioSampleRate > 0 && videoFps > 0)
                                                  ? static_cast<size_t>((audioSampleRate + videoFps / 2) / videoFps)
                                                  : 512;
                samplesPerVideoFrame = std::max(static_cast<size_t>(64), std::min(samplesPerVideoFrame, static_cast<size_t>(audioSampleRate)));

                // Process audio in loop until all available samples are exhausted
                // OPTIMIZATION: Reuse buffer to avoid unnecessary allocations
                // IMPORTANT: Add iteration limit to avoid infinite loop that would freeze main thread
                std::vector<int16_t> audioBuffer(samplesPerVideoFrame);

                // Iteration limit to avoid infinite loop (process at most 10 audio frames per cycle)
                const int maxIterations = 10;
                int iteration = 0;

                while (iteration < maxIterations)
                {
                    // Read audio in chunks corresponding to 1 video frame time
                    size_t samplesRead = m_audioCapture->getSamples(audioBuffer.data(), samplesPerVideoFrame);

                    if (samplesRead > 0)
                    {
                        // Share audio data between streaming and recording
                        if (m_streamManager && m_streamManager->isActive())
                        {
                            m_streamManager->pushAudio(audioBuffer.data(), samplesRead);
                        }
                        if (m_recordingManager && m_recordingManager->isRecording())
                        {
                            m_recordingManager->pushAudio(audioBuffer.data(), samplesRead);
                        }

                        // If we read less than expected, no more samples available
                        if (samplesRead < samplesPerVideoFrame)
                        {
                            break; // No more samples available
                        }
                    }
                    else
                    {
                        // No more samples available, stop
                        break;
                    }

                    iteration++;
                }

                // If we reached the limit, too much audio accumulated - log only occasionally
                if (iteration >= maxIterations)
                {
                    static int logCount = 0;
                    if (logCount < 3)
                    {
                        LOG_WARN("Audio accumulated: processing in chunks to avoid blocking main thread");
                        logCount++;
                    }
                }
            }
            else if (m_recordingManager && m_recordingManager->isRecording())
            {
                // Recording is active (but streaming is not), process audio for recording
                uint32_t audioSampleRate = m_audioCapture->getSampleRate();
                uint32_t videoFps = m_captureFps;
                size_t samplesPerVideoFrame = (audioSampleRate > 0 && videoFps > 0)
                                                  ? static_cast<size_t>((audioSampleRate + videoFps / 2) / videoFps)
                                                  : 512;
                samplesPerVideoFrame = std::max(static_cast<size_t>(64), std::min(samplesPerVideoFrame, static_cast<size_t>(audioSampleRate)));

                std::vector<int16_t> audioBuffer(samplesPerVideoFrame);
                const int maxIterations = 10;
                int iteration = 0;

                while (iteration < maxIterations)
                {
                    size_t samplesRead = m_audioCapture->getSamples(audioBuffer.data(), samplesPerVideoFrame);
                    if (samplesRead > 0)
                    {
                        m_recordingManager->pushAudio(audioBuffer.data(), samplesRead);
                        if (samplesRead < samplesPerVideoFrame)
                        {
                            break;
                        }
                    }
                    else
                    {
                        break;
                    }
                    iteration++;
                }
            }
            else
            {
                // Neither streaming nor recording active, but we still need to process mainloop
                // to prevent PulseAudio from freezing system audio
                // Read and discard samples to keep buffer clean
                const size_t maxSamples = 4096; // Temporary buffer
                std::vector<int16_t> tempBuffer(maxSamples);
                m_audioCapture->getSamples(tempBuffer.data(), maxSamples);
            }
        }

        // Process keyboard input (F12 to toggle UI)
        handleKeyInput();

        // Start ImGui frame
        if (m_ui)
        {
            m_ui->beginFrame();
            // Keep the Remote connection window's capture pointer current.
            // Cheap pointer swap each iteration; UIRemoteConnection only
            // dereferences it inside its own render() guarded by m_visible.
            if (auto *win = m_ui->getRemoteConnectionWindow())
            {
                win->setCapture(m_capture.get());
            }

            // #49 Phase 4: keep the directory browser running while
            // the dedicated 'Browse public directory' window is open.
            // Lazy-construct on first sighting so we don't pay for
            // the worker thread when the user never opens the window.
            if (auto *browseWin = m_ui->getDirectoryBrowserWindow())
            {
                if (browseWin->isVisible())
                {
                    if (!m_directoryBrowser)
                    {
                        m_directoryBrowser = std::make_unique<DirectoryBrowser>();
                    }
                    browseWin->setDirectoryBrowser(m_directoryBrowser.get());
                    // start() is idempotent on the same URL and
                    // tears-down/restarts on a different URL, so calling
                    // every tick lets a runtime URL edit take effect
                    // immediately. The no-op case has no cost.
                    m_directoryBrowser->start(m_ui->getDirectoryUrl());
                }
                else if (m_directoryBrowser && m_directoryBrowser->isRunning())
                {
                    m_directoryBrowser->stop();
                }
            }
        }

        // Try to capture and process the latest frame (discarding old frames)
        // IMPORTANT: Capture always continues, even when window is not focused
        // This ensures streaming and processing continue working
        // IMPORTANT: Try to capture if device is open OR in dummy mode
        bool newFrame = false;
        // Process frames if device is open OR in dummy mode
        bool shouldProcess = m_capture && (m_capture->isOpen() || m_capture->isDummyMode());

        // Debug log for dummy mode
        static bool dummyLogShown = false;
        if (m_capture && m_capture->isDummyMode() && !dummyLogShown)
        {
            LOG_INFO("Application: Processing dummy mode (isOpen: " + std::string(m_capture->isOpen() ? "true" : "false") +
                     ", isDummyMode: " + std::string(m_capture->isDummyMode() ? "true" : "false") + ")");
            dummyLogShown = true;
        }

        if (shouldProcess && !m_isReconfiguring)
        {
            // Double-check that capture is still open and not being reconfigured
            // This prevents race conditions where reconfiguration starts between checks
            if (!m_capture->isOpen() && !m_capture->isDummyMode())
            {
                // Device was closed, skip processing
                shouldProcess = false;
            }
            else
            {
                // Try to process frame multiple times if we don't have valid texture
                // This is important after reconfiguration when texture was deleted
                // In dummy mode, always try to process to ensure green frame appears
                int maxAttempts = (m_frameProcessor->getTexture() == 0 && !m_frameProcessor->hasValidFrame()) ? 5 : 1;
                if (m_capture->isDummyMode())
                {
                    maxAttempts = 5; // Always try multiple times in dummy mode
                }
                for (int attempt = 0; attempt < maxAttempts; ++attempt)
                {
                    // Check again before each attempt to avoid processing during reconfiguration
                    if (m_isReconfiguring || (!m_capture->isOpen() && !m_capture->isDummyMode()))
                    {
                        break; // Stop processing if reconfiguration started
                    }
                    newFrame = m_frameProcessor->processFrame(m_capture.get());

                    if (newFrame && m_frameProcessor->hasValidFrame() && m_frameProcessor->getTexture() != 0)
                    {
                        break; // Frame processed successfully
                    }
                    if (attempt < maxAttempts - 1)
                    {
#ifdef PLATFORM_LINUX
                        usleep(5000); // 5ms between attempts
#else
                        Sleep(5); // 5ms between attempts
#endif
                    }
                }
            }
        }

        // Always render if we have a valid frame
        // This ensures we're always showing the latest frame
        // Skip rendering during reconfiguration to avoid accessing deleted textures
        if (!m_isReconfiguring && m_frameProcessor && m_frameProcessor->hasValidFrame() && m_frameProcessor->getTexture() != 0)
        {
            // Log resolução de captura original (antes de qualquer processamento)
            static int originalCaptureLogCount = 0;
            if (originalCaptureLogCount++ < 3)
            {
                LOG_INFO("=== ORIGINAL CAPTURE TEXTURE ===");
                LOG_INFO("Original capture texture: " + std::to_string(m_frameProcessor->getTexture()) +
                         ", Size: " + std::to_string(m_frameProcessor->getTextureWidth()) + "x" + 
                         std::to_string(m_frameProcessor->getTextureHeight()));
                LOG_INFO("================================");
            }
            
            // Apply shader if active
            GLuint textureToRender = m_frameProcessor->getTexture();
            bool isShaderTexture = false;

            // Master toggle: when the user disables the shader pipeline
            // from the Shader tab we keep the loaded preset / parameters
            // intact but stop running the chain on the live render. The
            // captured texture for stream/recording then reflects the
            // raw source — same path the user sees on the window.
            const bool shaderPipelineOn = (!m_ui || m_ui->getShaderPipelineEnabled());
            if (shaderPipelineOn && m_shaderEngine && m_shaderEngine->isShaderActive())
            {
                // IMPORTANT: Update viewport with window dimensions before applying shader
                // This ensures the last pass renders to the correct window size
                // IMPORTANT: Always use current dimensions, especially when entering fullscreen
                // IMPORTANT: Validate dimensions before updating viewport to avoid issues during resize
                uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
                uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

                // Validate dimensions before updating viewport
                if (currentWidth > 0 && currentHeight > 0 && currentWidth <= 7680 && currentHeight <= 4320)
                {
                    m_shaderEngine->setViewport(currentWidth, currentHeight);
                }

                // Source para o shader chain. Por default, usamos a textura full-res
                // do FrameProcessor. Se o usuário pediu uma resolução LÓGICA menor que
                // a que o V4L2 entregou (driver ajustou pra mais próxima suportada),
                // fazemos um downscale com filter=NEAREST aqui — assim shaders CRT
                // recebem entrada "pixelada" baixa-res como foram desenhados, em vez
                // de 1080p suave onde scanlines viram sub-pixel e somem.
                GLuint shaderSrcTex = m_frameProcessor->getTexture();
                uint32_t shaderSrcW = m_frameProcessor->getTextureWidth();
                uint32_t shaderSrcH = m_frameProcessor->getTextureHeight();

                // Lê overscan antes de decidir o caminho — se há overscan, mesmo
                // que não precise de downscale, ainda fazemos o pass do FBO pra
                // aplicar o crop via viewport offset.
                const float overscanXPctRead = m_ui ? m_ui->getSourceOverscanPercentX() : 0.0f;
                const float overscanYPctRead = m_ui ? m_ui->getSourceOverscanPercentY() : 0.0f;
                const bool needsOverscan = (overscanXPctRead > 0.001f || overscanYPctRead > 0.001f);

                const bool needsDownscale = (m_logicalCaptureWidth > 0 &&
                                             m_logicalCaptureHeight > 0 &&
                                             m_logicalCaptureWidth < shaderSrcW &&
                                             m_logicalCaptureHeight < shaderSrcH &&
                                             shaderSrcTex != 0);

                if ((needsDownscale || needsOverscan) && shaderSrcTex != 0)
                {
                    // FBO size: logical quando há downscale, source dims caso contrário.
                    const uint32_t fboW = needsDownscale ? m_logicalCaptureWidth : shaderSrcW;
                    const uint32_t fboH = needsDownscale ? m_logicalCaptureHeight : shaderSrcH;

                    if (m_shaderSourceFBO == 0 ||
                        m_shaderSourceFBOWidth != fboW ||
                        m_shaderSourceFBOHeight != fboH)
                    {
                        if (m_shaderSourceTexture != 0)
                        {
                            glDeleteTextures(1, &m_shaderSourceTexture);
                            m_shaderSourceTexture = 0;
                        }
                        if (m_shaderSourceFBO != 0)
                        {
                            glDeleteFramebuffers(1, &m_shaderSourceFBO);
                            m_shaderSourceFBO = 0;
                        }

                        glGenTextures(1, &m_shaderSourceTexture);
                        glBindTexture(GL_TEXTURE_2D, m_shaderSourceTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                     static_cast<GLsizei>(fboW),
                                     static_cast<GLsizei>(fboH),
                                     0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        glGenFramebuffers(1, &m_shaderSourceFBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, m_shaderSourceFBO);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_TEXTURE_2D, m_shaderSourceTexture, 0);

                        m_shaderSourceFBOWidth = fboW;
                        m_shaderSourceFBOHeight = fboH;
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, m_shaderSourceFBO);

                    // Overscan: amplia o viewport de modo que apenas a região
                    // central (1 - 2*overscan) do source caia dentro do FBO.
                    // X e Y independentes; 0 = sem corte, 0.45 = corta 45% de cada lado.
                    // std::clamp é C++17; o MinGW antigo do build Windows não tem.
                    const float overscanX = std::max(0.0f, std::min(0.45f, overscanXPctRead / 100.0f));
                    const float overscanY = std::max(0.0f, std::min(0.45f, overscanYPctRead / 100.0f));
                    const float visibleFracX = 1.0f - 2.0f * overscanX;
                    const float visibleFracY = 1.0f - 2.0f * overscanY;
                    const float vpW = static_cast<float>(fboW) / visibleFracX;
                    const float vpH = static_cast<float>(fboH) / visibleFracY;
                    const GLint vpX = static_cast<GLint>((static_cast<float>(fboW) - vpW) / 2.0f);
                    const GLint vpY = static_cast<GLint>((static_cast<float>(fboH) - vpH) / 2.0f);
                    glViewport(vpX, vpY,
                               static_cast<GLsizei>(vpW),
                               static_cast<GLsizei>(vpH));

                    // Forçar NEAREST na textura source pra preservar look pixelado
                    // no downscale, e restaurar pra config do FrameProcessor depois.
                    const GLint restoreFilter = m_frameProcessor->getTextureFilterLinear()
                                                    ? GL_LINEAR
                                                    : GL_NEAREST;
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, shaderSrcTex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                    m_renderer->renderTexture(shaderSrcTex,
                                              fboW, fboH,
                                              false, false, 1.0f, 1.0f, false,
                                              shaderSrcW, shaderSrcH,
                                              /*preserveViewport=*/true);

                    glBindTexture(GL_TEXTURE_2D, shaderSrcTex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, restoreFilter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, restoreFilter);

                    glBindFramebuffer(GL_FRAMEBUFFER, 0);

                    shaderSrcTex = m_shaderSourceTexture;
                    shaderSrcW = fboW;
                    shaderSrcH = fboH;
                }

                textureToRender = m_shaderEngine->applyShader(shaderSrcTex, shaderSrcW, shaderSrcH);
                isShaderTexture = true;
                
                // Log saída do shader
                static int shaderOutputLogCount = 0;
                if (shaderOutputLogCount++ < 3)
                {
                    LOG_INFO("=== SHADER OUTPUT ===");
                    LOG_INFO("Shader output texture: " + std::to_string(textureToRender) +
                             ", Output size: " + std::to_string(m_shaderEngine->getOutputWidth()) + "x" + 
                             std::to_string(m_shaderEngine->getOutputHeight()));
                    LOG_INFO("=====================");
                }

                // DEBUG: Check returned texture
                if (textureToRender == 0)
                {
                    LOG_WARN("Shader returned invalid texture (0), using original texture");
                    textureToRender = m_frameProcessor->getTexture();
                    isShaderTexture = false;
                }
                else
                {
                }
            }

            // Clear window framebuffer before rendering
            // IMPORTANT: Framebuffer 0 is the window (default framebuffer)
            // IMPORTANT: Lock mutex to protect during resize
            std::lock_guard<std::mutex> resizeLock(m_resizeMutex);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // IMPORTANT: Reset viewport to full window size
            // This ensures texture is rendered to entire window
            // IMPORTANT: Always update viewport with current window dimensions
            // This is especially important when entering fullscreen
            uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
            uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

            // Validate dimensions before continuing
            if (currentWidth == 0 || currentHeight == 0 || currentWidth > 7680 || currentHeight > 4320)
            {
                // Invalid dimensions, skip this frame
                if (m_ui)
                {
                    m_ui->endFrame();
                }
                m_window->swapBuffers();
                continue;
            }

            // DEBUG: Log to check if dimensions changed
            static uint32_t lastViewportWidth = 0, lastViewportHeight = 0;
            if (currentWidth != lastViewportWidth || currentHeight != lastViewportHeight)
            {
                lastViewportWidth = currentWidth;
                lastViewportHeight = currentHeight;
            }

            // Usar resolução da janela (sem limitações hardcoded)
            // O usuário pode controlar a resolução de saída via setOutputResolution()
            glViewport(0, 0, currentWidth, currentHeight);

            // IMPORTANT: For shaders with alpha (like Game Boy), don't clear with opaque black
            // Clear with transparent black so blending works correctly
            if (isShaderTexture)
            {
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparent for shaders with alpha
            }
            else
            {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Opaque for normal capture
            }
            glClear(GL_COLOR_BUFFER_BIT);

            // For shader textures (framebuffer), invert Y (shader renders inverted)
            // For original texture (camera), don't invert Y (already correct)
            // IMPORTANT: If shader texture, may need blending for alpha
            // Get texture dimensions to calculate aspect ratio
            // IMPORTANT: For maintainAspect, always use ORIGINAL CAPTURE dimensions
            // because shader processes image but maintains same aspect ratio
            // Shader output texture may have window (viewport) dimensions, not image dimensions
            uint32_t renderWidth, renderHeight;
            if (isShaderTexture && m_maintainAspect)
            {
                // For maintainAspect with shader, use original capture dimensions
                // Shader processes but maintains original image aspect ratio
                renderWidth = m_frameProcessor->getTextureWidth();
                renderHeight = m_frameProcessor->getTextureHeight();
            }
            else if (isShaderTexture)
            {
                // Without maintainAspect, use shader output dimensions
                renderWidth = m_shaderEngine->getOutputWidth();
                renderHeight = m_shaderEngine->getOutputHeight();
                if (renderWidth == 0 || renderHeight == 0)
                {
                    LOG_WARN("Shader output dimensions invalid (0x0), using capture dimensions");
                    renderWidth = m_frameProcessor->getTextureWidth();
                    renderHeight = m_frameProcessor->getTextureHeight();
                }
            }
            else
            {
                // Without shader, use capture dimensions
                renderWidth = m_frameProcessor->getTextureWidth();
                renderHeight = m_frameProcessor->getTextureHeight();
            }

            // NOVO: Aplicar resolução de saída configurável (se definida)
            // Isso permite que o usuário controle a resolução final antes de esticar para a janela
            // 0 = automático (usar resolução do source)
            GLuint finalTexture = textureToRender;
            uint32_t finalRenderWidth = renderWidth;
            uint32_t finalRenderHeight = renderHeight;
            
            // Log detalhado das resoluções no início do pipeline
            static int pipelineResLogCount = 0;
            if (pipelineResLogCount++ < 3)
            {
                LOG_INFO("=== PIPELINE RESOLUTIONS ===");
                LOG_INFO("Original capture: " + 
                         std::to_string(m_frameProcessor->getTextureWidth()) + "x" + std::to_string(m_frameProcessor->getTextureHeight()));
                LOG_INFO("Shader output (renderWidth/Height): " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight));
                if (isShaderTexture)
                {
                    LOG_INFO("Shader engine output: " + std::to_string(m_shaderEngine->getOutputWidth()) + "x" + 
                             std::to_string(m_shaderEngine->getOutputHeight()));
                }
                LOG_INFO("Output resolution (m_outputWidth/Height): " + std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight));
                LOG_INFO("textureToRender: " + std::to_string(textureToRender) + ", isShaderTexture: " + std::string(isShaderTexture ? "yes" : "no"));
                LOG_INFO("===========================");
            }
            
            // Garantir que renderWidth e renderHeight são válidos (não 0)
            if (finalRenderWidth == 0 || finalRenderHeight == 0)
            {
                static int renderDimensionWarningCount = 0;
                if (renderDimensionWarningCount++ < 3)
                {
                    LOG_WARN("Frame render: Invalid render dimensions (" +
                             std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                             "), using capture dimensions");
                }
                // Fallback: usar dimensões de captura
                if (m_frameProcessor)
                {
                    finalRenderWidth = m_frameProcessor->getTextureWidth();
                    finalRenderHeight = m_frameProcessor->getTextureHeight();
                }
                // Se ainda for 0, usar dimensões padrão
                if (finalRenderWidth == 0 || finalRenderHeight == 0)
                {
                    finalRenderWidth = 1920;
                    finalRenderHeight = 1080;
                }
            }

            if (m_outputWidth > 0 && m_outputHeight > 0)
            {
                // Resolução de saída configurada - fazer downscale/upscale da textura
                // Criar framebuffer temporário para redimensionar
                static GLuint outputFramebuffer = 0;
                static GLuint outputTexture = 0;
                static uint32_t lastOutputWidth = 0;
                static uint32_t lastOutputHeight = 0;

                // Recriar framebuffer se necessário
                if (outputFramebuffer == 0 || outputTexture == 0 ||
                    lastOutputWidth != m_outputWidth || lastOutputHeight != m_outputHeight)
                {
                    // Limpar recursos antigos
                    if (outputFramebuffer != 0)
                    {
                        glDeleteFramebuffers(1, &outputFramebuffer);
                        outputFramebuffer = 0;
                    }
                    if (outputTexture != 0)
                    {
                        glDeleteTextures(1, &outputTexture);
                        outputTexture = 0;
                    }

                    // Criar nova textura
                    glGenTextures(1, &outputTexture);
                    glBindTexture(GL_TEXTURE_2D, outputTexture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_outputWidth, m_outputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                    // Criar framebuffer
                    glGenFramebuffers(1, &outputFramebuffer);
                    glBindFramebuffer(GL_FRAMEBUFFER, outputFramebuffer);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);

                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    {
                        LOG_ERROR("Failed to create output resolution framebuffer");
                        glDeleteFramebuffers(1, &outputFramebuffer);
                        glDeleteTextures(1, &outputTexture);
                        outputFramebuffer = 0;
                        outputTexture = 0;
                    }
                    else
                    {
                        lastOutputWidth = m_outputWidth;
                        lastOutputHeight = m_outputHeight;
                        LOG_INFO("Output resolution framebuffer created: " +
                                 std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight));
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }

                // Renderizar textura original para o framebuffer de saída (redimensionando)
                if (outputFramebuffer != 0 && outputTexture != 0)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, outputFramebuffer);
                    glViewport(0, 0, m_outputWidth, m_outputHeight);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);

                    // Renderizar textura original redimensionada.
                    // enableBlend=false: shaders RetroArch frequentemente escrevem
                    // vec4(rgb, 0.0); com blending habilitado (SRC_ALPHA × 0) o destino
                    // limpo a (0,0,0,0) gera preto. Reproduzimos o comportamento do
                    // RetroArch que ignora o alpha e mostra o RGB direto.
                    m_renderer->renderTexture(textureToRender, m_outputWidth, m_outputHeight,
                                              false, false, 1.0f, 1.0f,
                                              false, renderWidth, renderHeight);

                    glBindFramebuffer(GL_FRAMEBUFFER, 0);

                    // Usar textura redimensionada
                    finalTexture = outputTexture;
                    finalRenderWidth = m_outputWidth;
                    finalRenderHeight = m_outputHeight;
                    
                    static int outputResizeLogCount = 0;
                    if (outputResizeLogCount++ < 3)
                    {
                        LOG_INFO("Output resolution applied - Before: " + 
                                 std::to_string(renderWidth) + "x" + std::to_string(renderHeight) +
                                 ", After: " + std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                                 ", outputTexture: " + std::to_string(outputTexture));
                    }
                }
            }
            
            // Log final das resoluções após processamento
            static int finalResLogCount = 0;
            if (finalResLogCount++ < 3)
            {
                LOG_INFO("Final texture resolutions - finalTexture: " + std::to_string(finalTexture) +
                         ", finalRenderWidth: " + std::to_string(finalRenderWidth) +
                         ", finalRenderHeight: " + std::to_string(finalRenderHeight));
            }

            // Camera image and shader output both need Y inversion in
            // the general case — that's why flipY defaults to true.
            //
            // Exception: remote source consumed without a client-side
            // shader. The /raw wire data goes through one fewer Y
            // inversion than a locally-captured frame (no FrameProcessor
            // upload→shader→sample chain on the client), so the
            // renderer's implicit flip overshoots and the image lands
            // upside-down. When the user disables the client-side
            // shader pipeline on a Remote source, drop the flip so the
            // picture stays right-side-up (#67).
            const bool remoteWithoutShader =
                (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote &&
                 !isShaderTexture);
            bool shouldFlipY = !remoteWithoutShader;

            // Calculate viewport where capture will be rendered (may be smaller than window if maintainAspect is active)
            uint32_t windowWidth = m_window->getWidth();
            uint32_t windowHeight = m_window->getHeight();
            GLint viewportX = 0;
            GLint viewportY = 0;
            GLsizei viewportWidth = windowWidth;
            GLsizei viewportHeight = windowHeight;

            if (m_maintainAspect && finalRenderWidth > 0 && finalRenderHeight > 0)
            {
                // Calculate texture and window aspect ratio (same as renderTexture)
                float textureAspect = static_cast<float>(finalRenderWidth) / static_cast<float>(finalRenderHeight);
                float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);

                if (textureAspect > windowAspect)
                {
                    // Texture is wider: adjust height (letterboxing)
                    viewportHeight = static_cast<GLsizei>(windowWidth / textureAspect);
                    viewportY = (windowHeight - viewportHeight) / 2;
                }
                else
                {
                    // Texture is taller: adjust width (pillarboxing)
                    viewportWidth = static_cast<GLsizei>(windowHeight * textureAspect);
                    viewportX = (windowWidth - viewportWidth) / 2;
                }
            }

            // IMPORTANTE: Garantir que estamos renderizando no framebuffer padrão (janela)
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Renderizar textura final na janela (sempre preenche a janela completamente).
            // enableBlend=false pelo mesmo motivo do resize acima.
            m_renderer->renderTexture(finalTexture, m_window->getWidth(), m_window->getHeight(),
                                      shouldFlipY, false, m_brightness, m_contrast,
                                      m_maintainAspect, finalRenderWidth, finalRenderHeight);

            // IMPORTANTE: Para streaming e recording, capturar diretamente da textura final ao invés do framebuffer
            // Isso evita problemas com back/front buffer e garante que capturamos a imagem renderizada
            bool needsFrameCapture = (m_streamManager && m_streamManager->isActive()) ||
                                    (m_recordingManager && m_recordingManager->isRecording());

            // Log para debug: verificar tamanho da textura final antes da captura
            static int finalTextureSizeLogCount = 0;
            if (needsFrameCapture && finalTextureSizeLogCount++ < 3)
            {
                LOG_INFO("Frame capture: finalTexture=" + std::to_string(finalTexture) + 
                         ", finalRenderWidth=" + std::to_string(finalRenderWidth) + 
                         ", finalRenderHeight=" + std::to_string(finalRenderHeight) +
                         ", renderWidth=" + std::to_string(renderWidth) +
                         ", renderHeight=" + std::to_string(renderHeight) +
                         ", outputWidth=" + std::to_string(m_outputWidth) +
                         ", outputHeight=" + std::to_string(m_outputHeight) +
                         ", textureToRender=" + std::to_string(textureToRender));
            }

            if (needsFrameCapture)
            {

                // Capturar do viewport (o que está sendo renderizado)
                uint32_t captureWidth = static_cast<uint32_t>(viewportWidth);
                uint32_t captureHeight = static_cast<uint32_t>(viewportHeight);
                size_t captureDataSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 3;

                if (captureDataSize > 0 && captureDataSize <= (7680 * 4320 * 3) &&
                    captureWidth > 0 && captureHeight > 0 && captureWidth <= 7680 && captureHeight <= 4320)
                {
                    // SOLUÇÃO PARA DIRECTFB: Capturar diretamente da textura final usando FBO
                    // Isso evita problemas com back/front buffer do framebuffer padrão
                    // que não funciona corretamente com DirectFB

                    // Salvar FBO atual
                    GLint previousFBO = 0;
                        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);

                        // Verificar se a textura é válida e as dimensões são válidas antes de tentar capturar
                        if (finalTexture == 0)
                        {
                            static int textureWarningCount = 0;
                            if (textureWarningCount++ < 3)
                            {
                                LOG_WARN("Frame capture: finalTexture is 0, cannot capture");
                            }
                        }
                        else if (finalRenderWidth == 0 || finalRenderHeight == 0)
                        {
                            static int dimensionWarningCount = 0;
                            if (dimensionWarningCount++ < 3)
                            {
                                LOG_WARN("Frame capture: Invalid dimensions (" +
                                         std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight) +
                                         "), cannot capture");
                            }
                        }
                        else
                        {
                            // Criar FBO temporário para ler da textura
                        GLuint captureFBO = 0;
                        glGenFramebuffers(1, &captureFBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
                        
                        // IMPORTANTE: Para gravação, capturar da textura APÓS o shader e APÓS o redimensionamento de saída (se configurado)
                        // Isso garante que capturamos a imagem completa processada com o shader aplicado
                        // O MediaEncoder fará o redimensionamento final para a resolução de gravação se necessário
                        GLuint textureToCapture = finalTexture;
                        uint32_t captureTextureWidth = finalRenderWidth;
                        uint32_t captureTextureHeight = finalRenderHeight;
                        
                        // Se estamos gravando ou fazendo streaming, determinar qual textura e dimensões usar
                        // IMPORTANTE: Se há resolução de saída configurada, usar finalTexture
                        // Se não há resolução de saída mas há shader, usar textureToRender com dimensões do shader
                        // Isso garante que capturamos a textura completa processada tanto para streaming quanto para gravação
                        bool needsFrameCapture = (m_recordingManager && m_recordingManager->isRecording()) ||
                                                (m_streamManager && m_streamManager->isActive());
                        
                        if (needsFrameCapture)
                        {
                            // IMPORTANTE: Se há uma resolução de saída configurada, usar finalTexture que já foi redimensionada
                            // Se não há resolução de saída, mas há shader, usar textureToRender com as dimensões reais do shader
                            // Isso garante que capturamos a textura completa processada
                            if (m_outputWidth > 0 && m_outputHeight > 0)
                            {
                                // Resolução de saída configurada: usar finalTexture que já foi redimensionada
                                textureToCapture = finalTexture;
                                captureTextureWidth = finalRenderWidth;
                                captureTextureHeight = finalRenderHeight;
                            }
                            else if (isShaderTexture)
                            {
                                // Sem resolução de saída, mas com shader: usar textureToRender com dimensões reais do shader
                                textureToCapture = textureToRender;
                                captureTextureWidth = m_shaderEngine->getOutputWidth();
                                captureTextureHeight = m_shaderEngine->getOutputHeight();
                                
                                // Se dimensões do shader são inválidas, usar renderWidth/renderHeight
                                if (captureTextureWidth == 0 || captureTextureHeight == 0)
                                {
                                    captureTextureWidth = renderWidth;
                                    captureTextureHeight = renderHeight;
                                }
                            }
                            else
                            {
                                // Sem shader e sem resolução de saída: usar finalTexture
                                textureToCapture = finalTexture;
                                captureTextureWidth = finalRenderWidth;
                                captureTextureHeight = finalRenderHeight;
                            }
                            
                            static int captureSourceLogCount = 0;
                            if (captureSourceLogCount++ < 3)
                            {
                                LOG_INFO("=== FRAME CAPTURE DEBUG ===");
                                LOG_INFO("Original capture: " + 
                                         std::to_string(m_frameProcessor->getTextureWidth()) + "x" + std::to_string(m_frameProcessor->getTextureHeight()));
                                if (isShaderTexture)
                                {
                                    LOG_INFO("Shader engine output: " + 
                                             std::to_string(m_shaderEngine->getOutputWidth()) + "x" + std::to_string(m_shaderEngine->getOutputHeight()));
                                }
                                LOG_INFO("renderWidth/Height: " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight));
                                LOG_INFO("finalRenderWidth/Height: " + std::to_string(finalRenderWidth) + "x" + std::to_string(finalRenderHeight));
                                LOG_INFO("Output resolution: " + std::to_string(m_outputWidth) + "x" + std::to_string(m_outputHeight));
                                
                                if (m_recordingManager && m_recordingManager->isRecording())
                                {
                                    RecordingSettings recSettings = m_recordingManager->getRecordingSettings();
                                    LOG_INFO("Recording resolution: " + std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                                }
                                if (m_streamManager && m_streamManager->isActive() && m_ui)
                                {
                                    LOG_INFO("Streaming resolution: " + std::to_string(m_ui->getStreamingWidth()) + "x" + std::to_string(m_ui->getStreamingHeight()));
                                }
                                
                                LOG_INFO("Selected - textureToCapture: " + std::to_string(textureToCapture) +
                                         ", size: " + std::to_string(captureTextureWidth) + "x" + std::to_string(captureTextureHeight));
                                LOG_INFO("Textures - textureToRender: " + std::to_string(textureToRender) +
                                         ", finalTexture: " + std::to_string(finalTexture));
                                LOG_INFO("===========================");
                            }
                        }
                        
                        // Anexar a textura escolhida ao FBO
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureToCapture, 0);

                            // Verificar se o FBO está completo
                            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                            if (status != GL_FRAMEBUFFER_COMPLETE)
                            {
                                LOG_WARN("Frame capture: FBO incomplete, falling back to default framebuffer");
                                glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                                glDeleteFramebuffers(1, &captureFBO);

                                // Fallback: tentar capturar do framebuffer padrão
                                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                                glViewport(0, 0, windowWidth, windowHeight);
                                GLint readY = static_cast<GLint>(windowHeight) - viewportY - static_cast<GLint>(viewportHeight);

                                uint32_t actualCaptureWidth = static_cast<uint32_t>(viewportWidth);
                                uint32_t actualCaptureHeight = static_cast<uint32_t>(viewportHeight);
                                size_t rgbDataSize = static_cast<size_t>(actualCaptureWidth) * static_cast<size_t>(actualCaptureHeight) * 3;
                                size_t readRowSizeUnpadded = static_cast<size_t>(actualCaptureWidth) * 3;
                                size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
                                size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(actualCaptureHeight);

                                auto &frameDataWithPadding = m_captureSyncPadded;
                                frameDataWithPadding.resize(totalSizeWithPadding);

                                glReadPixels(viewportX, readY, static_cast<GLsizei>(actualCaptureWidth), static_cast<GLsizei>(actualCaptureHeight),
                                             GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

                                // Converter dados (mesmo código abaixo)
                                auto &frameData = m_captureFrameData;
                                frameData.resize(rgbDataSize);
                                for (uint32_t row = 0; row < actualCaptureHeight; row++)
                                {
                                    uint32_t srcRow = actualCaptureHeight - 1 - row;
                                    uint32_t dstRow = row;
                                    const uint8_t *srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
                                    uint8_t *dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
                                    memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
                                }

                                // Push every frame produced by this iteration. The main
                                // loop is already paced (see the render-pace blocks at the
                                // end of the loop), so the per-iteration rate matches the
                                // configured streaming FPS. The earlier dedicated push
                                // throttle here was meant to protect against an uncapped
                                // main loop on a high-refresh display, but with main-loop
                                // pacing in place it just rejected legitimate frames whose
                                // arrival jittered slightly below 1/fps (observed as
                                // "push throttle skips=20-24/s" while the encoder sat
                                // idle waiting for input).
                                if (m_streamManager && m_streamManager->isActive())
                                {
                                    m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                }
                                if (m_recordingManager && m_recordingManager->isRecording())
                                {
                                    m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                }
                            }
                            else
                            {
                                // FBO está completo, continuar com captura da textura

                                // IMPORTANTE: ShaderEngine cria texturas em GL_RGBA, não GL_RGB
                                // Precisamos ler como RGBA e converter para RGB depois
                                // Master pipeline toggle off → applyShader was skipped above and
                                // textureToCapture is the source RGB texture, so treat as RGB.
                                const bool pipelineEnabled = (!m_ui || m_ui->getShaderPipelineEnabled());
                                bool isShaderTexture = (pipelineEnabled && m_shaderEngine && m_shaderEngine->isShaderActive());
                                GLenum readFormat = isShaderTexture ? GL_RGBA : GL_RGB;
                                uint32_t bytesPerPixel = isShaderTexture ? 4 : 3;

                                static int formatLogCount = 0;
                                if (formatLogCount++ < 3)
                                {
                                    LOG_INFO("Frame capture: Using format " + std::string(isShaderTexture ? "RGBA" : "RGB") +
                                             " for texture " + std::to_string(finalTexture) +
                                             " (shader active: " + std::string(isShaderTexture ? "yes" : "no") + ")");
                                }

                            // IMPORTANTE: Capturar a textura COMPLETA, não apenas uma parte
                            // Usar as dimensões e textura determinadas acima (pode ser finalTexture ou textureToRender)
                            uint32_t textureWidth = captureTextureWidth;
                            uint32_t textureHeight = captureTextureHeight;
                            
                            // Log detalhado: verificar tamanho da textura vs resolução de gravação/streaming
                            static int textureSizeLogCount = 0;
                            bool shouldLog = (textureSizeLogCount++ < 3) && 
                                            ((m_recordingManager && m_recordingManager->isRecording()) ||
                                             (m_streamManager && m_streamManager->isActive()));
                            if (shouldLog)
                            {
                                LOG_INFO("=== CAPTURE DETAILS ===");
                                LOG_INFO("Capturing from texture: " + std::to_string(textureToCapture) +
                                         ", Size: " + std::to_string(textureWidth) + "x" + std::to_string(textureHeight));
                                if (m_recordingManager && m_recordingManager->isRecording())
                                {
                                    RecordingSettings recSettings = m_recordingManager->getRecordingSettings();
                                    LOG_INFO("Recording target: " + 
                                             std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                                    LOG_INFO("Will resize for recording: " + std::string(
                                        (textureWidth != recSettings.width || textureHeight != recSettings.height) ? "YES" : "NO"));
                                }
                                if (m_streamManager && m_streamManager->isActive() && m_ui)
                                {
                                    LOG_INFO("Streaming target: " + 
                                             std::to_string(m_ui->getStreamingWidth()) + "x" + std::to_string(m_ui->getStreamingHeight()));
                                    LOG_INFO("Will resize for streaming: " + std::string(
                                        (textureWidth != m_ui->getStreamingWidth() || textureHeight != m_ui->getStreamingHeight()) ? "YES" : "NO"));
                                }
                                LOG_INFO("======================");
                            }
                            
                            size_t rgbDataSize = static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 3;

                            // glReadPixels lê do FBO atual (com textura anexada). O viewport
                            // do FBO precisa ser exatamente o tamanho da textura.
                            glViewport(0, 0, textureWidth, textureHeight);

                            static int viewportCheckCount = 0;
                            if (viewportCheckCount++ < 3)
                            {
                                GLint currentViewport[4];
                                glGetIntegerv(GL_VIEWPORT, currentViewport);
                                LOG_INFO("Frame capture: Viewport set to " +
                                             std::to_string(textureWidth) + "x" + std::to_string(textureHeight) +
                                             ", actual viewport: [" + std::to_string(currentViewport[0]) + "," +
                                             std::to_string(currentViewport[1]) + "," +
                                             std::to_string(currentViewport[2]) + "x" + std::to_string(currentViewport[3]) + "]");
                            }

                            auto &frameData = m_captureFrameData;
                            bool frameDataReady = false;

                            // Decidir entre PBO async e leitura síncrona ANTES de tocar no FBO.
                            // O PBO precisa do FBO bound durante startAsyncRead; o sync precisa
                            // do FBO bound durante glReadPixels. Os dois caminhos divergem aqui.
                            bool useAsyncPBO = (m_pboManager &&
                                                m_pboManager->init(textureWidth, textureHeight, readFormat) &&
                                                m_pboManager->isInitialized());

                            if (useAsyncPBO)
                            {
                                // Agenda glReadPixels no PBO (não bloqueia).
                                m_pboManager->startAsyncRead(0, 0,
                                                             static_cast<GLsizei>(textureWidth),
                                                             static_cast<GLsizei>(textureHeight));

                                // FBO já não é mais necessário — glReadPixels foi agendado.
                                glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                                glDeleteFramebuffers(1, &captureFBO);

                                // Tenta obter os dados do frame ANTERIOR (já transferidos da GPU).
                                // Os primeiros 1-2 frames pós-init não têm dados ainda — drop.
                                if (bytesPerPixel == 3)
                                {
                                    frameData.resize(rgbDataSize);
                                    frameDataReady = m_pboManager->getReadData(
                                        frameData.data(), textureWidth, textureHeight, /*flipY=*/false);
                                }
                                else
                                {
                                    auto &rgbaData = m_captureRgbaScratch;
                                    rgbaData.resize(static_cast<size_t>(textureWidth) *
                                                    static_cast<size_t>(textureHeight) * 4);
                                    if (m_pboManager->getReadData(rgbaData.data(),
                                                                  textureWidth, textureHeight,
                                                                  /*flipY=*/false))
                                    {
                                        frameData.resize(rgbDataSize);
                                        for (uint32_t row = 0; row < textureHeight; row++)
                                        {
                                            const uint8_t *src = rgbaData.data() + (row * textureWidth * 4);
                                            uint8_t *dst = frameData.data() + (row * textureWidth * 3);
                                            for (uint32_t col = 0; col < textureWidth; col++)
                                            {
                                                dst[col * 3 + 0] = src[col * 4 + 0];
                                                dst[col * 3 + 1] = src[col * 4 + 1];
                                                dst[col * 3 + 2] = src[col * 4 + 2];
                                            }
                                        }
                                        frameDataReady = true;
                                    }
                                }
                            }
                            else
                            {
                                // Caminho síncrono (fallback quando PBO não está disponível).
                                size_t syncRowUnpadded = static_cast<size_t>(textureWidth) * bytesPerPixel;
                                size_t syncRowPadded = ((syncRowUnpadded + 3) / 4) * 4;
                                auto &frameDataWithPadding = m_captureSyncPadded;
                                frameDataWithPadding.resize(syncRowPadded * static_cast<size_t>(textureHeight));

                                glReadPixels(0, 0,
                                             static_cast<GLsizei>(textureWidth),
                                             static_cast<GLsizei>(textureHeight),
                                             readFormat, GL_UNSIGNED_BYTE,
                                             frameDataWithPadding.data());

                                glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);
                                glDeleteFramebuffers(1, &captureFBO);

                                frameData.resize(rgbDataSize);
                                for (uint32_t row = 0; row < textureHeight; row++)
                                {
                                    const uint8_t *srcPtr = frameDataWithPadding.data() + (row * syncRowPadded);
                                    uint8_t *dstPtr = frameData.data() + (row * textureWidth * 3);

                                    if (isShaderTexture)
                                    {
                                        for (uint32_t col = 0; col < textureWidth; col++)
                                        {
                                            dstPtr[col * 3 + 0] = srcPtr[col * 4 + 0];
                                            dstPtr[col * 3 + 1] = srcPtr[col * 4 + 1];
                                            dstPtr[col * 3 + 2] = srcPtr[col * 4 + 2];
                                        }
                                    }
                                    else
                                    {
                                        memcpy(dstPtr, srcPtr, textureWidth * 3);
                                    }
                                }
                                frameDataReady = true;
                            }

                            // Usar dimensões originais da textura
                            // O MediaEncoder fará o redimensionamento para a resolução de gravação/streaming se necessário
                            uint32_t actualCaptureWidth = textureWidth;
                            uint32_t actualCaptureHeight = textureHeight;
                            
                            // Verificar se o frame capturado está vazio/preto
                            // Isso ajuda a diagnosticar problemas com DirectFB
                            static int frameCheckCount = 0;
                                if (frameCheckCount++ < 10 || frameCheckCount % 60 == 0)
                                {
                                    // Verificar se todos os pixels são pretos (0,0,0) ou se há dados válidos
                                    size_t blackPixelCount = 0;
                                    size_t totalPixels = static_cast<size_t>(actualCaptureWidth) * static_cast<size_t>(actualCaptureHeight);
                                    size_t sampleSize = std::min(totalPixels, static_cast<size_t>(1000)); // Amostrar até 1000 pixels

                                    for (size_t i = 0; i < sampleSize; i++)
                                    {
                                        size_t pixelIdx = (i * totalPixels) / sampleSize; // Amostragem uniforme
                                        size_t byteIdx = pixelIdx * 3;
                                        if (byteIdx + 2 < frameData.size())
                                        {
                                            if (frameData[byteIdx] == 0 &&
                                                frameData[byteIdx + 1] == 0 &&
                                                frameData[byteIdx + 2] == 0)
                                            {
                                                blackPixelCount++;
                                            }
                                        }
                                    }

                                    double blackRatio = static_cast<double>(blackPixelCount) / static_cast<double>(sampleSize);
                                    if (blackRatio > 0.95 && frameCheckCount <= 10)
                                    {
                                        LOG_WARN("Frame capture: " + std::to_string(static_cast<int>(blackRatio * 100)) +
                                                 "% of sampled pixels are black (may indicate DirectFB/framebuffer issue)");
                                        LOG_WARN("Capture params: texture=" + std::to_string(textureToCapture) +
                                                 ", size=" + std::to_string(actualCaptureWidth) + "x" + std::to_string(actualCaptureHeight));
                                    }
                                }

                                // Per-pipeline shader override: when the master is on AND
                                // the shader is active, individual pipelines can request the
                                // raw pre-shader source instead. We do a sync readback of
                                // m_frameProcessor->getTexture() (raw V4L2 RGB upload) only
                                // when at least one active pipeline asked for it — keeps the
                                // common case (both pipelines follow the master) on the fast
                                // single-readback path.
                                const bool masterOn = (m_ui && m_ui->getShaderPipelineEnabled());
                                const bool shaderActive = (m_shaderEngine && m_shaderEngine->isShaderActive());
                                const bool streamWantsSource = m_ui && m_streamManager && m_streamManager->isActive() &&
                                                               !m_ui->getStreamingApplyShader();
                                const bool recordWantsSource = m_ui && m_recordingManager && m_recordingManager->isRecording() &&
                                                               !m_ui->getRecordingApplyShader();
                                // Phase 2 of #47: /raw is by-contract pre-shader, so any connected
                                // /raw client also triggers the source-frame capture below.
                                const bool rawWantsSource = m_streamManager && m_streamManager->isActive() &&
                                                            m_streamManager->hasRawClients();
                                const bool needSourceCapture = masterOn && shaderActive &&
                                                               (streamWantsSource || recordWantsSource || rawWantsSource);

                                bool sourceFrameReady = false;
                                uint32_t sourceFrameW = 0, sourceFrameH = 0;
                                if (needSourceCapture && m_frameProcessor)
                                {
                                    GLuint srcTex = m_frameProcessor->getTexture();
                                    sourceFrameW = m_frameProcessor->getTextureWidth();
                                    sourceFrameH = m_frameProcessor->getTextureHeight();
                                    if (srcTex && sourceFrameW > 0 && sourceFrameH > 0)
                                    {
                                        GLuint tmpFBO = 0;
                                        glGenFramebuffers(1, &tmpFBO);
                                        GLint prevFBO = 0;
                                        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
                                        glBindFramebuffer(GL_FRAMEBUFFER, tmpFBO);
                                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
                                        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                                        {
                                            const size_t rowUnpadded = static_cast<size_t>(sourceFrameW) * 3;
                                            const size_t rowPadded = ((rowUnpadded + 3) / 4) * 4;
                                            m_captureSourcePadded.resize(rowPadded * sourceFrameH);
                                            glReadPixels(0, 0, static_cast<GLsizei>(sourceFrameW),
                                                         static_cast<GLsizei>(sourceFrameH),
                                                         GL_RGB, GL_UNSIGNED_BYTE, m_captureSourcePadded.data());
                                            // glReadPixels returns rows bottom-up; flip + strip row padding.
                                            m_captureSourceFrameData.resize(rowUnpadded * sourceFrameH);
                                            for (uint32_t row = 0; row < sourceFrameH; ++row)
                                            {
                                                const uint32_t srcRow = sourceFrameH - 1 - row;
                                                const uint8_t *s = m_captureSourcePadded.data() + srcRow * rowPadded;
                                                uint8_t *d = m_captureSourceFrameData.data() + row * rowUnpadded;
                                                memcpy(d, s, rowUnpadded);
                                            }
                                            sourceFrameReady = true;
                                        }
                                        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
                                        glDeleteFramebuffers(1, &tmpFBO);
                                    }
                                }

                                // Share frame data between streaming and recording.
                                // Pula push se o PBO async ainda não tem dados do frame anterior.
                                // Push every frame the FBO path produces. Main-loop pacing
                                // (added in cd7b13a / 4b69c72) already caps the iteration rate
                                // at the configured streaming FPS, so the per-frame interval
                                // matches the target and there's no risk of overshooting the
                                // way an uncapped 240 Hz refresh free-run did. An earlier
                                // dedicated throttle here ended up rejecting ~30 % of
                                // legitimate frames every second because per-iteration work
                                // time jittered slightly below 1/fps — visible in the log as
                                // "push throttle: pushes=37/s skips=24/s" while VAAPI sat idle
                                // waiting for input.
                                if (frameDataReady && m_streamManager && m_streamManager->isActive())
                                {
                                    const bool useSource = streamWantsSource && sourceFrameReady;
                                    static int streamPushLogCount = 0;
                                    if (streamPushLogCount++ < 3 && m_ui)
                                    {
                                        LOG_INFO("--- PUSHING FRAME TO STREAMING ---");
                                        LOG_INFO("Frame size being pushed: " + std::to_string(useSource ? sourceFrameW : actualCaptureWidth) + "x" + std::to_string(useSource ? sourceFrameH : actualCaptureHeight) +
                                                 (useSource ? " (raw source — shader bypassed)" : ""));
                                        LOG_INFO("Streaming target resolution: " + std::to_string(m_ui->getStreamingWidth()) + "x" + std::to_string(m_ui->getStreamingHeight()));
                                        LOG_INFO("----------------------------------");
                                    }
                                    if (useSource)
                                    {
                                        m_streamManager->pushFrame(m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                                    }
                                    else
                                    {
                                        m_streamManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                    }

                                    // Phase 2 of #47: also feed the /raw output (pre-shader, always).
                                    // hasRawClients() gates this so the encoder idles when nothing
                                    // is listening — the CPU cost only shows up when a remote
                                    // client is actually consuming the raw feed.
                                    //
                                    // Two code paths because the pre-shader pixels live in
                                    // different buffers depending on whether the shader chain
                                    // is running this frame:
                                    //   - shader active → m_captureSourceFrameData (an extra
                                    //     readback from the FrameProcessor texture).
                                    //   - shader off (master toggle disabled or no preset
                                    //     loaded) → frameData IS already the pre-shader pixels,
                                    //     so we feed /raw from there. Without this branch,
                                    //     disabling the shader from the UI silently kills
                                    //     /raw transmission (#67 — client log showed video
                                    //     stop while audio kept flowing).
                                    if (m_streamManager->hasRawClients())
                                    {
                                        if (sourceFrameReady)
                                        {
                                            m_streamManager->pushRawFrame(m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                                        }
                                        else if (!masterOn || !shaderActive)
                                        {
                                            m_streamManager->pushRawFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                        }
                                    }
                                }
                                if (frameDataReady && m_recordingManager && m_recordingManager->isRecording())
                                {
                                    const bool useSource = recordWantsSource && sourceFrameReady;
                                    static int recordingPushLogCount = 0;
                                    if (recordingPushLogCount++ < 3)
                                    {
                                        LOG_INFO("=== PUSHING FRAME TO RECORDING ===");
                                        LOG_INFO("Frame size being pushed: " + std::to_string(useSource ? sourceFrameW : actualCaptureWidth) + "x" + std::to_string(useSource ? sourceFrameH : actualCaptureHeight) +
                                                 (useSource ? " (raw source — shader bypassed)" : ""));
                                        RecordingSettings recSettings = m_recordingManager->getRecordingSettings();
                                        LOG_INFO("Recording target resolution: " + std::to_string(recSettings.width) + "x" + std::to_string(recSettings.height));
                                        LOG_INFO("===================================");
                                    }
                                    if (useSource)
                                    {
                                        m_recordingManager->pushFrame(m_captureSourceFrameData.data(), sourceFrameW, sourceFrameH);
                                    }
                                    else
                                    {
                                        m_recordingManager->pushFrame(frameData.data(), actualCaptureWidth, actualCaptureHeight);
                                    }
                                }
                        } // fim do else (textura válida)
                    } // fim do else (FBO completo)
                } // fim do if (needsFrameCapture)
            } // fim do if (captureDataSize > 0)

            auto streamManager = m_streamManager.get();
            if (m_ui && streamManager && streamManager->isActive())
            {
                m_ui->setStreamingActive(true);
                auto urls = streamManager->getStreamUrls();
                if (!urls.empty())
                {
                    m_ui->setStreamUrl(urls[0]);
                }
                uint32_t clientCount = streamManager->getTotalClientCount();
                m_ui->setStreamClientCount(clientCount);

                // Update cooldown (if active, can start and there's no cooldown)
                m_ui->setCanStartStreaming(true);
                m_ui->setStreamingCooldownRemainingMs(0);

                // Update SSL certificate information if HTTPS is active
                std::string foundCert = streamManager->getFoundSSLCertificatePath();
                std::string foundKey = streamManager->getFoundSSLKeyPath();

                if (m_webPortalHTTPSEnabled && !foundCert.empty())
                {
                    m_foundSSLCertPath = foundCert;
                    m_foundSSLKeyPath = foundKey;
                    m_ui->setFoundSSLCertificatePath(foundCert);
                    m_ui->setFoundSSLKeyPath(foundKey);
                }
                else
                {
                    // Clear found paths if HTTPS is not active
                    m_foundSSLCertPath.clear();
                    m_foundSSLKeyPath.clear();
                    m_ui->setFoundSSLCertificatePath("");
                    m_ui->setFoundSSLKeyPath("");
                }
            }
            else if (m_ui)
            {
                m_ui->setStreamingActive(false);
                m_ui->setStreamUrl("");
                m_ui->setStreamClientCount(0);

                // Update StreamManager cooldown if available
                if (streamManager)
                {
                    bool canStart = streamManager->canStartStreaming();
                    int64_t cooldownMs = streamManager->getStreamingCooldownRemainingMs();
                    m_ui->setCanStartStreaming(canStart);
                    m_ui->setStreamingCooldownRemainingMs(cooldownMs);
                }
                else
                {
                    // If no StreamManager, can start
                    m_ui->setCanStartStreaming(true);
                    m_ui->setStreamingCooldownRemainingMs(0);
                }
            }

            // Update recording status
            if (m_ui && m_recordingManager)
            {
                bool isRecording = m_recordingManager->isRecording();
                m_ui->setRecordingActive(isRecording);
                if (isRecording)
                {
                    m_ui->setRecordingDurationUs(m_recordingManager->getCurrentDurationUs());
                    m_ui->setRecordingFileSize(m_recordingManager->getCurrentFileSize());
                    m_ui->setRecordingFilename(m_recordingManager->getCurrentFilename());
                }
            }

            // Render UI (after capturing capture area)
            if (m_ui)
            {
                m_ui->render();
                // IMPORTANT: endFrame() must be called BEFORE swapBuffers()
                // so UI is rendered to correct buffer
                m_ui->endFrame();
            }

            // Check if window is still valid before swapping
            if (m_window && !m_window->shouldClose())
            {
                m_window->swapBuffers();
            }

            // Pacing policy:
            //  - Remote source: vsync drives the loop at the panel's
            //    display refresh rate. Vsync is toggled on focus —
            //    when the window is backgrounded a lot of compositors
            //    park vsync at 0 Hz, so swapBuffers would block forever
            //    and the main loop would stop draining the frame queue
            //    (the user-observed "consumed=0 drops=60/s queueDepth=20"
            //    stall). When unfocused we disable vsync and add a
            //    small sleep to avoid burning CPU on hidden refreshes.
            //  - Local source streaming: keep the existing software
            //    pace at the configured streaming FPS so we don't
            //    burn GPU + glReadPixels for frames the encoder will
            //    just throttle.
            if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote)
            {
                const bool focused = m_window && m_window->isFocused();
                if (m_window && focused != m_remoteWindowFocused)
                {
                    m_window->setVsync(focused);
                    m_remoteWindowFocused = focused;
                }
                if (!focused)
                {
                    // Hidden / backgrounded — sleep a frame's worth so
                    // captureLatestFrame still drains the queue at a
                    // reasonable rate without spinning the CPU.
#ifdef PLATFORM_LINUX
                    usleep(16000);
#else
                    Sleep(16);
#endif
                }
                // When focused, vsync does the pacing.
            }
            else
            {
                bool streamingActive = (m_streamManager && m_streamManager->isActive());
                if (streamingActive && m_ui)
                {
                    const uint32_t targetFps = m_ui->getStreamingFps() > 0 ? m_ui->getStreamingFps() : 60;
                    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch()).count();
                    const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(targetFps);
                    if (m_lastFrameSwapUs == 0) m_lastFrameSwapUs = nowUs;
                    const int64_t elapsedUs = nowUs - m_lastFrameSwapUs;
                    if (elapsedUs < targetIntervalUs)
                    {
                        const int64_t sleepUs = targetIntervalUs - elapsedUs;
#ifdef PLATFORM_LINUX
                        usleep(static_cast<useconds_t>(sleepUs));
#else
                        Sleep(static_cast<DWORD>(sleepUs / 1000));
#endif
                    }
                    m_lastFrameSwapUs = nowUs + std::max<int64_t>(0, targetIntervalUs - elapsedUs);
                }
            }
        }
        else
        {
            // If no valid frame yet, we still need to render UI and update window
            // so window is visible even without video frame

            // Check if window is still valid before operations
            if (!m_window || m_window->shouldClose())
            {
                continue;
            }

            // Clear framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
            uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

            if (currentWidth > 0 && currentHeight > 0)
            {
                glViewport(0, 0, currentWidth, currentHeight);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            // IMPORTANT: Always finalize ImGui frame, even if we don't render anything
            // This avoids the error "Forgot to call Render() or EndFrame()"
            if (m_ui)
            {
                m_ui->render();
                m_ui->endFrame();
            }

            // IMPORTANT: Always do swapBuffers so window is updated and visible
            // But check if window is still valid first
            if (m_window && !m_window->shouldClose())
            {
                m_window->swapBuffers();
            }

            // Remote-source render pacing: when this client is consuming a
            // remote /raw stream, there's no point iterating faster than
            // the host's source FPS. Without this cap an idle 144 / 240 /
            // 360 Hz display will spin the main loop at hundreds of fps,
            // re-rendering the same decoded frame and re-running
            // applyPendingRemoteMeta — wastes GPU and produces the
            // 500-fps reading the user spotted in MangoHud. Sleep until
            // the next host-frame deadline.
            //
            // Fall back to 60 fps when /meta hasn't arrived yet or reports
            // fps=0 — anything's better than the 1 ms-sleep free-run that
            // produced the 500 fps reading we're trying to fix.
            if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Remote)
            {
                const uint32_t fps = m_remoteSourceFps > 0 ? m_remoteSourceFps : 60;
                const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch()).count();
                const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(fps);
                if (m_lastFrameSwapUs == 0)
                {
                    m_lastFrameSwapUs = nowUs;
                }
                const int64_t elapsedUs = nowUs - m_lastFrameSwapUs;

                if (elapsedUs < targetIntervalUs)
                {
                    const int64_t sleepUs = targetIntervalUs - elapsedUs;
#ifdef PLATFORM_LINUX
                    usleep(static_cast<useconds_t>(sleepUs));
#else
                    Sleep(static_cast<DWORD>(sleepUs / 1000));
#endif
                }

                m_lastFrameSwapUs = nowUs + std::max<int64_t>(0, targetIntervalUs - elapsedUs);
            }
            else
            {
                // Local-source path (V4L2 / DS / None / no remote source
                // info yet). When streaming is active, pace the main loop
                // to match the configured streaming FPS so we don't burn
                // GPU + glReadPixels work re-rendering a frame the encoder
                // throttle will discard anyway. When streaming is idle,
                // fall back to the existing 1 ms sleep so the local
                // preview stays responsive on whatever refresh rate the
                // display happens to run at.
                bool streamingActive = (m_streamManager && m_streamManager->isActive());
                if (streamingActive && m_ui)
                {
                    const uint32_t targetFps = m_ui->getStreamingFps() > 0 ? m_ui->getStreamingFps() : 60;
                    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch()).count();
                    const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(targetFps);
                    if (m_lastFrameSwapUs == 0) m_lastFrameSwapUs = nowUs;
                    const int64_t elapsedUs = nowUs - m_lastFrameSwapUs;
                    if (elapsedUs < targetIntervalUs)
                    {
                        const int64_t sleepUs = targetIntervalUs - elapsedUs;
#ifdef PLATFORM_LINUX
                        usleep(static_cast<useconds_t>(sleepUs));
#else
                        Sleep(static_cast<DWORD>(sleepUs / 1000));
#endif
                    }
                    m_lastFrameSwapUs = nowUs + std::max<int64_t>(0, targetIntervalUs - elapsedUs);
                }
                else
                {
#ifdef PLATFORM_LINUX
                    usleep(1000);
#else
                    Sleep(1);
#endif
                }
            }
        }
    }

    LOG_INFO("Loop principal encerrado");
}

void Application::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    LOG_INFO("Shutting down Application...");

    if (m_shaderSourceTexture != 0)
    {
        glDeleteTextures(1, &m_shaderSourceTexture);
        m_shaderSourceTexture = 0;
    }
    if (m_shaderSourceFBO != 0)
    {
        glDeleteFramebuffers(1, &m_shaderSourceFBO);
        m_shaderSourceFBO = 0;
    }

    if (m_frameProcessor)
    {
        m_frameProcessor->deleteTexture();
    }

    if (m_recordingManager)
    {
        m_recordingManager->shutdown();
        m_recordingManager.reset();
    }

    if (m_capture)
    {
        m_capture->stopCapture();
        m_capture->close();
        m_capture.reset();
    }

    if (m_shaderEngine)
    {
        m_shaderEngine->shutdown();
        m_shaderEngine.reset();
    }

    if (m_frameProcessor)
    {
        m_frameProcessor.reset();
    }

    if (m_renderer)
    {
        m_renderer->shutdown();
        m_renderer.reset();
    }

    if (m_ui)
    {
        m_ui->shutdown();
        m_ui.reset();
    }

    if (m_window)
    {
        m_window->shutdown();
        m_window.reset();
    }

    // SwsContext for resize was removed - now done in encoding

    if (m_streamManager)
    {
        m_streamManager->cleanup();
        m_streamManager.reset();
    }

    if (m_audioCapture)
    {
        try
        {
            m_audioCapture->stopCapture();
        }
        catch (...)
        {
            LOG_WARN("Exception during audio capture stop - continuing");
        }
        
        try
        {
            m_audioCapture->close();
        }
        catch (...)
        {
            LOG_WARN("Exception during audio capture close - continuing");
        }
        
        m_audioCapture.reset();
    }

    m_initialized = false;
}

void Application::schedulePresetApplication(const std::string &presetName)
{
    // Thread-safe: add to queue for processing in main thread
    std::lock_guard<std::mutex> lock(m_presetQueueMutex);
    m_pendingPresets.push(presetName);
    LOG_INFO("Preset application scheduled: " + presetName);
}

void Application::scheduleResolutionChange(uint32_t width, uint32_t height)
{
    // Thread-safe: add to queue for processing in main thread
    std::lock_guard<std::mutex> lock(m_resolutionQueueMutex);
    ResolutionChange change;
    change.width = width;
    change.height = height;
    m_pendingResolutionChanges.push(change);
    LOG_INFO("Resolution change scheduled: " + std::to_string(width) + "x" + std::to_string(height));
}

void Application::applyResolutionChange(uint32_t width, uint32_t height)
{
    LOG_INFO("Resolution changed via UI: " + std::to_string(width) + "x" + std::to_string(height));
    // If no device is open, activate dummy mode
    if (!m_capture || !m_capture->isOpen())
    {
        if (!m_capture)
        {
            LOG_WARN("VideoCapture not initialized. Select a device first.");
            return;
        }

        // If not in dummy mode, try to activate
        if (!m_capture->isDummyMode())
        {
            LOG_INFO("No device open. Activating dummy mode...");
            m_capture->setDummyMode(true);
        }

        // Configure dummy format
        if (m_capture->setFormat(width, height, 0))
        {
            if (m_capture->startCapture())
            {
                LOG_INFO("Dummy resolution updated: " + std::to_string(width) + "x" + std::to_string(height));
                if (m_ui)
                {
                    m_ui->setCaptureInfo(width, height, m_captureFps, "None (Dummy)");
                    m_ui->setCurrentDevice(""); // Empty string = None
                }
                return;
            }
        }
        LOG_WARN("Failed to configure dummy resolution. Select a device first.");
        return;
    }
    if (reconfigureCapture(width, height, m_captureFps))
    {
        // Update texture if needed (use actual device values)
        uint32_t actualWidth = m_capture->getWidth();
        uint32_t actualHeight = m_capture->getHeight();

        // Texture was already deleted in reconfigureCapture before closing device
        // It will be recreated automatically on next frame processing

        // Update UI information with actual values
        if (m_ui && m_capture)
        {
            m_ui->setCaptureInfo(actualWidth, actualHeight,
                                 m_captureFps, m_devicePath);
        }

        LOG_INFO("Texture will be recreated on next frame: " +
                 std::to_string(actualWidth) + "x" + std::to_string(actualHeight));
    }
    else
    {
        // If failed, update UI with current values
        if (m_ui && m_capture)
        {
            m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                 m_captureFps, m_devicePath);
        }
    }
}

void Application::applyPreset(const std::string &presetName)
{
    if (!m_initialized)
    {
        LOG_ERROR("Cannot apply preset: Application not initialized");
        return;
    }

    PresetManager presetManager;
    PresetManager::PresetData data;
    if (!presetManager.loadPreset(presetName, data))
    {
        LOG_ERROR("Failed to load preset: " + presetName);
        return;
    }

    LOG_INFO("Applying preset: " + presetName);

    // 1. Apply shader
    if (!data.shaderPath.empty() && m_shaderEngine)
    {
        // Resolve relative shader path to absolute using centralized method
        std::string shaderPath = data.shaderPath;
        fs::path shaderPathObj(shaderPath);
        if (shaderPathObj.is_relative())
        {
            std::string resolvedPath = resolveShaderPath(shaderPath);
            if (fs::exists(resolvedPath))
            {
                shaderPath = resolvedPath;
            }
            else
            {
                // Try as-is (might already be absolute or in different location)
                shaderPath = data.shaderPath;
            }
        }

        if (m_shaderEngine->loadPreset(shaderPath))
        {
            // Apply shader parameters
            for (const auto &param : data.shaderParameters)
            {
                m_shaderEngine->setShaderParameter(param.first, param.second);
            }

            // Update UI with shader path (use the relative path from preset)
            // This is important because setCurrentShader triggers a callback that
            // will try to reload the shader, so we need to pass the correct relative path
            if (m_ui)
            {
                // Use the relative path from the preset data (not the absolute path)
                // The callback expects a path relative to shaders/shaders_glsl
                m_ui->setCurrentShader(data.shaderPath);
            }
        }
        else
        {
            LOG_ERROR("Failed to load shader for preset: " + shaderPath);
        }
    }
    else if (m_ui && data.shaderPath.empty())
    {
        // No shader in preset - clear shader in UI
        m_ui->setCurrentShader("");
    }

    // 2. Apply source type if changed
    if (m_ui && data.sourceType >= 0)
    {
        UIManager::SourceType sourceType = static_cast<UIManager::SourceType>(data.sourceType);
        if (m_ui->getSourceType() != sourceType)
        {
            m_ui->triggerSourceTypeChange(sourceType);
        }
    }

    // 3. Reconfigure capture
    // Note: devicePath is NOT used - it varies between systems
    if (data.captureWidth > 0 && data.captureHeight > 0 && m_capture)
    {
        // devicePath is NOT used - varies between systems

        // Check if we need to reconfigure
        bool needsReconfig = false;
        if (m_capture->isOpen())
        {
            // Check if resolution or FPS changed
            if (m_captureWidth != data.captureWidth ||
                m_captureHeight != data.captureHeight ||
                m_captureFps != data.captureFps)
            {
                needsReconfig = true;
            }
        }
        else if (data.sourceType != 0) // Not None - need to open device
        {
            needsReconfig = true;
        }

        if (needsReconfig)
        {
            if (m_capture->isOpen())
            {
                // Use reconfigureCapture which properly closes and reopens the device
                if (reconfigureCapture(data.captureWidth, data.captureHeight, data.captureFps))
                {
                    m_captureWidth = data.captureWidth;
                    m_captureHeight = data.captureHeight;
                    m_captureFps = data.captureFps;

                    // Delete and recreate texture with new dimensions
                    if (m_frameProcessor)
                    {
                        m_frameProcessor->deleteTexture();
                    }
                }
                else
                {
                    LOG_ERROR("Failed to reconfigure capture for preset");
                }
            }
            else if (data.sourceType != 0) // V4L2 or DirectShow - open device
            {
                // Open device if not already open
                if (m_capture->open(m_devicePath))
                {
                    if (m_capture->setFormat(data.captureWidth, data.captureHeight, 0))
                    {
                        m_capture->setFramerate(data.captureFps);
                        m_capture->startCapture();
                        m_captureWidth = data.captureWidth;
                        m_captureHeight = data.captureHeight;
                        m_captureFps = data.captureFps;

                        // Delete and recreate texture with new dimensions
                        if (m_frameProcessor)
                        {
                            m_frameProcessor->deleteTexture();
                        }
                    }
                    else
                    {
                        LOG_ERROR("Failed to set format for preset");
                        m_capture->close();
                    }
                }
                else
                {
                    LOG_ERROR("Failed to open device for preset: " + m_devicePath);
                }
            }
        }
        else if (data.sourceType == 0) // None - use dummy mode
        {
            // Activate dummy mode if source type is None
            if (!m_capture->isDummyMode())
            {
                m_capture->setDummyMode(true);
            }
            if (m_capture->setFormat(data.captureWidth, data.captureHeight, 0))
            {
                if (!m_capture->isOpen() || !m_capture->startCapture())
                {
                    m_capture->startCapture();
                }
                m_captureWidth = data.captureWidth;
                m_captureHeight = data.captureHeight;
                m_captureFps = data.captureFps;
            }
        }

        // Update internal state to match preset (even if reconfig had issues)
        // This keeps UI in sync with what the preset expects
        m_captureWidth = data.captureWidth;
        m_captureHeight = data.captureHeight;
        m_captureFps = data.captureFps;
    }

    // 4. Apply image settings
    // Note: fullscreen and monitorIndex are NOT applied - they vary per user/system
    m_brightness = data.imageBrightness;
    m_contrast = data.imageContrast;
    m_maintainAspect = data.maintainAspect;
    // fullscreen is NOT applied - varies per user preference
    // monitorIndex is NOT applied - varies per system

    // 5. Apply V4L2 controls
    if (m_capture && !data.v4l2Controls.empty())
    {
        for (const auto &control : data.v4l2Controls)
        {
            m_capture->setControl(control.first, control.second);
        }
    }

    // 6. Update UI with all applied values
    if (m_ui)
    {
        // Update capture info (resolution and FPS)
        if (data.captureWidth > 0 && data.captureHeight > 0)
        {
            std::string currentDevice = m_capture && m_capture->isOpen() ? m_devicePath : "";
            m_ui->setCaptureInfo(data.captureWidth, data.captureHeight, data.captureFps, currentDevice);
        }

        // Update image settings
        m_ui->setBrightness(m_brightness);
        m_ui->setContrast(m_contrast);
        m_ui->setMaintainAspect(m_maintainAspect);
        // fullscreen and monitorIndex are NOT updated - they vary per user/system

        // Save all configuration changes
        m_ui->saveConfig();
    }

    LOG_INFO("Preset applied successfully: " + presetName);
}

void Application::createPresetFromCurrentState(const std::string &name, const std::string &description, bool captureThumbnail)
{
    if (!m_initialized)
    {
        LOG_ERROR("Cannot create preset: Application not initialized");
        return;
    }

    PresetManager presetManager;

    // Capture thumbnail if requested (must be done before creating preset)
    std::string thumbnailPath;
    if (captureThumbnail)
    {
        ThumbnailGenerator thumbnailGenerator;
        std::string sanitizedName = PresetManager::sanitizeName(name);
        fs::path thumbPath = fs::path(presetManager.getThumbnailsDirectory()) / (sanitizedName + ".png");

        if (thumbnailGenerator.captureAndSaveThumbnail(thumbPath.string(), 320, 240))
        {
            // Store thumbnail path as relative to thumbnails directory
            fs::path thumbnailsDir = fs::path(presetManager.getThumbnailsDirectory());
            try
            {
                fs::path relativePath = fs::relative(thumbPath, thumbnailsDir);
                if (!relativePath.empty() && relativePath.string() != ".")
                {
                    thumbnailPath = relativePath.string();
                }
                else
                {
                    // If relative fails, use just the filename
                    thumbnailPath = (sanitizedName + ".png");
                }
            }
            catch (...)
            {
                // If relative conversion fails, use just the filename
                thumbnailPath = (sanitizedName + ".png");
            }

            LOG_INFO("Thumbnail captured for preset: " + name);
        }
        else
        {
            LOG_WARN("Failed to capture thumbnail for preset: " + name);
        }
    }

    PresetManager::PresetData data;
    data.name = name;
    data.description = description;
    data.thumbnailPath = thumbnailPath; // Set thumbnail path if captured

    // Collect shader information
    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        std::string shaderPath = m_shaderEngine->getPresetPath();

        // Convert shader path to relative path (relative to shaders/shaders_glsl)
        fs::path shaderBasePath = getShaderBasePath();
        fs::path shaderPathObj(shaderPath);

        if (shaderPathObj.is_absolute())
        {
            try
            {
                fs::path relativePath = fs::relative(shaderPathObj, shaderBasePath);
                if (!relativePath.empty() && relativePath.string() != ".")
                {
                    data.shaderPath = relativePath.string();
                }
                else
                {
                    // If relative fails, try to extract path after "shaders_glsl"
                    std::string shaderPathStr = shaderPath;
                    size_t pos = shaderPathStr.find("shaders_glsl");
                    if (pos != std::string::npos)
                    {
                        data.shaderPath = shaderPathStr.substr(pos + 12); // +12 for "shaders_glsl"
                        // Remove leading slash if present
                        if (!data.shaderPath.empty() && data.shaderPath[0] == '/')
                        {
                            data.shaderPath = data.shaderPath.substr(1);
                        }
                    }
                    else
                    {
                        data.shaderPath = shaderPath; // Fallback to original
                    }
                }
            }
            catch (...)
            {
                // If relative conversion fails, try to extract path after "shaders_glsl"
                std::string shaderPathStr = shaderPath;
                size_t pos = shaderPathStr.find("shaders_glsl");
                if (pos != std::string::npos)
                {
                    data.shaderPath = shaderPathStr.substr(pos + 12);
                    if (!data.shaderPath.empty() && data.shaderPath[0] == '/')
                    {
                        data.shaderPath = data.shaderPath.substr(1);
                    }
                }
                else
                {
                    data.shaderPath = shaderPath; // Fallback to original
                }
            }
        }
        else
        {
            data.shaderPath = shaderPath; // Already relative
        }

        // Get shader parameters
        auto params = m_shaderEngine->getShaderParameters();
        for (const auto &param : params)
        {
            data.shaderParameters[param.name] = param.value;
        }
    }

    // Collect capture configuration
    if (m_ui)
    {
        // Get source type from UI
        UIManager::SourceType sourceType = m_ui->getSourceType();
        data.sourceType = static_cast<int>(sourceType);
    }

    // Note: devicePath is NOT saved - it can vary between systems
    if (m_capture && m_capture->isOpen())
    {
        data.captureWidth = m_capture->getWidth();
        data.captureHeight = m_capture->getHeight();
        data.captureFps = m_captureFps;
        // devicePath is NOT saved - varies between systems
    }
    else if (m_capture && m_capture->isDummyMode())
    {
        // Even in dummy mode, save the configuration
        data.captureWidth = m_captureWidth;
        data.captureHeight = m_captureHeight;
        data.captureFps = m_captureFps;
        // devicePath is NOT saved - varies between systems
    }

    // Collect image settings
    // Note: fullscreen and monitorIndex are NOT saved - they can vary
    data.imageBrightness = m_brightness;
    data.imageContrast = m_contrast;
    data.maintainAspect = m_maintainAspect;
    // fullscreen is NOT saved - varies per user preference
    // monitorIndex is NOT saved - varies per system

    // Collect streaming settings (if active)
    if (m_streamManager && m_streamManager->isActive())
    {
        data.streamingWidth = m_streamingWidth;
        data.streamingHeight = m_streamingHeight;
        data.streamingFps = m_streamingFps;
        data.streamingBitrate = m_streamingBitrate;
        data.streamingAudioBitrate = m_streamingAudioBitrate;
        data.streamingVideoCodec = m_streamingVideoCodec;
        data.streamingAudioCodec = m_streamingAudioCodec;
        data.streamingH264Preset = m_streamingH264Preset;
        data.streamingH265Preset = m_streamingH265Preset;
        data.streamingH265Profile = m_streamingH265Profile;
        data.streamingH265Level = m_streamingH265Level;
        data.streamingVP8Speed = m_streamingVP8Speed;
        data.streamingVP9Speed = m_streamingVP9Speed;
        data.streamingHardwareEncoder = m_streamingHardwareEncoder;
    }

    // Collect V4L2 controls (if applicable)
    if (m_capture && m_capture->isOpen())
    {
        // Get common V4L2 controls
        std::vector<std::string> controlNames = {"Brightness", "Contrast", "Saturation", "Hue"};
        for (const auto &controlName : controlNames)
        {
            int32_t value = 0;
            if (m_capture->getControl(controlName, value))
            {
                data.v4l2Controls[controlName] = value;
            }
        }
    }

    // Save preset
    if (presetManager.savePreset(name, data))
    {
        LOG_INFO("Preset created from current state: " + name);
    }
    else
    {
        LOG_ERROR("Failed to create preset: " + name);
    }
}

fs::path Application::getShaderBasePath() const
{
    const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
    if (envShaderPath && fs::exists(envShaderPath))
    {
        return fs::path(envShaderPath);
    }
    return fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl";
}

std::string Application::resolveShaderPath(const std::string &shaderPath) const
{
    if (shaderPath.empty())
    {
        return "";
    }

    fs::path shaderBasePath = getShaderBasePath();
    fs::path fullPath = shaderBasePath / shaderPath;

    return fullPath.string();
}

void Application::applyPendingRemoteMeta()
{
    if (!m_hasPendingRemoteMeta.load()) return;

    // Snapshot the pending values out from under the polling thread.
    std::string preset;
    std::string presetHash;
    bool pipelineEnabled = true;
    std::vector<std::pair<std::string, float>> params;
    uint32_t sourceW = 0, sourceH = 0;
    bool seedImage = false;
    float imgBrightness = 1.0f, imgContrast = 1.0f;
    bool imgMaintainAspect = true;
    uint32_t imgOutW = 0, imgOutH = 0;
    {
        std::lock_guard<std::mutex> lock(m_pendingRemoteMutex);
        preset           = std::move(m_pendingRemotePreset);
        presetHash       = std::move(m_pendingRemotePresetHash);
        pipelineEnabled  = m_pendingRemotePipelineEnabled;
        params           = std::move(m_pendingRemoteParams);
        sourceW          = m_pendingRemoteSourceWidth;
        sourceH          = m_pendingRemoteSourceHeight;
        if (m_hasPendingRemoteImageSeed)
        {
            seedImage         = true;
            imgBrightness     = m_pendingRemoteImageBrightness;
            imgContrast       = m_pendingRemoteImageContrast;
            imgMaintainAspect = m_pendingRemoteImageMaintainAspect;
            imgOutW           = m_pendingRemoteImageOutputWidth;
            imgOutH           = m_pendingRemoteImageOutputHeight;
            m_hasPendingRemoteImageSeed = false;
        }
        m_hasPendingRemoteMeta.store(false);
    }

    // Seed the local Image tab from the host on the very first snapshot
    // per connection. m_remoteImageSeeded flips true here and gates the
    // callback so subsequent snapshots leave these values alone — the
    // user is free to tweak the local Image controls after the initial
    // sync.
    if (seedImage && m_ui)
    {
        m_ui->setBrightness(imgBrightness);
        m_ui->setContrast(imgContrast);
        m_ui->setMaintainAspect(imgMaintainAspect);
        m_ui->setOutputResolution(imgOutW, imgOutH);
        m_remoteImageSeeded = true;
        LOG_INFO("RemoteMetaSync: seeded local Image from host — brightness=" +
                 std::to_string(imgBrightness) + " contrast=" +
                 std::to_string(imgContrast) + " maintainAspect=" +
                 std::to_string(imgMaintainAspect) + " output=" +
                 std::to_string(imgOutW) + "x" + std::to_string(imgOutH));
    }

    // Tell the remote capture to rescale to the host's source dims (if the
    // stream is encoded at a different size) and sync our render-size view
    // so downstream FBO / viewport calculations use the right values.
    if (sourceW > 0 && sourceH > 0)
    {
        if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_capture.get()))
        {
            remote->setTargetResolution(sourceW, sourceH);
        }
        if (sourceW != m_captureWidth || sourceH != m_captureHeight)
        {
            LOG_INFO("Remote source dims from /meta: " +
                     std::to_string(sourceW) + "x" + std::to_string(sourceH) +
                     " (was " + std::to_string(m_captureWidth) + "x" + std::to_string(m_captureHeight) + ")");
            m_captureWidth  = sourceW;
            m_captureHeight = sourceH;
        }
    }

    if (!m_shaderEngine || !m_ui)
    {
        return;
    }

    // Phase 4 minimum: resolve preset by name in the local shader library.
    // If the preset isn't there, log and keep whatever shader the client
    // already has — Phase 4b will fetch the bundle from /meta/shader-bundle
    // and cache it locally for these cases.
    bool reloaded = false;
    if (!preset.empty() && presetHash != m_appliedRemotePresetHash)
    {
        const std::string fullPath = resolveShaderPath(preset);
        if (!fullPath.empty())
        {
            LOG_INFO("RemoteMetaSync: applying host preset '" + preset + "' (hash " + presetHash + ")");
            if (m_shaderEngine->loadPreset(fullPath))
            {
                m_ui->setCurrentShader(preset);
                m_appliedRemotePresetHash = presetHash;
                reloaded = true;
            }
            else
            {
                LOG_WARN("RemoteMetaSync: failed to load preset locally — bundle fetch is a Phase 4b TODO");
            }
        }
    }

    // Master pipeline toggle: mirror the host's "Apply shader pipeline".
    m_ui->setShaderPipelineEnabled(pipelineEnabled);

    // Parameter overrides apply on top of whatever preset is now active.
    // After a preset reload, the engine's parameters are at their preset
    // defaults; layering the host's overrides on top reproduces the look.
    if (!params.empty())
    {
        for (const auto &kv : params)
        {
            m_shaderEngine->setShaderParameter(kv.first, kv.second);
        }
        if (reloaded)
        {
            LOG_INFO("RemoteMetaSync: applied " + std::to_string(params.size()) +
                     " parameter override(s)");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// #49 Phase 2 — public directory publish lifecycle
//
// The UI owns the "publish on/off" toggle plus the user-visible
// metadata fields. This method, called once per main-loop iteration,
// reconciles those UI fields with the DirectoryClient instance that
// actually talks HTTP to the directory service:
//
//   - toggle OFF → ensure DirectoryClient is stopped
//   - toggle ON, client Idle → build a Config from UI state, start
//   - toggle ON, client Active → no-op (changes are applied via
//     PATCH on the next heartbeat tick if updateMetadata was called)
//
// Always cheap when there's no transition; just compares the toggle
// against the current state.
// ─────────────────────────────────────────────────────────────────────
void Application::syncDirectoryClient()
{
    if (!m_ui) return;

    // Mirror the remote capture's reconnect-backoff flag and the
    // "currently decoding frames" flag onto UIManager so the Info
    // panel and the connection overlay can read them without holding
    // the VideoCaptureRemote pointer themselves. In host mode
    // m_capture isn't a VideoCaptureRemote and the dynamic_cast
    // falls through to defaults (offline=false, receivingFrames=
    // whatever the base class reports — which is isOpen() for local
    // backends).
    {
        bool offline   = false;
        bool receiving = false;
        if (m_capture)
        {
            if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_capture.get()))
            {
                offline   = remote->isHostLikelyOffline();
                receiving = remote->isReceivingFrames();
            }
            else
            {
                receiving = m_capture->isReceivingFrames();
            }
        }
        m_ui->setRemoteHostLikelyOffline(offline);
        m_ui->setRemoteReceivingFrames(receiving);
    }

    // #49 Phase 3 — keep the server-side password gate in sync with
    // whatever the user typed in the publish UI. We hash on every
    // call (cheap, sha256 of < 1 KB), but the StreamManager setter
    // is a no-op when the value hasn't changed. Done outside the
    // wantPublish gate below so the gate stays active even on
    // streams the user doesn't publicly list (someone you handed the
    // URL directly is still protected).
    if (m_streamManager)
    {
        const std::string &raw = m_ui->getDirectoryPassword();
        m_streamManager->setStreamPasswordHash(
            raw.empty() ? std::string{} : PasswordHash::sha256Hex(raw));
    }

    // Publish only makes sense when there's actually a stream being
    // served. Two scenarios where this gate matters:
    //   1. User toggled publish on previously, the persisted config
    //      flag stays true, and now they launch a second instance on
    //      the same machine just to view (client mode). Without the
    //      gate, that client instance would happily try to register
    //      itself in the directory even though it has nothing to
    //      advertise — duplicate-looking entries from the same IP.
    //   2. User toggles publish on before pressing Start Streaming.
    //      The entry would advertise an endpoint that 503s for any
    //      client that tried to connect. Better to not list at all
    //      until /raw is serving.
    //
    // 'Streaming active' is the right signal here — it's set by
    // StreamManager once a streamer has bound its port and is
    // accepting clients. Toggle stays on (persisted) so as soon as
    // streaming actually starts, publish kicks in automatically.
    const bool wantPublish    = m_ui->getDirectoryPublishEnabled();
    const bool streamingLive  = m_ui->getStreamingActive();
    const bool shouldPublish  = wantPublish && streamingLive;
    const std::string mode    = m_ui->getDirectoryEndpointMode();

    // #49 Phase 2.5 — manage the cloudflared child process whenever
    // the user has tunnel-cloudflare selected. We tear it down on
    // any of: publish toggle off, streaming offline, mode switched
    // away. Lazy-construct on first need so plain direct/custom-mode
    // users never spawn an extra thread.
    const bool wantTunnel = shouldPublish && (mode == "tunnel-cloudflare");
    if (wantTunnel)
    {
        if (!m_cloudflaredManager)
        {
            m_cloudflaredManager = std::make_unique<CloudflaredManager>();
        }
        auto cfState = m_cloudflaredManager->getState();
        if (cfState == CloudflaredManager::State::Idle)
        {
            // Phase 2.5c (#60): pick Quick vs Named based on the user's
            // sub-mode. Quick keeps the old 'localPort only' shape;
            // Named also needs a configured tunnel id + hostname.
            const std::string tunnelMode = m_ui->getDirectoryTunnelMode();
            CloudflaredManager::TunnelConfig cfg;
            cfg.localPort = m_ui->getStreamingPort();
            if (tunnelMode == "named")
            {
                const std::string id   = m_ui->getDirectoryNamedTunnelId();
                const std::string host = m_ui->getDirectoryNamedTunnelHostname();
                if (id.empty() || host.empty())
                {
                    // Defer the start — the UI is responsible for
                    // collecting these fields. syncDirectoryClient
                    // shows the waiting message a few lines below.
                    m_ui->setDirectoryStatusText("Named tunnel needs id + hostname");
                }
                else
                {
                    cfg.mode      = CloudflaredManager::Mode::Named;
                    cfg.tunnelId  = id;
                    cfg.hostname  = host;
                    cfg.publicUrl = "https://" + host;
                    m_cloudflaredManager->start(cfg);
                }
            }
            else
            {
                cfg.mode = CloudflaredManager::Mode::Quick;
                m_cloudflaredManager->start(cfg);
            }
        }
    }
    else if (m_cloudflaredManager &&
             m_cloudflaredManager->getState() != CloudflaredManager::State::Idle)
    {
        m_cloudflaredManager->stop();
    }

    if (!shouldPublish && !m_directoryClient) return;
    if (!m_directoryClient)
    {
        m_directoryClient = std::make_unique<DirectoryClient>();
    }

    auto state = m_directoryClient->getState();

    // #69 — If the user edited the Directory URL while we're already
    // registered/registering against the old host, drop the session so
    // the Idle branch below re-registers against the new URL on this
    // very tick. Without this, URL edits only take effect on app
    // restart, which silently surprises users.
    {
        const std::string currentUrl = m_ui->getDirectoryUrl();
        if (state != DirectoryClient::State::Idle &&
            !m_publishedDirectoryUrl.empty() &&
            m_publishedDirectoryUrl != currentUrl)
        {
            m_directoryClient->stop();
            state = DirectoryClient::State::Idle;
        }
        m_publishedDirectoryUrl = currentUrl;
    }

    if (!shouldPublish)
    {
        if (state != DirectoryClient::State::Idle)
        {
            m_directoryClient->stop();
            // Surface why we stopped so the user understands the
            // toggle is still on but nothing is listed right now.
            if (wantPublish && !streamingLive)
            {
                m_ui->setDirectoryStatusText("Waiting for streaming to start");
            }
            else
            {
                m_ui->setDirectoryStatusText("Idle");
            }
        }
        else if (wantPublish && !streamingLive &&
                 m_ui->getDirectoryStatusText() != "Waiting for streaming to start")
        {
            m_ui->setDirectoryStatusText("Waiting for streaming to start");
        }
        return;
    }

    // wantPublish == true beyond this point.

    // For tunnel mode, gate the DirectoryClient on cloudflared having
    // actually handed us a URL. Anything earlier would advertise a
    // localhost endpoint that nobody outside this machine can reach.
    std::string tunnelUrl;
    if (mode == "tunnel-cloudflare")
    {
        if (!m_cloudflaredManager)
        {
            m_ui->setDirectoryStatusText("Tunnel unavailable");
            return;
        }
        auto cfState = m_cloudflaredManager->getState();
        switch (cfState)
        {
            case CloudflaredManager::State::Active:
                tunnelUrl = m_cloudflaredManager->getUrl();
                break;
            case CloudflaredManager::State::Spawning:
                if (state != DirectoryClient::State::Idle) m_directoryClient->stop();
                m_ui->setDirectoryStatusText("Waiting for cloudflared tunnel…");
                return;
            case CloudflaredManager::State::NotFound:
                if (state != DirectoryClient::State::Idle) m_directoryClient->stop();
                {
                    // NotFound covers both "binary missing" and any
                    // pre-flight rejection (missing credentials,
                    // invalid config, etc.). Surface the manager's
                    // own error text when it has one — it knows what
                    // actually failed. Fall back to the generic
                    // install hint only when there's nothing to say.
                    std::string err = m_cloudflaredManager->getLastError();
                    if (err.empty())
                    {
                        err = "cloudflared not installed — install from "
                              "cloudflare.com/products/tunnel";
                    }
                    m_ui->setDirectoryStatusText("Error: " + err);
                }
                return;
            case CloudflaredManager::State::Crashed:
                if (state != DirectoryClient::State::Idle) m_directoryClient->stop();
                m_ui->setDirectoryStatusText("Error: tunnel crashed — " + m_cloudflaredManager->getLastError());
                return;
            default:
                m_ui->setDirectoryStatusText("Waiting for cloudflared tunnel…");
                return;
        }
    }

    if (state == DirectoryClient::State::Idle)
    {
        // Build Config from current UI + streaming state.
        DirectoryClient::Config cfg;
        cfg.directoryUrl     = m_ui->getDirectoryUrl();
        cfg.name             = m_ui->getDirectoryStreamName();
        cfg.hostNickname     = m_ui->getDirectoryHostNickname();
        cfg.shader           = m_ui->getCurrentShader();
        cfg.resolutionW      = m_ui->getCaptureWidth();
        cfg.resolutionH      = m_ui->getCaptureHeight();
        cfg.fps              = m_ui->getCaptureFps();
        cfg.codec            = m_ui->getStreamingVideoCodec() == "h265" ? "h265" : "h264";
        cfg.passwordRequired = !m_ui->getDirectoryPassword().empty();
        cfg.endpointMode     = mode;
        cfg.version          = RETROCAPTURE_VERSION;
        cfg.insecureSkipVerify = m_ui->getDirectoryInsecureSkipVerify();

        if (cfg.endpointMode == "tunnel-cloudflare")
        {
            cfg.endpoint = tunnelUrl;
        }
        else if (cfg.endpointMode == "custom")
        {
            cfg.endpoint = m_ui->getDirectoryCustomEndpoint();
        }
        else
        {
            // Direct mode: the directory's `endpoint` field is a
            // BASE URL (http://host:port). The client appends /raw
            // and /meta by convention; if we include them here they
            // get appended again ('/raw/raw' → 404). The local stream
            // URL ends in '/stream' which is wrong here for the same
            // reason — never reuse it for the directory endpoint.
            //
            // Phase 2.5 (Cloudflare Tunnel) will replace this with
            // the tunnel base URL.
            cfg.endpoint = "http://localhost:" + std::to_string(m_ui->getStreamingPort());
        }

        if (m_directoryClient->start(cfg))
        {
            m_ui->setDirectoryStatusText("Registering…");
        }
        else
        {
            m_ui->setDirectoryStatusText("Error: " + m_directoryClient->getLastError());
            // Roll the toggle back so the UI shows the actual state.
            m_ui->setDirectoryPublishEnabled(false);
        }
        return;
    }

    // State display — translate enum to one-liner.
    std::string status;
    switch (state)
    {
        case DirectoryClient::State::Registering: status = "Registering…"; break;
        case DirectoryClient::State::Active:
        {
            const auto id = m_directoryClient->getStreamId();
            status = id.empty() ? "Active" : "Active — " + id;
            break;
        }
        case DirectoryClient::State::Error:
        {
            const auto err = m_directoryClient->getLastError();
            status = err.empty() ? "Error" : "Error: " + err;
            break;
        }
        case DirectoryClient::State::Stopping: status = "Stopping…"; break;
        case DirectoryClient::State::Idle:      status = "Idle"; break;
    }
    if (m_ui->getDirectoryStatusText() != status)
    {
        m_ui->setDirectoryStatusText(status);
    }

    // Mirror telemetry counters into UIManager every frame so the
    // publish section can render them without holding a pointer to
    // DirectoryClient.
    const auto stats = m_directoryClient->getStats();
    m_ui->setDirectoryStats(stats.registerOk, stats.registerFail,
                            stats.heartbeatOk, stats.heartbeatFail,
                            stats.patchOk, stats.patchFail,
                            stats.secondsSinceLastHeartbeat);
}

// Recording methods
void Application::setRecordingSettings(const RecordingSettings &settings)
{
    if (m_recordingManager)
    {
        m_recordingManager->setRecordingSettings(settings);
    }
}

RecordingSettings Application::getRecordingSettings() const
{
    if (m_recordingManager)
    {
        return m_recordingManager->getRecordingSettings();
    }
    return RecordingSettings();
}

bool Application::startRecording()
{
    if (m_recordingManager)
    {
        populateRecordingContext();
        RecordingSettings settings = m_recordingManager->getRecordingSettings();
        return m_recordingManager->startRecording(settings);
    }
    return false;
}

void Application::populateRecordingContext()
{
    if (!m_recordingManager) return;

    RecordingManager::Context ctx;
    if (m_ui)
    {
        const std::string shader = m_ui->getCurrentShader();
        ctx.shaderName  = shader.empty() ? "(none)" : shader;
        ctx.hostNickname = m_ui->getDirectoryHostNickname();
        ctx.sourceWidth  = m_ui->getCaptureWidth();
        ctx.sourceHeight = m_ui->getCaptureHeight();
        ctx.sourceFps    = m_ui->getCaptureFps();
        switch (m_ui->getSourceType())
        {
            case UIManager::SourceType::V4L2:   ctx.sourceType = "v4l2";       break;
            case UIManager::SourceType::DS:     ctx.sourceType = "directshow"; break;
            case UIManager::SourceType::Remote: ctx.sourceType = "remote";     break;
            default:                            ctx.sourceType = "none";       break;
        }
    }
#ifdef RETROCAPTURE_VERSION
    ctx.applicationVersion = RETROCAPTURE_VERSION;
#endif
    // Single-output today — when dual recording lands in #59 phase C
    // we'll override 'kind' from the raw-side path.
    ctx.kind = "single";
    m_recordingManager->setRecordingContext(ctx);
}

void Application::stopRecording()
{
    if (m_recordingManager)
    {
        m_recordingManager->stopRecording();
    }
}

bool Application::isRecording() const
{
    if (m_recordingManager)
    {
        return m_recordingManager->isRecording();
    }
    return false;
}

uint64_t Application::getRecordingDurationUs()
{
    if (m_recordingManager)
    {
        return m_recordingManager->getCurrentDurationUs();
    }
    return 0;
}

uint64_t Application::getRecordingFileSize()
{
    if (m_recordingManager)
    {
        return m_recordingManager->getCurrentFileSize();
    }
    return 0;
}

std::string Application::getRecordingFilename()
{
    if (m_recordingManager)
    {
        return m_recordingManager->getCurrentFilename();
    }
    return "";
}

std::vector<RecordingMetadata> Application::listRecordings()
{
    if (m_recordingManager)
    {
        return m_recordingManager->listRecordings();
    }
    return std::vector<RecordingMetadata>();
}

bool Application::deleteRecording(const std::string &recordingId)
{
    if (m_recordingManager)
    {
        return m_recordingManager->deleteRecording(recordingId);
    }
    return false;
}

bool Application::renameRecording(const std::string &recordingId, const std::string &newName)
{
    if (m_recordingManager)
    {
        return m_recordingManager->renameRecording(recordingId, newName);
    }
    return false;
}

std::string Application::getRecordingPath(const std::string &recordingId)
{
    if (m_recordingManager)
    {
        return m_recordingManager->getRecordingPath(recordingId);
    }
    return "";
}
