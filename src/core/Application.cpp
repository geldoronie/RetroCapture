#include "Application.h"
#include "UICallbackWiring.h"
#include "RemoteSourceManager.h"
#include "FrameCapturePipeline.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureFactory.h"
#include "../capture/VideoCaptureRemote.h"
#include "../capture/VideoCaptureScreen.h"
#include "../capture/VideoCaptureTestPattern.h"
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
#include "../osd/OSDChat.h"
#include "../chat/ChatClient.h"
#include "../identity/ChatIdentity.h"
#include "../identity/OwnedRooms.h"
#if defined(__linux__)
#  include "../output/VirtualCameraOutput.h"
#elif defined(_WIN32)
#  include "../output/VirtualCameraOutputWin.h"
#elif defined(__APPLE__)
#  include "../output/VirtualCameraOutputMac.h"
#endif
#include "../tray/ISystemTray.h"
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
    m_pipeline = std::make_unique<FrameCapturePipeline>(*this);
    m_remoteManager = std::make_unique<RemoteSourceManager>(*this);
    m_callbackWiring = std::make_unique<UICallbackWiring>(*this);
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

    // Chat transport (#84). Created up front so the OSD panel has a
    // live pointer even before publish/connect; the client self-idles
    // until connect(streamId) is called by the publish or remote-meta
    // path.
    //
    // URL precedence:
    //   1. --chat-url (non-empty m_chatBaseUrl) wins on launch and is
    //      written into UIManager so the Streaming → Advanced field
    //      reflects the live value. The user can then edit it from
    //      there.
    //   2. Otherwise UIManager's value (loaded from config.json, or
    //      the built-in default ws://localhost:8082) is the source
    //      of truth.
    // After init, the UI is the canonical value — syncDirectoryClient
    // reads it every frame and pushes any change down to ChatClient.
    m_chatClient = std::make_unique<ChatClient>();
    if (m_ui)
    {
        if (!m_chatBaseUrl.empty())
        {
            m_ui->setChatBaseUrl(m_chatBaseUrl);
        }
        m_chatClient->setBaseUrl(m_ui->getChatBaseUrl());
        m_ui->setChatClient(m_chatClient.get());
    }
    else
    {
        m_chatClient->setBaseUrl(m_chatBaseUrl);
    }

    // Persistent chat identity (#84). The OSD Profile window will
    // populate fields and re-save on first launch; we just load
    // whatever's on disk here so an existing user's rc_<id> goes
    // out with every hello from the get-go.
    {
        const ChatIdentity ident = identity::load();
        if (ident.isInitialized())
        {
            m_chatClient->setClientId(ident.id);
            // Push the saved nickname into ChatClient AND UIManager —
            // the OSD's Rooms-browse click + manual join paths read
            // snap.nickname; without this seeding, connectBySlug
            // bails on the empty-nickname guard and silently does
            // nothing on first launch.
            if (!ident.nickname.empty())
            {
                m_chatClient->setNickname(ident.nickname);
                if (m_ui) m_ui->setChatNickname(ident.nickname);
            }
        }
    }

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
        const bool quickInteractive =
            m_ui->getQuickActionsOverlay() &&
            m_ui->getQuickActionsOverlay()->isVisible();
        const bool chatInteractive =
            m_ui->getChatOverlay() &&
            m_ui->getChatOverlay()->isVisible();
        m_window->setCursorVisible(
            m_ui->isVisible() || quickInteractive || chatInteractive);
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
        remote->setAudioVolume(m_remoteAudioGain);
        m_capture = std::move(remote);
    }
    else if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Screen)
    {
        // #107 — screen capture is its own backend (not in the
        // platform factory, like Remote). Target + region come from the
        // UI/config; open() happens below via the shared device path.
        LOG_INFO("Source is screen — creating VideoCaptureScreen");
        auto screen = std::make_unique<VideoCaptureScreen>();
        screen->setRegion(m_ui->getScreenRegionX(), m_ui->getScreenRegionY(),
                          m_ui->getScreenRegionW(), m_ui->getScreenRegionH());
        m_capture = std::move(screen);
    }
    else if (m_ui && m_ui->getSourceType() == UIManager::SourceType::Test)
    {
        // #149 — synthetic test-pattern source for the smoke-test. Isolated
        // from the platform factory so it can't regress real capture.
        LOG_INFO("Source is test — creating VideoCaptureTestPattern");
        m_capture = std::make_unique<VideoCaptureTestPattern>();
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
#if defined(_WIN32) || defined(__APPLE__)
        // Windows (DirectShow) and macOS (AVFoundation) both pick a
        // capture device by enumeration at the backend layer — there
        // is no filesystem path. Without a device explicitly chosen,
        // start in dummy mode and let the user pick a device via the
        // Source tab.
        LOG_INFO("No device specified - activating dummy mode directly");
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
        m_remoteManager->startWorker(m_devicePath, nullptr);
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
    m_callbackWiring->wireAll();
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

        // F8 toggles the chat overlay (#84). Suppressed while the
        // chat input box has focus — otherwise pressing F8 to send a
        // funny line just toggles the panel.
        static bool f8Pressed = false;
        bool f8Now = sdlWindow->isKeyPressed(SDLK_F8);
        if (f8Now && !f8Pressed)
        {
            auto *chat = m_ui->getChatOverlay();
            const bool inputFocused = chat && chat->isInputFocused();
            if (chat && !inputFocused) chat->toggleVisibility();
            f8Pressed = true;
        }
        else if (!f8Now)
        {
            f8Pressed = false;
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

    // F8 toggles the chat overlay (#84). Suppressed while the chat
    // input box has focus so typing "F8" in a message doesn't toggle
    // the panel mid-sentence.
    static bool f8Pressed = false;
    if (glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS)
    {
        if (!f8Pressed)
        {
            auto *chat = m_ui->getChatOverlay();
            const bool inputFocused = chat && chat->isInputFocused();
            if (chat && !inputFocused) chat->toggleVisibility();
            f8Pressed = true;
        }
    }
    else
    {
        f8Pressed = false;
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

    // Host-side monitor playback: on Linux it lives inside
    // AudioCapturePulse (MonitorPlayback), on macOS inside
    // AudioCaptureCoreAudio (also called MonitorPlayback there). The
    // capture class spins up its own monitor in open() — no Application-
    // level wiring needed.

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

// ─────────────────────────────────────────────────────────────────────
// #86 — system tray + hide-to-tray
// ─────────────────────────────────────────────────────────────────────
namespace {
// Open a URL in the user's default browser. Mirrors the pattern used
// by UIRecordings / UICredits.
void openUrlInBrowser(const std::string &url)
{
#if defined(PLATFORM_LINUX)
    std::string cmd = "xdg-open \"" + url + "\" &";
    if (std::system(cmd.c_str()) != 0) { /* best-effort */ }
#elif defined(PLATFORM_MACOS)
    std::string cmd = "open \"" + url + "\" &";
    if (std::system(cmd.c_str()) != 0) { /* best-effort */ }
#elif defined(_WIN32)
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
#endif
}
} // namespace

void Application::setupSystemTray()
{
    using retrocapture::ISystemTray;
    using retrocapture::TrayMenuItem;

    m_tray = retrocapture::createSystemTray();

    const bool wantTray = m_ui && m_ui->getTrayEnabled();

    // Window close-button policy. Installed regardless of tray host:
    //  - tray on + minimize-on-close + a real tray host → hide.
    //  - otherwise → real quit (so a user with no tray host isn't
    //    trapped with a close button that does nothing).
    m_window->setCloseCallback([this]()
    {
        const bool minimize = m_ui && m_ui->getTrayEnabled() &&
                              m_ui->getTrayMinimizeOnClose() && m_trayActive;
        if (minimize)
        {
            m_window->hide();
            refreshTrayMenu();
        }
        else
        {
            m_window->requestClose();
        }
    });

    if (!wantTray)
    {
        LOG_INFO("System tray disabled in preferences");
        return;
    }

    // The Linux backend loads our branded logo.png and serves it as
    // the SNI IconPixmap (the actual visual). The name passed here is
    // only the themed-icon fallback used if that asset can't be found
    // at runtime — "camera-video" exists in every icon theme.
    if (!m_tray->start("camera-video", "RetroCapture"))
    {
        LOG_WARN("System tray: no tray host available — the close "
                 "button will quit the application. Install an "
                 "AppIndicator/StatusNotifier host to enable "
                 "minimize-to-tray.");
        m_trayActive = false;
        return;
    }
    m_trayActive = true;

    // Left-click activation: toggle window visibility.
    m_tray->setOnActivate([this]()
    {
        if (m_window->isVisible()) m_window->hide();
        else                       m_window->show();
        refreshTrayMenu();
    });

    refreshTrayMenu();

    // Start-minimized: launch straight to the tray.
    if (m_ui->getTrayStartMinimized())
    {
        m_window->hide();
    }

    LOG_INFO("System tray active");
}

void Application::refreshTrayMenu()
{
    using retrocapture::TrayMenuItem;
    if (!m_tray || !m_trayActive) return;

    const bool streaming   = m_ui && m_ui->getStreamingActive();
    const bool recording   = m_ui && m_ui->getRecordingActive();

    // Tray notifications (#86) — fire on streaming/recording edges,
    // gated by the user preference. Runs before the menu-signature
    // early-return below so a visibility-only change doesn't suppress
    // it. The first pass just seeds the previous-state baseline.
    if (m_notifyInit && m_ui && m_ui->getTrayNotifications())
    {
        if (streaming != m_notifyPrevStreaming)
        {
            m_tray->notify("RetroCapture",
                           streaming ? "Streaming started" : "Streaming stopped");
        }
        if (recording != m_notifyPrevRecording)
        {
            if (recording)
            {
                m_tray->notify("RetroCapture", "Recording started");
            }
            else
            {
                const std::string fn = m_ui->getRecordingFilename();
                m_tray->notify("RetroCapture",
                               fn.empty() ? "Recording saved"
                                          : ("Recording saved: " + fn));
            }
        }
    }
    m_notifyPrevStreaming = streaming;
    m_notifyPrevRecording = recording;
    m_notifyInit = true;

    const bool hasSource    = m_ui && !m_ui->getCurrentDevice().empty();
    const bool clientMode  = m_ui && m_ui->isRemoteSource() && hasSource;
    const bool windowShown = m_window && m_window->isVisible();

    // Only re-push when something the menu reflects actually changed —
    // updateMenu() emits a D-Bus signal on Linux, not worth doing every
    // frame.
    const uint32_t sig =
        (streaming ? 1u : 0u) | (recording ? 2u : 0u) |
        (hasSource ? 4u : 0u) | (clientMode ? 8u : 0u) |
        (windowShown ? 16u : 0u);
    if (m_trayMenuSig == sig && m_trayMenuBuilt) return;
    m_trayMenuSig   = sig;
    m_trayMenuBuilt = true;

    std::vector<TrayMenuItem> items;

    // Streaming toggle — hidden in client mode (nothing local to
    // broadcast), disabled until a source is configured.
    if (!clientMode)
    {
        TrayMenuItem stream;
        stream.id      = "streaming";
        stream.label   = streaming ? "Stop Streaming" : "Start Streaming";
        stream.enabled = hasSource;
        stream.onClick = [this]()
        {
            if (m_ui) m_ui->triggerStreamingStartStop(!m_ui->getStreamingActive());
        };
        items.push_back(std::move(stream));
    }

    // Recording toggle — available in client mode too (records whatever
    // is in the framebuffer).
    {
        TrayMenuItem rec;
        rec.id      = "recording";
        rec.label   = recording ? "Stop Recording" : "Start Recording";
        rec.enabled = true;
        rec.onClick = [this]()
        {
            if (m_ui) m_ui->triggerRecordingStartStop(!m_ui->getRecordingActive());
        };
        items.push_back(std::move(rec));
    }

    // Open Web Portal.
    {
        TrayMenuItem portal;
        portal.id      = "webportal";
        portal.label   = "Open Web Portal";
        portal.enabled = m_ui && m_ui->getWebPortalEnabled();
        portal.onClick = [this]()
        {
            const uint16_t port = m_ui ? m_ui->getStreamingPort() : 8080;
            openUrlInBrowser("http://localhost:" + std::to_string(port) + "/");
        };
        items.push_back(std::move(portal));
    }

    { TrayMenuItem sep; sep.type = TrayMenuItem::Type::Separator; sep.id = "sep1"; items.push_back(std::move(sep)); }

    // Show / Hide window.
    {
        TrayMenuItem vis;
        vis.id      = "visibility";
        vis.label   = windowShown ? "Hide Window" : "Show Window";
        vis.onClick = [this]()
        {
            if (m_window->isVisible()) m_window->hide();
            else                       m_window->show();
            refreshTrayMenu();
        };
        items.push_back(std::move(vis));
    }

    { TrayMenuItem sep; sep.type = TrayMenuItem::Type::Separator; sep.id = "sep2"; items.push_back(std::move(sep)); }

    // Quit — set shouldClose so the main loop exits and shutdown()
    // runs the orderly teardown (finalize recording, leave directory,
    // join chat, release virtcam).
    {
        TrayMenuItem quit;
        quit.id      = "quit";
        quit.label   = "Quit";
        quit.onClick = [this]() { m_window->requestClose(); };
        items.push_back(std::move(quit));
    }

    m_tray->setMenu(items);
}

void Application::processAudioCapture()
{
    // #152 - extracted verbatim from run(): drain the audio capture into
    // the stream/recording managers (and keep the PulseAudio mainloop
    // serviced even when idle so system audio doesn't freeze).
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
            // Read and discard samples to keep the capture buffer
            // from backing up. The host-side monitor (Linux:
            // MonitorPlayback inside AudioCapturePulse; macOS:
            // monitor inside AudioCaptureCoreAudio) has its own
            // tap on the bus, so the user still hears the input.
            const size_t maxSamples = 4096;
            std::vector<int16_t> tempBuffer(maxSamples);
            m_audioCapture->getSamples(tempBuffer.data(), maxSamples);
        }
    }
}

void Application::run()
{
    if (!m_initialized)
    {
        LOG_ERROR("Application not initialized");
        return;
    }

    LOG_INFO("Starting main loop...");

    // #86 — bring up the system tray + hide-to-tray wiring now that
    // the window, UI and all pipelines are initialised.
    setupSystemTray();

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
        m_remoteManager->applyPendingRemoteMeta();

        // Phase 2 of #49: reconcile the public-directory publish state
        // with whatever the UI toggle currently says. Cheap when no
        // transition is needed (just compares two booleans and a few
        // strings).
        syncDirectoryClient();
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        syncVirtualCamera();
#endif
        // Cursor visibility tracks BOTH UIManager::isVisible() and the
        // OSD overlay (#68). The setOnVisibilityChanged callback only
        // fires on F12 toggles — toggling the quick-actions widget via
        // View doesn't, and the desktop client mode flip doesn't either.
        // Running this every frame keeps cursor state in sync; the
        // window manager's own internal cache makes redundant calls
        // free.
        updateCursorVisibility();

        m_window->pollEvents();

        // #86 — drain tray events (menu clicks / activation) on this
        // thread, then refresh the menu labels/enabled if state moved.
        if (m_tray)
        {
            m_tray->pump();
            refreshTrayMenu();
        }

        // Check again after polling events (window may have been invalidated)
        if (m_window->shouldClose())
        {
            break;
        }

        // Hide-to-tray pacing (#86): while the window is hidden,
        // swapBuffers() is a no-op, so there's no vsync/compositor
        // pacing to lean on and the loop would spin at 100% CPU.
        // Sleep a frame's worth here so the capture/shader/streaming/
        // recording/virtcam pipelines keep running at a sane rate
        // while we're backgrounded. ~60 Hz upper bound; lower-FPS
        // capture just sees more no-new-frame polls, which is cheap.
        if (m_window && !m_window->isVisible())
        {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
            usleep(16000);
#else
            Sleep(16);
#endif
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
        processAudioCapture();

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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
            // true → this frame was already presented (invalid-dims early-out),
            // skip the rest of the iteration exactly as the old inline continue did.
            if (m_pipeline->renderAndDistributeFrame())
                continue;
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
                        usleep(static_cast<useconds_t>(sleepUs));
#else
                        Sleep(static_cast<DWORD>(sleepUs / 1000));
#endif
                    }
                    m_lastFrameSwapUs = nowUs + std::max<int64_t>(0, targetIntervalUs - elapsedUs);
                }
                else
                {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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

    // #86 — tear down the tray icon early so it disappears the moment
    // the user picks Quit, before the (slower) pipeline teardown runs.
    if (m_tray)
    {
        m_tray->stop();
        m_tray.reset();
    }

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
#if defined(__linux__)
// #85 — Virtual camera (v4l2loopback) lifecycle. Mirrors the
// directory-client sync's "reconcile UI toggle with sink state"
// pattern. Cheap when nothing changes; on a toggle / device /
// dims transition does a stop + start. Status text is written
// back into UIManager for the configuration window to display.
void Application::syncVirtualCamera()
{
    if (!m_ui) return;

    // Synchronous "stop now" handshake (UI module-remove flow).
    // When the UI fires the request, force the sink down THIS
    // tick and post a notice the UI's worker spins on before
    // running pkexec rmmod. Without this, rmmod fails with
    // EBUSY because RetroCapture is still holding /dev/videoN
    // through m_virtcam's fd.
    if (m_ui->consumeVirtcamStopRequest())
    {
        if (m_virtcam && m_virtcam->isRunning())
        {
            m_virtcam->stop();
            m_ui->setVirtcamStatusText("");
            m_ui->setVirtcamErrorText("");
        }
        m_ui->setVirtcamStopped(true);
    }

    const bool        enabled = m_ui->getVirtcamEnabled();
    const std::string &cfgDev = m_ui->getVirtcamDevicePath();
    const std::string &fmtStr = m_ui->getVirtcamPixelFormat();
    const VirtualCameraOutput::PixelFormat fmt =
        (fmtStr == "rgb24") ? VirtualCameraOutput::PixelFormat::RGB24
                            : VirtualCameraOutput::PixelFormat::YUYV;

    // Resolve "0 = follow upstream" sentinels. Cascade:
    //   user-configured override (virtcam.outputWidth)
    //     → shader/image output (outputWidth, only set when the
    //       user picked an output resolution under Image)
    //     → raw capture dims (captureWidth, always populated as
    //       soon as a source is open).
    // The shader-output fallback alone wasn't enough — many
    // users don't configure an output resolution, so getOutputWidth
    // sits at 0 forever and we silently never started. Capture
    // dims is the right last-resort: it tracks the actual frame
    // size we're feeding through the pipeline.
    uint32_t w = m_ui->getVirtcamOutputWidth();
    uint32_t h = m_ui->getVirtcamOutputHeight();
    if (w == 0) w = m_ui->getOutputWidth();
    if (h == 0) h = m_ui->getOutputHeight();
    if (w == 0) w = m_ui->getCaptureWidth();
    if (h == 0) h = m_ui->getCaptureHeight();
    uint32_t f = m_ui->getVirtcamOutputFps();
    if (f == 0) f = m_ui->getCaptureFps();
    if (f == 0) f = 30; // cosmetic only; loopback doesn't enforce pacing

    if (!enabled)
    {
        if (m_virtcam && m_virtcam->isRunning())
        {
            m_virtcam->stop();
            m_ui->setVirtcamStatusText("");
            m_ui->setVirtcamErrorText("");
        }
        return;
    }

    if (w == 0 || h == 0)
    {
        // Truly nothing to push yet — no source open and no
        // override configured. Surface the wait so the user
        // doesn't think the click did nothing.
        m_ui->setVirtcamStatusText(
            "Waiting for a capture source (open a Source first).");
        return;
    }

    // Resolve device path: empty config = auto-pick first loopback.
    std::string devicePath = cfgDev;
    if (devicePath.empty())
    {
        const auto devices = VirtualCameraOutput::enumerateDevices();
        if (devices.empty())
        {
            m_ui->setVirtcamErrorText(
                "No v4l2loopback device. Run "
                "`sudo modprobe v4l2loopback exclusive_caps=1`.");
            return;
        }
        devicePath = devices.front().path;
    }

    // Lazy construct the sink.
    if (!m_virtcam) m_virtcam = std::make_unique<VirtualCameraOutput>();

    std::string err;
    if (!m_virtcam->start(devicePath, w, h, f, fmt, err))
    {
        m_ui->setVirtcamErrorText(err);
        return;
    }
    m_ui->setVirtcamErrorText("");
    // Status line for the configuration window.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Publishing %ux%u %s to %s",
                  m_virtcam->outputWidth(),
                  m_virtcam->outputHeight(),
                  (fmt == VirtualCameraOutput::PixelFormat::YUYV ? "YUYV" : "RGB24"),
                  devicePath.c_str());
    m_ui->setVirtcamStatusText(buf);
}
#endif // __linux__

#if defined(_WIN32)
// #85 Phase 2 — Windows virtual camera lifecycle. Simpler than the
// Linux side: there's exactly one virtual device (the
// RetroCaptureVCam.dll filter), no device picker. Driver state is
// determined by whether the DLL is registered. The sink writes
// frames to shared memory; the DirectShow filter inside every
// consumer process reads them out.
void Application::syncVirtualCamera()
{
    if (!m_ui) return;

    const bool enabled = m_ui->getVirtcamEnabled();

    if (!enabled)
    {
        if (m_virtcam && m_virtcam->isRunning())
        {
            m_virtcam->stop();
            m_ui->setVirtcamStatusText("");
            m_ui->setVirtcamErrorText("");
        }
        return;
    }

    if (!VirtualCameraOutputWin::isFilterDllRegistered())
    {
        m_ui->setVirtcamErrorText(
            "RetroCaptureVCam.dll is not registered. Run the "
            "installer (or `regsvr32 RetroCaptureVCam.dll` as admin) "
            "to expose the virtual camera to consumers.");
        return;
    }

    // Resolve dims using the same cascade as Linux: UI override →
    // shader/image output → capture dims. Filter side advertises a
    // fixed set (640x480, 1280x720, 1920x1080) so any sink dims
    // outside that will fall back to the frozen frame in the
    // filter. UI surfaces this caveat next to the resolution field.
    uint32_t w = m_ui->getVirtcamOutputWidth();
    uint32_t h = m_ui->getVirtcamOutputHeight();
    if (w == 0) w = m_ui->getOutputWidth();
    if (h == 0) h = m_ui->getOutputHeight();
    if (w == 0) w = m_ui->getCaptureWidth();
    if (h == 0) h = m_ui->getCaptureHeight();

    if (w == 0 || h == 0)
    {
        m_ui->setVirtcamStatusText(
            "Waiting for a capture source (open a Source first).");
        return;
    }

    // Filter currently only advertises RGB24; offer it as the
    // wire format regardless of what the UI cached. Once the
    // filter learns RGBA / YUYV we can route the UI's choice
    // through here.
    constexpr auto fmt = VirtualCameraOutputWin::PixelFormat::RGB24;

    if (!m_virtcam) m_virtcam = std::make_unique<VirtualCameraOutputWin>();

    if (!m_virtcam->isRunning())
    {
        std::string err;
        if (!m_virtcam->start(w, h, fmt, err))
        {
            m_ui->setVirtcamErrorText(err);
            return;
        }
    }
    m_ui->setVirtcamErrorText("");

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Publishing %ux%u RGB24 to RetroCaptureVCam.dll",
                  m_virtcam->outputWidth(),
                  m_virtcam->outputHeight());
    m_ui->setVirtcamStatusText(buf);
}
#endif // _WIN32

#if defined(__APPLE__)
// #85 macOS — virtual camera lifecycle. Mirrors the Windows path:
// single device (the CoreMediaIO DAL plug-in at
// /Library/CoreMediaIO/Plug-Ins/DAL/RetroCaptureVCam.plugin), no
// device picker. The host writes frames into POSIX shm; the
// plug-in inside every consumer process reads them.
void Application::syncVirtualCamera()
{
    if (!m_ui) return;

    const bool enabled = m_ui->getVirtcamEnabled();

    if (!enabled)
    {
        if (m_virtcam && m_virtcam->isRunning())
        {
            m_virtcam->stop();
            m_ui->setVirtcamStatusText("");
            m_ui->setVirtcamErrorText("");
        }
        return;
    }

    if (!VirtualCameraOutputMac::isPluginInstalled())
    {
        m_ui->setVirtcamErrorText(
            "RetroCaptureVCam.plugin is not installed in "
            "/Library/CoreMediaIO/Plug-Ins/DAL/. Run the "
            "install-virtcam.sh helper (it copies the bundle "
            "from the .app and requires sudo).");
        return;
    }

    // Resolve dims using the same cascade as Linux/Windows: UI
    // override → shader/image output → capture dims. The DAL
    // plug-in currently only advertises one fixed format (RGB24
    // @ 1280x720) so anything wildly off-spec will fall back to
    // a frozen frame on the consumer side.
    uint32_t w = m_ui->getVirtcamOutputWidth();
    uint32_t h = m_ui->getVirtcamOutputHeight();
    if (w == 0) w = m_ui->getOutputWidth();
    if (h == 0) h = m_ui->getOutputHeight();
    if (w == 0) w = m_ui->getCaptureWidth();
    if (h == 0) h = m_ui->getCaptureHeight();

    if (w == 0 || h == 0)
    {
        m_ui->setVirtcamStatusText(
            "Waiting for a capture source (open a Source first).");
        return;
    }

    // UYVY — must match the DAL plug-in's advertised '2vuy'
    // (kCVPixelFormatType_422YpCbCr8), the camera-native YUV format.
    // History: 24RGB → device shown, no image; BGRA → format
    // recognised but consumer never started the stream.
    constexpr auto fmt = VirtualCameraOutputMac::PixelFormat::UYVY;

    if (!m_virtcam) m_virtcam = std::make_unique<VirtualCameraOutputMac>();

    if (!m_virtcam->isRunning())
    {
        std::string err;
        if (!m_virtcam->start(w, h, fmt, err))
        {
            m_ui->setVirtcamErrorText(err);
            return;
        }
    }
    m_ui->setVirtcamErrorText("");

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Publishing %ux%u UYVY to RetroCaptureVCam.plugin",
                  m_virtcam->outputWidth(),
                  m_virtcam->outputHeight());
    m_ui->setVirtcamStatusText(buf);
}
#endif // __APPLE__

void Application::syncDirectoryClient()
{
    if (!m_ui) return;

    // #84 — Keep ChatClient's base URL in sync with the editable
    // Streaming → Advanced field. setBaseUrl is a no-op when the
    // value hasn't changed; on change it tears down the WS and
    // reconnects against the new URL. Runs at the top of the
    // function (before any of the publish-gated early returns)
    // so chat works whether or not the user has the directory
    // publish toggle on.
    if (m_chatClient)
    {
        m_chatClient->setBaseUrl(m_ui->getChatBaseUrl());
    }

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
        bool failing   = false;
        if (m_capture)
        {
            if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_capture.get()))
            {
                offline   = remote->isHostLikelyOffline();
                receiving = remote->isReceivingFrames();
                failing   = remote->isInitialConnectFailing();
            }
            else
            {
                receiving = m_capture->isReceivingFrames();
            }
        }
        m_ui->setRemoteHostLikelyOffline(offline);
        m_ui->setRemoteReceivingFrames(receiving);
        m_ui->setRemoteInitialConnectFailing(failing);
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

    // #84 — Chat lifecycle on the host side. As of the identity-
    // bound rework, the chat room is no longer keyed by per-session
    // streamId — it's a standalone room with a user-chosen slug,
    // provisioned once and reused across every stream. The slug
    // (m_ui->getStreamRoomSlug()) lives in the user's config and the
    // host always reconnects to that same room when the toggle is on.
    //
    // BYPASS: when the user has manually joined a *different* room
    // via the OSD popup (snap.slug non-empty AND different from the
    // configured stream slug), the auto-bind stays out of the way —
    // otherwise we'd fight the user's intent.
    if (m_chatClient)
    {
        const bool chatEnabled = m_ui->getStreamChatEnabled();
        const bool active = (state == DirectoryClient::State::Active);
        std::string configuredSlug = m_ui->getStreamRoomSlug();
        const auto snap   = m_chatClient->getSnapshot();
        // Keep DirectoryStreamId updated so other consumers (UI
        // labels, the host's own indicator) still see the session
        // id — we just don't key chat on it anymore.
        m_ui->setDirectoryStreamId(m_directoryClient->getStreamId());

        // First stream start after the user ticked "Create chat room"
        // with no slug derived yet — derive one from the title (user
        // text or fallback "Stream of <nick>") so the rest of this
        // function has something to bind to. Stored back into
        // UIManager so subsequent reconnects skip the derivation.
        if (active && chatEnabled && configuredSlug.empty())
        {
            const std::string nick = m_ui->getChatNickname();
            std::string source = m_ui->getStreamRoomTitle();
            if (source.empty() && !nick.empty())
            {
                source = std::string("Stream of ") + nick;
            }
            // Slugify: lowercase ASCII alnum + dashes, collapse
            // runs of separators, strip leading/trailing dash,
            // clamp to 32 chars to leave room for the suffix. Matches
            // the server's isValidSlug shape (handlers.go).
            auto slugify = [](const std::string &in) {
                std::string out;
                out.reserve(in.size());
                bool lastWasDash = false;
                for (char c : in)
                {
                    const unsigned char uc = static_cast<unsigned char>(c);
                    if ((uc >= 'A' && uc <= 'Z') ||
                        (uc >= 'a' && uc <= 'z') ||
                        (uc >= '0' && uc <= '9'))
                    {
                        out.push_back(static_cast<char>(std::tolower(uc)));
                        lastWasDash = false;
                    }
                    else if (!lastWasDash && !out.empty())
                    {
                        out.push_back('-');
                        lastWasDash = true;
                    }
                }
                while (!out.empty() && out.back() == '-') out.pop_back();
                if (out.size() > 32) out.resize(32);
                while (!out.empty() && out.back() == '-') out.pop_back();
                return out;
            };
            std::string slug = slugify(source);
            // Anything below 2 chars fails the server's isValidSlug.
            // Backstop with a hex-suffix slug so the user isn't
            // blocked just because their inputs were unusable.
            if (slug.size() < 2)
            {
                slug = ownedrooms::generateSecret().substr(0, 8);
            }
            else
            {
                // Disambiguate against collisions by appending the
                // first 4 hex chars of a random secret. Cheap and
                // makes the slug effectively unique-per-creation.
                slug += "-" + ownedrooms::generateSecret().substr(0, 4);
                if (slug.size() > 41) slug.resize(41);
                while (!slug.empty() && slug.back() == '-') slug.pop_back();
            }
            m_ui->setStreamRoomSlug(slug);
            m_ui->saveConfig();
            configuredSlug = slug;
        }

        const bool userPinnedElsewhere = !snap.slug.empty() &&
                                         snap.slug != configuredSlug;

        if (!userPinnedElsewhere)
        {
            if (active && chatEnabled && !configuredSlug.empty() &&
                configuredSlug != m_chatBoundSlug)
            {
                m_chatBoundSlug = configuredSlug;
                // Move the whole probe/provision/patch/connect
                // sequence off the main thread — used to block the
                // render loop for several hundred ms on every
                // stream-start. Worker does the HTTP, then calls
                // connectBySlug (itself async) at the end. All
                // ChatClient HTTP helpers are thread-safe.
                ChatClient *chat       = m_chatClient.get();
                const std::string slug = configuredSlug;
                const std::string clientId  = chat->getClientId();
                std::string nick = m_ui->getChatNickname();
                if (nick.empty()) nick = m_ui->getDirectoryHostNickname();
                std::string titleHint = m_ui->getStreamRoomTitle();
                if (titleHint.empty() && !m_ui->getChatNickname().empty())
                    titleHint = std::string("Stream of ") +
                                m_ui->getChatNickname();
                if (titleHint.empty())
                    titleHint = m_ui->getDirectoryStreamName();
                // Bump the bind epoch and capture the new value.
                // The worker checks it at the end before calling
                // connectBySlug; if the user has hit Stop Streaming
                // (or otherwise transitioned away) in the meantime,
                // m_chatBindEpoch will have advanced and the
                // worker drops the connect to avoid undoing the
                // disconnect that ran on the main thread.
                const uint64_t epoch = m_chatBindEpoch.fetch_add(1) + 1;
                std::atomic<uint64_t> *epochPtr = &m_chatBindEpoch;

                std::thread([chat, slug, clientId, nick, titleHint,
                             epoch, epochPtr]() {
                    OwnedRoom owned;
                    std::string ownerSecret;
                    if (ownedrooms::findBySlug(slug, owned))
                    {
                        ownerSecret = owned.ownerSecret;
                        // Revive if reaped.
                        std::string probeErr;
                        const bool exists = chat->roomExistsBySlug(slug, probeErr);
                        if (!exists && probeErr.empty())
                        {
                            LOG_INFO("Chat: room '" + slug +
                                     "' missing on server, reviving");
                            std::string title = titleHint;
                            if (title.empty()) title = owned.title;
                            std::string newRoomId, newSlug, err;
                            if (chat->createStandaloneRoom(
                                    title, slug,
                                    /*password=*/"", /*listed=*/true,
                                    /*ownerClientId=*/clientId,
                                    owned.ownerSecret,
                                    /*isStreamRoom=*/true,
                                    newRoomId, newSlug, err))
                            {
                                OwnedRoom rec = owned;
                                rec.roomId = newRoomId;
                                rec.title  = title;
                                ownedrooms::append(rec);
                                owned = rec;
                            }
                            else
                            {
                                LOG_WARN("Chat: revive failed: " + err);
                            }
                        }
                        // Idempotent listed=true bump.
                        std::string patchErr;
                        if (!chat->setStandaloneRoomListed(
                                owned.roomId, ownerSecret,
                                /*listed=*/true, patchErr))
                        {
                            LOG_INFO("Chat: PATCH listed=true skipped/failed: " +
                                     patchErr);
                        }
                    }
                    else
                    {
                        // First-time provision.
                        ownerSecret = ownedrooms::generateSecret();
                        std::string newRoomId, newSlug, err;
                        if (chat->createStandaloneRoom(
                                titleHint, slug,
                                /*password=*/"", /*listed=*/true,
                                /*ownerClientId=*/clientId,
                                ownerSecret,
                                /*isStreamRoom=*/true,
                                newRoomId, newSlug, err))
                        {
                            OwnedRoom rec;
                            rec.roomId      = newRoomId;
                            rec.slug        = newSlug;
                            rec.title       = titleHint;
                            rec.ownerSecret = ownerSecret;
                            ownedrooms::append(rec);
                        }
                        else
                        {
                            LOG_WARN("Chat: stream-room provision failed: " + err);
                        }
                    }
                    // Late-arrival cancel: main thread may have
                    // disconnected or rebound during the HTTP
                    // sequence. Don't undo that by reconnecting
                    // here.
                    if (epochPtr->load() != epoch)
                    {
                        LOG_INFO("Chat: bind worker stale (epoch "
                                 "advanced), skipping connectBySlug");
                        return;
                    }
                    chat->connectBySlug(slug, nick,
                                        /*password=*/"", ownerSecret);
                }).detach();
            }
            else if ((!active || !chatEnabled) && !m_chatBoundSlug.empty())
            {
                m_chatBoundSlug.clear();
                // Advance the epoch so any in-flight bind worker
                // notices its slug was abandoned and skips the
                // tail connect.
                m_chatBindEpoch.fetch_add(1);
                m_chatClient->disconnect();
            }
        }
    }
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
