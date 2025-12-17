#include "Application.h"
#include "../utils/Logger.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureFactory.h"
#ifdef PLATFORM_LINUX
#include "../v4l2/V4L2ControlMapper.h"
#endif
// FrameProcessor and OpenGLRenderer work on all platforms
#include "../processing/FrameProcessor.h"
#include "../renderer/OpenGLRenderer.h"
#include "../output/WindowManager.h"
#include "../shader/ShaderEngine.h"
#include "../ui/UIManager.h"
#include "../ui/UICapturePresets.h"
#include "../renderer/glad_loader.h"
#include "../streaming/StreamManager.h"
#include "../streaming/HTTPTSStreamer.h"
#include "../audio/IAudioCapture.h"
#include "../audio/AudioCaptureFactory.h"
#include "../utils/PresetManager.h"
#include "../utils/ThumbnailGenerator.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
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

    if (!initStreaming())
    {
        LOG_WARN("Failed to initialize streaming - continuing without streaming");
    }

    // Initialize audio capture (always required for streaming)
    if (m_streamingEnabled)
    {
        if (!initAudioCapture())
        {
            LOG_WARN("Failed to initialize audio capture - continuing without audio");
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

    LOG_INFO("Application initialized successfully");
    return true;
}

bool Application::initWindow()
{
    m_window = std::make_unique<WindowManager>();

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
    LOG_INFO("FrameProcessor created");

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
    m_capture = VideoCaptureFactory::create();
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
    if (m_frameProcessor) {
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

    // Get GLFWwindow* from WindowManager
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window->getWindow());
    if (!window)
    {
        LOG_ERROR("Failed to get GLFW window for ImGui");
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
    m_ui->setOnShaderChanged([this](const std::string &shaderPath)
                             {
        if (m_shaderEngine) {
            if (shaderPath.empty()) {
                m_shaderEngine->disableShader();
                LOG_INFO("Shader disabled");
            } else {
                // Use RETROCAPTURE_SHADER_PATH if available (for AppImage)
                const char* envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
                fs::path shaderBasePath;
                if (envShaderPath && fs::exists(envShaderPath)) {
                    shaderBasePath = fs::path(envShaderPath);
                } else {
                    shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
                }
                fs::path fullPath = shaderBasePath / shaderPath;
                if (m_shaderEngine->loadPreset(fullPath.string())) {
                    LOG_INFO("Shader loaded via UI: " + shaderPath);
                } else {
                    LOG_ERROR("Failed to load shader via UI: " + shaderPath);
                }
            }
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
        scheduleResolutionChange(width, height);
    });

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

    // Configure initial values
    m_ui->setBrightness(m_brightness);
    m_ui->setContrast(m_contrast);
    m_ui->setMaintainAspect(m_maintainAspect);
    m_ui->setFullscreen(m_fullscreen);
    m_ui->setMonitorIndex(m_monitorIndex);

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
        // Use RETROCAPTURE_SHADER_PATH if available (for AppImage)
        const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
        fs::path shaderBasePath;
        if (envShaderPath && fs::exists(envShaderPath))
        {
            shaderBasePath = fs::path(envShaderPath);
        }
        else
        {
            shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
        }
        fs::path fullPath = shaderBasePath / loadedShader;
        if (m_shaderEngine->loadPreset(fullPath.string()))
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

    m_ui->setOnStreamingWidthChanged([this](uint32_t width)
                                     { m_streamingWidth = width; });

    m_ui->setOnStreamingHeightChanged([this](uint32_t height)
                                      { m_streamingHeight = height; });

    m_ui->setOnStreamingFpsChanged([this](uint32_t fps)
                                   { m_streamingFps = fps; });

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
                                 });

    m_ui->setOnDeviceChanged([this](const std::string &devicePath)
                             {
        // Avoid infinite loop: if already processing "None", do nothing
        static bool processingDeviceChange = false;
        if (processingDeviceChange) {
            return;
        }
        processingDeviceChange = true;
        
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
        
        LOG_INFO("=== CALLBACK setOnDeviceChanged CALLED ===");
        LOG_INFO("Changing device to: " + devicePath);
        std::cout << "[FORCE] setOnDeviceChanged called with devicePath: " << devicePath << std::endl;
        
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
        fs::path basePath("shaders/shaders_glsl");
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
                LOG_ERROR("Falha ao salvar preset como: " + path);
            }
        } });
    }

    return true;
}

void Application::handleKeyInput()
{
    if (!m_ui || !m_window)
        return;

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

    // OPTION A: No more streaming thread to clean up

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
    if (!m_streamingEnabled)
    {
        return true; // Audio not enabled, not an error
    }

    m_audioCapture = AudioCaptureFactory::create();
    if (!m_audioCapture)
    {
        LOG_ERROR("Failed to create AudioCapture for this platform");
        return false;
    }

    // Open default audio device (will create virtual sink)
    if (!m_audioCapture->open())
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

    return true;
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
        m_window->pollEvents();

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
                        m_streamManager->pushAudio(audioBuffer.data(), samplesRead);

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
            else
            {
                // Streaming is not active, but we still need to process mainloop
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
            // Apply shader if active
            GLuint textureToRender = m_frameProcessor->getTexture();
            bool isShaderTexture = false;

            if (m_shaderEngine && m_shaderEngine->isShaderActive())
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

                textureToRender = m_shaderEngine->applyShader(m_frameProcessor->getTexture(),
                                                              m_frameProcessor->getTextureWidth(),
                                                              m_frameProcessor->getTextureHeight());
                isShaderTexture = true;

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

            // IMPORTANT: Camera image comes inverted (Y inverted)
            // Shaders also render inverted, so both need Y inversion
            // flipY: true for both (camera and shader need to invert)
            bool shouldFlipY = true;

            // Calculate viewport where capture will be rendered (may be smaller than window if maintainAspect is active)
            uint32_t windowWidth = m_window->getWidth();
            uint32_t windowHeight = m_window->getHeight();
            GLint viewportX = 0;
            GLint viewportY = 0;
            GLsizei viewportWidth = windowWidth;
            GLsizei viewportHeight = windowHeight;

            if (m_maintainAspect && renderWidth > 0 && renderHeight > 0)
            {
                // Calculate texture and window aspect ratio (same as renderTexture)
                float textureAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);
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

            m_renderer->renderTexture(textureToRender, m_window->getWidth(), m_window->getHeight(),
                                      shouldFlipY, isShaderTexture, m_brightness, m_contrast,
                                      m_maintainAspect, renderWidth, renderHeight);

            if (m_streamManager && m_streamManager->isActive())
            {
                uint32_t captureWidth = static_cast<uint32_t>(viewportWidth);
                uint32_t captureHeight = static_cast<uint32_t>(viewportHeight);
                size_t captureDataSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 3;

                if (captureDataSize > 0 && captureDataSize <= (7680 * 4320 * 3) &&
                    captureWidth > 0 && captureHeight > 0 && captureWidth <= 7680 && captureHeight <= 4320)
                {
                    size_t readRowSizeUnpadded = static_cast<size_t>(captureWidth) * 3;
                    size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
                    size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(captureHeight);

                    std::vector<uint8_t> frameDataWithPadding;
                    frameDataWithPadding.resize(totalSizeWithPadding);

                    glReadPixels(viewportX, viewportY, static_cast<GLsizei>(captureWidth), static_cast<GLsizei>(captureHeight),
                                 GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

                    std::vector<uint8_t> frameData;
                    frameData.resize(captureDataSize);

                    for (uint32_t row = 0; row < captureHeight; row++)
                    {
                        uint32_t srcRow = captureHeight - 1 - row; // Vertical flip
                        uint32_t dstRow = row;

                        const uint8_t *srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
                        uint8_t *dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
                        memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
                    }
                    if (m_streamManager)
                    {
                        m_streamManager->pushFrame(frameData.data(), captureWidth, captureHeight);
                    }
                }
            }

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

            // Render UI (after capturing capture area)
            if (m_ui)
            {
                m_ui->render();
                // IMPORTANT: endFrame() must be called BEFORE swapBuffers()
                // so UI is rendered to correct buffer
                m_ui->endFrame();
            }

            m_window->swapBuffers();
        }
        else
        {
            // If no valid frame yet, we still need to render UI and update window
            // so window is visible even without video frame

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
            m_window->swapBuffers();

// Do a small sleep to not consume 100% CPU
#ifdef PLATFORM_LINUX
            usleep(1000); // 1ms
#else
            Sleep(1); // 1ms sleep
#endif
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

    if (m_frameProcessor)
    {
        m_frameProcessor->deleteTexture();
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

    // OPTION A: No more streaming thread to clean up

    if (m_streamManager)
    {
        m_streamManager->cleanup();
        m_streamManager.reset();
    }

    if (m_audioCapture)
    {
        m_audioCapture->stopCapture();
        m_audioCapture->close();
        m_audioCapture.reset();
    }

    m_initialized = false;
}

void Application::schedulePresetApplication(const std::string& presetName)
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
    if (!m_capture || !m_capture->isOpen()) {
        if (!m_capture) {
            LOG_WARN("VideoCapture not initialized. Select a device first.");
            return;
        }
        
        // If not in dummy mode, try to activate
        if (!m_capture->isDummyMode()) {
            LOG_INFO("No device open. Activating dummy mode...");
            m_capture->setDummyMode(true);
        }
        
        // Configure dummy format
        if (m_capture->setFormat(width, height, 0)) {
            if (m_capture->startCapture()) {
                LOG_INFO("Dummy resolution updated: " + std::to_string(width) + "x" + std::to_string(height));
                if (m_ui) {
                    m_ui->setCaptureInfo(width, height, m_captureFps, "None (Dummy)");
                    m_ui->setCurrentDevice(""); // Empty string = None
                }
                return;
            }
        }
        LOG_WARN("Failed to configure dummy resolution. Select a device first.");
        return;
    }
    if (reconfigureCapture(width, height, m_captureFps)) {
        // Update texture if needed (use actual device values)
        uint32_t actualWidth = m_capture->getWidth();
        uint32_t actualHeight = m_capture->getHeight();
        
        // Texture was already deleted in reconfigureCapture before closing device
        // It will be recreated automatically on next frame processing
        
        // Update UI information with actual values
        if (m_ui && m_capture) {
            m_ui->setCaptureInfo(actualWidth, actualHeight, 
                                m_captureFps, m_devicePath);
        }
        
        LOG_INFO("Texture will be recreated on next frame: " + 
                 std::to_string(actualWidth) + "x" + std::to_string(actualHeight));
    } else {
        // If failed, update UI with current values
        if (m_ui && m_capture) {
            m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                m_captureFps, m_devicePath);
        }
    }
}

void Application::applyPreset(const std::string& presetName)
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
        // Resolve relative shader path to absolute
        std::string shaderPath = data.shaderPath;
        const char* envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
        fs::path shaderBasePath;
        if (envShaderPath && fs::exists(envShaderPath))
        {
            shaderBasePath = fs::path(envShaderPath);
        }
        else
        {
            shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
        }
        
        // If path is relative, make it absolute
        fs::path shaderPathObj(shaderPath);
        if (shaderPathObj.is_relative())
        {
            fs::path absolutePath = shaderBasePath / shaderPathObj;
            if (fs::exists(absolutePath))
            {
                shaderPath = absolutePath.string();
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
            for (const auto& [name, value] : data.shaderParameters)
            {
                m_shaderEngine->setShaderParameter(name, value);
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
        for (const auto& [name, value] : data.v4l2Controls)
        {
            m_capture->setControl(name, value);
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

void Application::createPresetFromCurrentState(const std::string& name, const std::string& description)
{
    if (!m_initialized)
    {
        LOG_ERROR("Cannot create preset: Application not initialized");
        return;
    }

    PresetManager presetManager;
    PresetManager::PresetData data;
    data.name = name;
    data.description = description;

    // Collect shader information
    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        std::string shaderPath = m_shaderEngine->getPresetPath();
        
        // Convert shader path to relative path (relative to shaders/shaders_glsl)
        const char* envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
        fs::path shaderBasePath;
        if (envShaderPath && fs::exists(envShaderPath))
        {
            shaderBasePath = fs::path(envShaderPath);
        }
        else
        {
            shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
        }
        
        // Make path relative to shader base
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
        for (const auto& param : params)
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
    }

    // Collect V4L2 controls (if applicable)
    if (m_capture && m_capture->isOpen())
    {
        // Get common V4L2 controls
        std::vector<std::string> controlNames = {"Brightness", "Contrast", "Saturation", "Hue"};
        for (const auto& controlName : controlNames)
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
