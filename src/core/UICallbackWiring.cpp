#include "UICallbackWiring.h"
#include "Application.h"
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

UICallbackWiring::UICallbackWiring(Application &app) : m_app(app)
{
}

void UICallbackWiring::wireAll()
{
    wireVisualCallbacks();
    wireStreamingCallbacks();
    wireRecordingCallbacks();
    wireWebPortalCallbacks();
    wireSourceAndMiscCallbacks();
}

void UICallbackWiring::wireVisualCallbacks()
{
    m_app.m_ui->setOnVisibilityChanged([this](bool /* visible */)
                                  {
        m_app.updateCursorVisibility();
    });

    // Set initial cursor visibility based on UI visibility
    // This ensures cursor is hidden if UI starts hidden (e.g., --hide-ui flag)
    m_app.updateCursorVisibility();

    m_app.m_ui->setOnShaderChanged([this](const std::string &shaderPath)
                             {
        if (!m_app.m_shaderEngine) return;

        if (shaderPath.empty()) {
            m_app.m_shaderEngine->disableShader();
            LOG_INFO("Shader disabled");
            return;
        }

        std::string fullPath = m_app.resolveShaderPath(shaderPath);

        // Idempotência: se o shader pedido já é o ativo, não recarrega.
        // `applyPreset` aplica params custom *antes* de sincronizar a UI;
        // recarregar aqui zeraria m_customParameters do ShaderEngine
        // (vide ShaderEngine::loadPreset) e os overrides do capture preset
        // se perderiam silenciosamente.
        if (m_app.m_shaderEngine->isShaderActive() &&
            m_app.m_shaderEngine->getPresetPath() == fullPath) {
            return;
        }

        if (m_app.m_shaderEngine->loadPreset(fullPath)) {
            LOG_INFO("Shader loaded via UI: " + shaderPath);
        } else {
            LOG_ERROR("Failed to load shader via UI: " + shaderPath);
        } });

    m_app.m_ui->setOnBrightnessChanged([this](float brightness)
                                 { m_app.m_brightness = brightness; });

    m_app.m_ui->setOnContrastChanged([this](float contrast)
                               { m_app.m_contrast = contrast; });

    m_app.m_ui->setOnMaintainAspectChanged([this](bool maintain)
                                     { m_app.m_maintainAspect = maintain; });

    m_app.m_ui->setOnFullscreenChanged([this](bool fullscreen)
                                 {
        LOG_INFO("Fullscreen toggle requested: " + std::string(fullscreen ? "ON" : "OFF"));
        // IMPORTANT: Make fullscreen change asynchronously to avoid freezing
        // The resize callback will be called automatically by GLFW when window changes
        // Don't do blocking operations here
        if (m_app.m_window) {
            m_app.m_fullscreen = fullscreen;
            // The fullscreen change will be done in the next frame of main loop
            // to avoid deadlocks and freezing
            m_app.m_pendingFullscreenChange = true;
        } });

    m_app.m_ui->setOnMonitorIndexChanged([this](int monitorIndex)
                                   {
        LOG_INFO("Monitor index changed: " + std::to_string(monitorIndex));
        m_app.m_monitorIndex = monitorIndex;
        // If in fullscreen, update to use new monitor
        if (m_app.m_fullscreen && m_app.m_window) {
            m_app.m_window->setFullscreen(true, monitorIndex);
            
            // Update shader engine viewport after monitor change
            if (m_app.m_shaderEngine) {
                uint32_t currentWidth = m_app.m_window->getWidth();
                uint32_t currentHeight = m_app.m_window->getHeight();
                m_app.m_shaderEngine->setViewport(currentWidth, currentHeight);
            }
        } });

    m_app.m_ui->setOnOutputResolutionChanged([this](uint32_t width, uint32_t height)
                                       {
        LOG_INFO("Output resolution changed: " + std::to_string(width) + "x" + std::to_string(height) + 
                 (width == 0 && height == 0 ? " (automatic)" : ""));
        m_app.m_outputWidth = width;
        m_app.m_outputHeight = height; });

    m_app.m_ui->setOnV4L2ControlChanged([this](const std::string &name, int32_t value)
                                  {
        if (!m_app.m_capture) return;
        
        // Use generic interface to set control
        int32_t minVal, maxVal;
        if (m_app.m_capture->getControlMin(name, minVal) && 
            m_app.m_capture->getControlMax(name, maxVal)) {
            // Clamp ao range
            value = std::max(minVal, std::min(maxVal, value));
        }
        
        m_app.m_capture->setControl(name, value); });

    m_app.m_ui->setOnResolutionChanged([this](uint32_t width, uint32_t height)
                                 {
        // Schedule resolution change for main thread (thread-safe)
        // This is necessary because the callback may be called from API threads
        m_app.scheduleResolutionChange(width, height); });

    m_app.m_ui->setOnFramerateChanged([this](uint32_t fps)
                                {
        LOG_INFO("Framerate changed via UI: " + std::to_string(fps) + "fps");
        // Update FPS in configuration
        m_app.m_captureFps = fps;
        
        // If no device is open, just update configuration (dummy mode doesn't need reconfiguration)
        if (!m_app.m_capture || !m_app.m_capture->isOpen()) {
            if (m_app.m_capture && m_app.m_capture->isDummyMode()) {
                LOG_INFO("Framerate updated for dummy mode: " + std::to_string(fps) + "fps");
            } else {
                LOG_WARN("No device open. FPS will be applied when a device is selected.");
            }
            return;
        }
        if (m_app.reconfigureCapture(m_app.m_captureWidth, m_app.m_captureHeight, fps)) {
            m_app.m_captureFps = fps;
            // Update UI information
            if (m_app.m_ui && m_app.m_capture) {
                m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(), 
                                    m_app.m_captureFps, m_app.m_devicePath);
            }
        } });

    // IMPORTANT: UIManager has already loaded saved configurations in its constructor
    // So we should read FROM UI first, then set callbacks
    // This ensures saved values are not overwritten by default values

    // Read saved values from UI (loaded from config file)
    m_app.m_brightness = m_app.m_ui->getBrightness();
    m_app.m_contrast = m_app.m_ui->getContrast();
    m_app.m_maintainAspect = m_app.m_ui->getMaintainAspect();
    m_app.m_fullscreen = m_app.m_ui->getFullscreen();
    m_app.m_monitorIndex = m_app.m_ui->getMonitorIndex();

    // Read saved capture resolution from UI (loaded from config file)
    // The UIManager loads config in its constructor, so values are available here
    uint32_t savedWidth = m_app.m_ui->getCaptureWidth();
    uint32_t savedHeight = m_app.m_ui->getCaptureHeight();
    uint32_t savedFps = m_app.m_ui->getCaptureFps();
    std::string savedDevice = m_app.m_ui->getCurrentDevice();

    // Use saved values if they exist (savedWidth/Height > 0 means config was loaded)
    // Only override if we have valid saved values
    bool useSavedResolution = (savedWidth > 0 && savedHeight > 0);

    // Check if current values are the defaults (1920x1080) - if so, likely not set via command line
    bool isDefaultResolution = (m_app.m_captureWidth == 1920 && m_app.m_captureHeight == 1080);

    if (useSavedResolution && ((m_app.m_captureWidth == 0 && m_app.m_captureHeight == 0) || isDefaultResolution))
    {
        LOG_INFO("Using saved capture resolution: " +
                 std::to_string(savedWidth) + "x" + std::to_string(savedHeight) +
                 " @ " + std::to_string(savedFps) + "fps");
        m_app.m_captureWidth = savedWidth;
        m_app.m_captureHeight = savedHeight;
        if (savedFps > 0)
        {
            m_app.m_captureFps = savedFps;
        }

        // If capture is already initialized, reconfigure it with saved resolution
        if (m_app.m_capture && (m_app.m_capture->isOpen() || m_app.m_capture->isDummyMode()))
        {
            LOG_INFO("Reconfiguring capture with saved resolution...");
            if (m_app.m_capture->isDummyMode() || !m_app.m_capture->isOpen())
            {
                // For dummy mode or closed device, just reconfigure
                m_app.m_capture->stopCapture();
                m_app.m_capture->close();
                m_app.m_capture->setDummyMode(true);
                if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                {
                    m_app.m_capture->startCapture();
                    if (m_app.m_ui)
                    {
                        m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                             m_app.m_captureFps, "None (Dummy)");
                    }
                }
            }
            else
            {
                // For real device, use reconfigureCapture
                m_app.reconfigureCapture(m_app.m_captureWidth, m_app.m_captureHeight, m_app.m_captureFps);
            }
        }
    }

    // Trocar pro dispositivo salvo se diferente do atual. initCapture já abriu
    // o default (/dev/video0 no Linux ou primeiro DirectShow no Windows). Só
    // mexemos se o config tem um valor não-vazio diferente.
    if (m_app.m_capture && !savedDevice.empty() && savedDevice != m_app.m_devicePath)
    {
        LOG_INFO("Switching to saved device: " + savedDevice);
        m_app.m_capture->stopCapture();
        m_app.m_capture->close();
        if (m_app.m_capture->open(savedDevice))
        {
            m_app.m_devicePath = savedDevice;
            if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
            {
                m_app.m_capture->setFramerate(m_app.m_captureFps);
                if (m_app.m_capture->startCapture())
                {
                    if (m_app.m_ui)
                    {
                        m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                             m_app.m_captureFps, m_app.m_devicePath);
                        m_app.m_ui->setCurrentDevice(m_app.m_devicePath);
                    }
                    LOG_INFO("Saved device opened: " + savedDevice);
                }
            }
        }
        else
        {
            LOG_WARN("Failed to open saved device " + savedDevice + ", reverting to default.");
            // Reabre o default que estava antes — best-effort.
            if (m_app.m_capture->open(m_app.m_devicePath))
            {
                m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0);
                m_app.m_capture->setFramerate(m_app.m_captureFps);
                m_app.m_capture->startCapture();
            }
        }
    }

    // Now set callbacks so future changes are synchronized
    // (Values are already set above, so this won't overwrite saved config)

    // Check initial source type and configure appropriately
    if (m_app.m_ui->getSourceType() == UIManager::SourceType::None)
    {
        // If None is selected, ensure dummy mode is active
        if (m_app.m_capture)
        {
            if (!m_app.m_capture->isDummyMode() || !m_app.m_capture->isOpen())
            {
                m_app.m_capture->stopCapture();
                m_app.m_capture->close();
                m_app.m_capture->setDummyMode(true);
                if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                {
                    if (m_app.m_capture->startCapture())
                    {
                        m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                             m_app.m_captureFps, "None (Dummy)");
                    }
                }
            }
        }
        m_app.m_ui->setCaptureControls(nullptr);
    }
    else
    {
        // Configure V4L2 controls if device is open
        // IMPORTANT: Always pass m_capture to UIManager, even if not open,
        // to allow device enumeration (especially for DirectShow)
        if (m_app.m_capture)
        {
            m_app.m_ui->setCaptureControls(m_app.m_capture.get());
        }
        else
        {
            // Without capture, don't allow selection
            m_app.m_ui->setCaptureControls(nullptr);
        }
    }

    // Configure capture information
    if (m_app.m_capture && m_app.m_capture->isOpen())
    {
        m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                             m_app.m_captureFps, m_app.m_devicePath);
        m_app.m_ui->setCurrentDevice(m_app.m_devicePath);
    }
    else
    {
        // No device - show "None"
        m_app.m_ui->setCaptureInfo(0, 0, 0, "None");
        m_app.m_ui->setCurrentDevice(""); // Empty string = None
    }

    // Connect Application to UICapturePresets
    if (m_app.m_ui->getCapturePresetsWindow())
    {
        m_app.m_ui->getCapturePresetsWindow()->setApplication(&m_app);
    }

    if (m_app.m_ui->getRecordingsWindow())
    {
        m_app.m_ui->getRecordingsWindow()->setApplication(&m_app);
    }

    // IMPORTANT: After init(), UIManager has already loaded saved configurations
    // Synchronize Application values with values loaded from UI
    // This ensures saved configurations are applied
    //
    // Port exception (#163): an explicit CLI --stream-port / --web-portal-port
    // must win over the saved config. When set, push the Application value into
    // the UI (so the bind, URLs and directory all agree); otherwise adopt the
    // UI's saved port as before.
    if (m_app.m_streamingPortExplicit)
        m_app.m_ui->setStreamingPort(m_app.m_streamingPort);
    else
        m_app.m_streamingPort = m_app.m_ui->getStreamingPort();
    m_app.m_streamingWidth = m_app.m_ui->getStreamingWidth();
    m_app.m_streamingHeight = m_app.m_ui->getStreamingHeight();
    m_app.m_streamingFps = m_app.m_ui->getStreamingFps();
    m_app.m_streamingBitrate = m_app.m_ui->getStreamingBitrate();
    m_app.m_streamingAudioBitrate = m_app.m_ui->getStreamingAudioBitrate();
    m_app.m_streamingVideoCodec = m_app.m_ui->getStreamingVideoCodec();
    m_app.m_streamingAudioCodec = m_app.m_ui->getStreamingAudioCodec();
    m_app.m_streamingH264Preset = m_app.m_ui->getStreamingH264Preset();
    m_app.m_streamingHardwareEncoder = m_app.m_ui->getStreamingHardwareEncoder();
    m_app.m_streamingNvencPreset = m_app.m_ui->getStreamingNvencPreset();
    m_app.m_streamingVaapiRcMode = m_app.m_ui->getStreamingVaapiRcMode();
    m_app.m_streamingQsvPreset   = m_app.m_ui->getStreamingQsvPreset();
    m_app.m_streamingAmfQuality  = m_app.m_ui->getStreamingAmfQuality();
    m_app.m_remoteInterpolation  = m_app.m_ui->getRemoteInterpolation();
    m_app.m_remoteAudioGain      = m_app.m_ui->getRemoteAudioMuted() ? 0.0f : m_app.m_ui->getRemoteAudioVolume();
    m_app.m_streamingH265Preset = m_app.m_ui->getStreamingH265Preset();
    m_app.m_streamingH265Profile = m_app.m_ui->getStreamingH265Profile();
    m_app.m_streamingH265Level = m_app.m_ui->getStreamingH265Level();
    m_app.m_streamingVP8Speed = m_app.m_ui->getStreamingVP8Speed();
    m_app.m_streamingVP9Speed = m_app.m_ui->getStreamingVP9Speed();

    // Load streaming buffer parameters
    m_app.m_streamingMaxVideoBufferSize = m_app.m_ui->getStreamingMaxVideoBufferSize();
    m_app.m_streamingMaxAudioBufferSize = m_app.m_ui->getStreamingMaxAudioBufferSize();
    m_app.m_streamingMaxBufferTimeSeconds = m_app.m_ui->getStreamingMaxBufferTimeSeconds();
    m_app.m_streamingAVIOBufferSize = m_app.m_ui->getStreamingAVIOBufferSize();

    // Load recording settings from UI
    if (m_app.m_recordingManager)
    {
        RecordingSettings settings;
        settings.width = m_app.m_ui->getRecordingWidth();
        settings.height = m_app.m_ui->getRecordingHeight();
        settings.fps = m_app.m_ui->getRecordingFps();
        settings.bitrate = m_app.m_ui->getRecordingBitrate();
        settings.codec = m_app.m_ui->getRecordingVideoCodec();
        settings.preset = (settings.codec == "h264") ? m_app.m_ui->getRecordingH264Preset() : m_app.m_ui->getRecordingH265Preset();
        settings.h265Profile = m_app.m_ui->getRecordingH265Profile();
        settings.h265Level = m_app.m_ui->getRecordingH265Level();
        settings.vp8Speed = m_app.m_ui->getRecordingVP8Speed();
        settings.vp9Speed = m_app.m_ui->getRecordingVP9Speed();
        settings.audioBitrate = m_app.m_ui->getRecordingAudioBitrate();
        settings.audioCodec = m_app.m_ui->getRecordingAudioCodec();
        settings.container = m_app.m_ui->getRecordingContainer();
        settings.outputPath = m_app.m_ui->getRecordingOutputPath();
        settings.filenameTemplate = m_app.m_ui->getRecordingFilenameTemplate();
        settings.includeAudio = m_app.m_ui->getRecordingIncludeAudio();
        // Hardware encoder + backend-specific preset (#59) — resolved
        // from the UI's per-backend preset fields based on the user's
        // selected backend. Auto/Software leave hwPreset empty so
        // MediaEncoder uses its compiled-in default.
        settings.hardwareEncoder = m_app.m_ui->getRecordingHardwareEncoder();
        switch (settings.hardwareEncoder)
        {
            case 2: settings.hwPreset = m_app.m_ui->getRecordingNvencPreset(); break; // NVENC
            case 3: settings.hwPreset = m_app.m_ui->getRecordingVaapiRcMode(); break; // VAAPI
            case 4: settings.hwPreset = m_app.m_ui->getRecordingQsvPreset();   break; // QSV
            case 5: settings.hwPreset = m_app.m_ui->getRecordingAmfQuality();  break; // AMF
            default: settings.hwPreset.clear(); break;                          // Auto / Software
        }
        m_app.m_recordingManager->setRecordingSettings(settings);
    }

    // Load buffer settings (already loaded by UIManager from config file)

    // Load Web Portal settings
    m_app.m_webPortalEnabled = m_app.m_ui->getWebPortalEnabled();
    m_app.m_webPortalHTTPSEnabled = m_app.m_ui->getWebPortalHTTPSEnabled();
    m_app.m_webPortalSSLCertPath = m_app.m_ui->getWebPortalSSLCertPath();
    m_app.m_webPortalSSLKeyPath = m_app.m_ui->getWebPortalSSLKeyPath();
    m_app.m_webPortalTitle = m_app.m_ui->getWebPortalTitle();
    m_app.m_webPortalSubtitle = m_app.m_ui->getWebPortalSubtitle();
    m_app.m_webPortalImagePath = m_app.m_ui->getWebPortalImagePath();
    m_app.m_webPortalBackgroundImagePath = m_app.m_ui->getWebPortalBackgroundImagePath();

    // Load editable texts
    m_app.m_webPortalTextStreamInfo = m_app.m_ui->getWebPortalTextStreamInfo();
    m_app.m_webPortalTextQuickActions = m_app.m_ui->getWebPortalTextQuickActions();
    m_app.m_webPortalTextCompatibility = m_app.m_ui->getWebPortalTextCompatibility();
    m_app.m_webPortalTextStatus = m_app.m_ui->getWebPortalTextStatus();
    m_app.m_webPortalTextCodec = m_app.m_ui->getWebPortalTextCodec();
    m_app.m_webPortalTextResolution = m_app.m_ui->getWebPortalTextResolution();
    m_app.m_webPortalTextStreamUrl = m_app.m_ui->getWebPortalTextStreamUrl();
    m_app.m_webPortalTextCopyUrl = m_app.m_ui->getWebPortalTextCopyUrl();
    m_app.m_webPortalTextOpenNewTab = m_app.m_ui->getWebPortalTextOpenNewTab();
    m_app.m_webPortalTextSupported = m_app.m_ui->getWebPortalTextSupported();
    m_app.m_webPortalTextFormat = m_app.m_ui->getWebPortalTextFormat();
    m_app.m_webPortalTextCodecInfo = m_app.m_ui->getWebPortalTextCodecInfo();
    m_app.m_webPortalTextSupportedBrowsers = m_app.m_ui->getWebPortalTextSupportedBrowsers();
    m_app.m_webPortalTextFormatInfo = m_app.m_ui->getWebPortalTextFormatInfo();
    m_app.m_webPortalTextCodecInfoValue = m_app.m_ui->getWebPortalTextCodecInfoValue();
    m_app.m_webPortalTextConnecting = m_app.m_ui->getWebPortalTextConnecting();

    // Load colors (with safety check)
    const float *bg = m_app.m_ui->getWebPortalColorBackground();
    if (bg)
    {
        memcpy(m_app.m_webPortalColorBackground, bg, 4 * sizeof(float));
    }
    const float *txt = m_app.m_ui->getWebPortalColorText();
    if (txt)
    {
        memcpy(m_app.m_webPortalColorText, txt, 4 * sizeof(float));
    }
    const float *prim = m_app.m_ui->getWebPortalColorPrimary();
    if (prim)
    {
        memcpy(m_app.m_webPortalColorPrimary, prim, 4 * sizeof(float));
    }
    const float *primLight = m_app.m_ui->getWebPortalColorPrimaryLight();
    if (primLight)
    {
        memcpy(m_app.m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
    }
    const float *primDark = m_app.m_ui->getWebPortalColorPrimaryDark();
    if (primDark)
    {
        memcpy(m_app.m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
    }
    const float *sec = m_app.m_ui->getWebPortalColorSecondary();
    if (sec)
    {
        memcpy(m_app.m_webPortalColorSecondary, sec, 4 * sizeof(float));
    }
    const float *secHighlight = m_app.m_ui->getWebPortalColorSecondaryHighlight();
    if (secHighlight)
    {
        memcpy(m_app.m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
    }
    const float *ch = m_app.m_ui->getWebPortalColorCardHeader();
    if (ch)
    {
        memcpy(m_app.m_webPortalColorCardHeader, ch, 4 * sizeof(float));
    }
    const float *bord = m_app.m_ui->getWebPortalColorBorder();
    if (bord)
    {
        memcpy(m_app.m_webPortalColorBorder, bord, 4 * sizeof(float));
    }
    const float *succ = m_app.m_ui->getWebPortalColorSuccess();
    if (succ)
    {
        memcpy(m_app.m_webPortalColorSuccess, succ, 4 * sizeof(float));
    }
    const float *warn = m_app.m_ui->getWebPortalColorWarning();
    if (warn)
    {
        memcpy(m_app.m_webPortalColorWarning, warn, 4 * sizeof(float));
    }
    const float *dang = m_app.m_ui->getWebPortalColorDanger();
    if (dang)
    {
        memcpy(m_app.m_webPortalColorDanger, dang, 4 * sizeof(float));
    }
    const float *inf = m_app.m_ui->getWebPortalColorInfo();
    if (inf)
    {
        memcpy(m_app.m_webPortalColorInfo, inf, 4 * sizeof(float));
    }

    // Also synchronize image settings
    m_app.m_brightness = m_app.m_ui->getBrightness();
    m_app.m_contrast = m_app.m_ui->getContrast();
    m_app.m_maintainAspect = m_app.m_ui->getMaintainAspect();
    m_app.m_fullscreen = m_app.m_ui->getFullscreen();
    m_app.m_monitorIndex = m_app.m_ui->getMonitorIndex();

    // Apply loaded shader if available
    std::string loadedShader = m_app.m_ui->getCurrentShader();
    if (!loadedShader.empty() && m_app.m_shaderEngine)
    {
        // Use centralized shader path resolution
        std::string fullPath = m_app.resolveShaderPath(loadedShader);
        if (m_app.m_shaderEngine->loadPreset(fullPath))
        {
            LOG_INFO("Shader loaded from configuration: " + loadedShader);
        }
    }

    // Apply image settings
    // FrameProcessor applies brightness/contrast during processing, no need to set here

    // Apply fullscreen if needed
    if (m_app.m_fullscreen && m_app.m_window)
    {
        m_app.m_window->setFullscreen(m_app.m_fullscreen, m_app.m_monitorIndex);
    }

}

void UICallbackWiring::wireStreamingCallbacks()
{
    m_app.m_ui->setOnStreamingStartStop([this](bool start)
                                  {
        // CRITICAL: This callback runs in main thread (ImGui render thread)
        // DO NOT do ANY blocking operations here - just set flag and create thread
        // DO NOT access m_streamManager or other shared resources here
        
        LOG_INFO("[CALLBACK] Streaming " + std::string(start ? "START" : "STOP") + " - creating thread...");
        
        if (start) {
            // Check if can start (not in cooldown)
            if (m_app.m_ui && !m_app.m_ui->canStartStreaming()) {
                int64_t cooldownMs = m_app.m_ui->getStreamingCooldownRemainingMs();
                int cooldownSeconds = static_cast<int>(cooldownMs / 1000);
                LOG_WARN("Streaming start attempt blocked - still in cooldown. Wait " + 
                         std::to_string(cooldownSeconds) + " seconds");
                if (m_app.m_ui) {
                    m_app.m_ui->setStreamingProcessing(false); // Resetar flag se bloqueado
                }
                return; // Don't start if in cooldown
            }
            
            // Just set flag - separate thread will do all the work
            m_app.m_streamingEnabled = true;
            
            // Update status immediately to "starting" (will be updated again when actually starting)
            if (m_app.m_ui) {
                m_app.m_ui->setStreamingActive(false); // Not active yet, but starting
            }
            
            // Create separate thread immediately - don't wait
            std::thread([this]() {
                // All blocking operations should be here, in the separate thread
                bool success = false;
                try {
                    if (m_app.initStreaming()) {
                        // Initialize audio capture (always required for streaming)
                        if (!m_app.m_audioCapture) {
                            if (!m_app.initAudioCapture()) {
                                LOG_WARN("Failed to initialize audio capture - continuing without audio");
                            }
                        }
                        success = true;
                    } else {
                        LOG_ERROR("Failed to start streaming");
                        m_app.m_streamingEnabled = false;
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception starting streaming: " + std::string(e.what()));
                    m_app.m_streamingEnabled = false;
                }
                
                // Update UI after initialization (can be called from any thread)
                // IMPORTANT: Check if m_streamManager exists before calling isActive()
                if (m_app.m_ui) {
                    bool active = success && m_app.m_streamManager && m_app.m_streamManager->isActive();
                    m_app.m_ui->setStreamingActive(active);
                    m_app.m_ui->setStreamingProcessing(false); // Processing completed
                    // initStreaming() calls stopWebPortal() before starting, which clears
                    // setWebPortalActive(false). When the streaming server itself hosts the
                    // portal (m_webPortalEnabled), it's effectively active again — reflect
                    // that back in the native UI so it doesn't read "Portal Web Inativo"
                    // while the portal is reachable through the stream port.
                    if (active && m_app.m_webPortalEnabled) {
                        m_app.m_ui->setWebPortalActive(true);
                    }
                }
            }).detach();
        } else {
            // Stop streaming also in separate thread to not block UI
            m_app.m_streamingEnabled = false;
            
            // Update status immediately when stopping
            if (m_app.m_ui) {
                m_app.m_ui->setStreamingActive(false);
                m_app.m_ui->setStreamUrl("");
                m_app.m_ui->setStreamClientCount(0);
            }
            
            // Create separate thread immediately - don't wait
            std::thread([this]() {
                try {
                    if (m_app.m_streamManager) {
                        // Ordem correta: stop() primeiro, depois cleanup()
                        m_app.m_streamManager->stop();
                        m_app.m_streamManager->cleanup();
                        m_app.m_streamManager.reset();
                        m_app.m_currentStreamer = nullptr; // Clear streamer reference
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception stopping streaming: " + std::string(e.what()));
                }
                
                // Ensure status is updated after stopping
                if (m_app.m_ui) {
                    m_app.m_ui->setStreamingActive(false);
                    m_app.m_ui->setStreamUrl("");
                    m_app.m_ui->setStreamClientCount(0);
                    m_app.m_ui->setStreamingProcessing(false); // Processing completed
                    // Streaming was hosting the portal — once it stops, the portal
                    // isn't actually reachable anymore. Mirror that in the UI so the
                    // "Portal Web Ativo" badge doesn't lie.
                    m_app.m_ui->setWebPortalActive(false);
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

    m_app.m_ui->setOnStreamingPortChanged([this](uint16_t port)
                                    {
        m_app.m_streamingPort = port;
        // If streaming is active, restart
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    // Resolution / FPS changes need the same restart-if-streaming policy
    // as bitrate/preset — initStreaming() bakes width/height/fps into the
    // MediaEncoder + MediaMuxer at construction time, so just storing the
    // new value on the field has no effect on a live stream.
    m_app.m_ui->setOnStreamingWidthChanged([this](uint32_t width) {
        m_app.m_streamingWidth = width;
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        }
    });

    m_app.m_ui->setOnStreamingHeightChanged([this](uint32_t height) {
        m_app.m_streamingHeight = height;
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        }
    });

    m_app.m_ui->setOnStreamingFpsChanged([this](uint32_t fps) {
        m_app.m_streamingFps = fps;
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        }
    });

    m_app.m_ui->setOnStreamingBitrateChanged([this](uint32_t bitrate)
                                       {
        m_app.m_streamingBitrate = bitrate;
        // Update streamer bitrate if active
        if (m_app.m_streamManager && m_app.m_streamManager->isActive()) {
            // Restart streaming with new bitrate
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingAudioBitrateChanged([this](uint32_t bitrate)
                                            {
        m_app.m_streamingAudioBitrate = bitrate;
        // If streaming is active, restart
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingVideoCodecChanged([this](const std::string &codec)
                                          {
        m_app.m_streamingVideoCodec = codec;
        // If streaming is active, restart
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingAudioCodecChanged([this](const std::string &codec)
                                          {
        m_app.m_streamingAudioCodec = codec;
        // If streaming is active, restart
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingH264PresetChanged([this](const std::string &preset)
                                          {
        m_app.m_streamingH264Preset = preset;
        // If streaming is active, restart to apply new preset
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingH265PresetChanged([this](const std::string &preset)
                                          {
        m_app.m_streamingH265Preset = preset;
        // If streaming is active, restart to apply new preset
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingH265ProfileChanged([this](const std::string &profile)
                                           {
        m_app.m_streamingH265Profile = profile;
        // If streaming is active, restart to apply new profile
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingH265LevelChanged([this](const std::string &level)
                                         {
        m_app.m_streamingH265Level = level;
        // If streaming is active, restart to apply new level
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingVP8SpeedChanged([this](int speed)
                                        {
        m_app.m_streamingVP8Speed = speed;
        // If streaming is active, restart to apply new speed
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingVP9SpeedChanged([this](int speed)
                                        {
        m_app.m_streamingVP9Speed = speed;
        // If streaming is active, restart to apply new speed
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnStreamingHardwareEncoderChanged([this](int v)
                                               {
        m_app.m_streamingHardwareEncoder = v;
        // Encoder backend is set at codec-init time inside MediaEncoder;
        // a live stream needs a restart for the new selection to take
        // effect (just like a preset / bitrate change does).
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    // Per-backend preset/quality strings — same restart-on-change
    // policy as above, since these all end up in opts that are passed
    // to avcodec_open2 at MediaEncoder::initializeHardwareVideoCodec.
    auto restartIfStreaming = [this] {
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        }
    };
    m_app.m_ui->setOnStreamingNvencPresetChanged([this, restartIfStreaming](const std::string &v) {
        m_app.m_streamingNvencPreset = v; restartIfStreaming();
    });
    m_app.m_ui->setOnStreamingVaapiRcModeChanged([this, restartIfStreaming](const std::string &v) {
        m_app.m_streamingVaapiRcMode = v; restartIfStreaming();
    });
    m_app.m_ui->setOnStreamingQsvPresetChanged([this, restartIfStreaming](const std::string &v) {
        m_app.m_streamingQsvPreset = v; restartIfStreaming();
    });
    m_app.m_ui->setOnStreamingAmfQualityChanged([this, restartIfStreaming](const std::string &v) {
        m_app.m_streamingAmfQuality = v; restartIfStreaming();
    });

    // Remote interpolation mode — applied immediately to the active
    // VideoCaptureRemote (if any). No streaming restart required since
    // it's a client-side display decision.
    m_app.m_ui->setOnRemoteInterpolationChanged([this](const std::string &v) {
        m_app.m_remoteInterpolation = v;
        if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()))
        {
            VideoCaptureRemote::InterpolationMode mode = VideoCaptureRemote::InterpolationMode::Linear;
            if (v == "nearest") mode = VideoCaptureRemote::InterpolationMode::Nearest;
            else if (v == "off") mode = VideoCaptureRemote::InterpolationMode::Off;
            remote->setInterpolationMode(mode);
        }
    });

    // #77 Remote audio volume — UIManager hands us the effective gain
    // (muted ? 0 : volume); push it live to the active VideoCaptureRemote
    // (and remember it for any remote created later this session).
    m_app.m_ui->setOnRemoteAudioVolumeChanged([this](float gain) {
        m_app.m_remoteAudioGain = gain;
        if (auto *remote = dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()))
        {
            remote->setAudioVolume(gain);
        }
    });

    // #107 — live screen-capture region crop. Push the new rect to the
    // active VideoCaptureScreen so the crop changes without a restart.
    m_app.m_ui->setOnScreenRegionChanged([this](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        if (auto *screen = dynamic_cast<VideoCaptureScreen *>(m_app.m_capture.get()))
        {
            screen->setRegion(x, y, w, h);
        }
    });

    // Callbacks for buffer settings
    m_app.m_ui->setOnStreamingMaxVideoBufferSizeChanged([this](size_t size)
                                                  {
        m_app.m_streamingMaxVideoBufferSize = size;
        if (m_app.m_currentStreamer) {
            m_app.m_currentStreamer->setBufferConfig(
                m_app.m_streamingMaxVideoBufferSize,
                m_app.m_streamingMaxAudioBufferSize,
                m_app.m_streamingMaxBufferTimeSeconds,
                m_app.m_streamingAVIOBufferSize);
        } });

    m_app.m_ui->setOnStreamingMaxAudioBufferSizeChanged([this](size_t size)
                                                  {
        m_app.m_streamingMaxAudioBufferSize = size;
        if (m_app.m_currentStreamer) {
            m_app.m_currentStreamer->setBufferConfig(
                m_app.m_streamingMaxVideoBufferSize,
                m_app.m_streamingMaxAudioBufferSize,
                m_app.m_streamingMaxBufferTimeSeconds,
                m_app.m_streamingAVIOBufferSize);
        } });

    m_app.m_ui->setOnStreamingMaxBufferTimeSecondsChanged([this](int64_t seconds)
                                                    {
        m_app.m_streamingMaxBufferTimeSeconds = seconds;
        if (m_app.m_currentStreamer) {
            m_app.m_currentStreamer->setBufferConfig(
                m_app.m_streamingMaxVideoBufferSize,
                m_app.m_streamingMaxAudioBufferSize,
                m_app.m_streamingMaxBufferTimeSeconds,
                m_app.m_streamingAVIOBufferSize);
        } });

    m_app.m_ui->setOnStreamingAVIOBufferSizeChanged([this](size_t size)
                                              {
        m_app.m_streamingAVIOBufferSize = size;
        if (m_app.m_currentStreamer) {
            m_app.m_currentStreamer->setBufferConfig(
                m_app.m_streamingMaxVideoBufferSize,
                m_app.m_streamingMaxAudioBufferSize,
                m_app.m_streamingMaxBufferTimeSeconds,
                m_app.m_streamingAVIOBufferSize);
        } });

}

void UICallbackWiring::wireRecordingCallbacks()
{
    // Recording callbacks
    m_app.m_ui->setOnRecordingStartStop([this](bool start)
                                  {
        if (start) {
            if (m_app.m_recordingManager) {
                RecordingSettings settings;
                // Use actual capture resolution and FPS, not UI settings
                // This ensures the recording matches what's being captured
                // CRITICAL: Use actual capture FPS to prevent video appearing sped up
                settings.width = m_app.m_captureWidth;
                settings.height = m_app.m_captureHeight;
                settings.fps = m_app.m_captureFps; // Use actual capture FPS, not UI setting
                settings.bitrate = m_app.m_ui->getRecordingBitrate();
                settings.codec = m_app.m_ui->getRecordingVideoCodec();
                settings.preset = (settings.codec == "h264") ? m_app.m_ui->getRecordingH264Preset() : m_app.m_ui->getRecordingH265Preset();
                settings.h265Profile = m_app.m_ui->getRecordingH265Profile();
                settings.h265Level = m_app.m_ui->getRecordingH265Level();
                settings.vp8Speed = m_app.m_ui->getRecordingVP8Speed();
                settings.vp9Speed = m_app.m_ui->getRecordingVP9Speed();
                settings.audioBitrate = m_app.m_ui->getRecordingAudioBitrate();
                settings.audioCodec = m_app.m_ui->getRecordingAudioCodec();
                settings.container = m_app.m_ui->getRecordingContainer();
                settings.outputPath = m_app.m_ui->getRecordingOutputPath();
                settings.filenameTemplate = m_app.m_ui->getRecordingFilenameTemplate();
                settings.includeAudio = m_app.m_ui->getRecordingIncludeAudio();
                settings.hardwareEncoder = m_app.m_ui->getRecordingHardwareEncoder();
                switch (settings.hardwareEncoder)
                {
                    case 2: settings.hwPreset = m_app.m_ui->getRecordingNvencPreset(); break;
                    case 3: settings.hwPreset = m_app.m_ui->getRecordingVaapiRcMode(); break;
                    case 4: settings.hwPreset = m_app.m_ui->getRecordingQsvPreset();   break;
                    case 5: settings.hwPreset = m_app.m_ui->getRecordingAmfQuality();  break;
                    default: settings.hwPreset.clear(); break;
                }

                if (!m_app.m_recordingManager) {
                    LOG_ERROR("Application: RecordingManager not initialized. Cannot start recording.");
                    m_app.m_ui->setRecordingActive(false);
                } else {
                    // Snapshot the shader/source state at click time so
                    // the recording's embedded metadata reflects exactly
                    // what was active when the session started (#59).
                    m_app.populateRecordingContext();
                    if (m_app.m_recordingManager->startRecording(settings)) {
                        LOG_INFO("Application: Recording started successfully");
                        m_app.m_ui->setRecordingActive(true);
                    } else {
                        LOG_ERROR("Application: Failed to start recording. Check logs for details.");
                        m_app.m_ui->setRecordingActive(false);
                    }
                }
            }
        } else {
            if (m_app.m_recordingManager) {
                m_app.m_recordingManager->stopRecording();
                m_app.m_ui->setRecordingActive(false);
            }
        } });

    m_app.m_ui->setOnRecordingWidthChanged([this](uint32_t width)
                                     { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.width = width;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingHeightChanged([this](uint32_t height)
                                      { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.height = height;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingFpsChanged([this](uint32_t fps)
                                   { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.fps = fps;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingBitrateChanged([this](uint32_t bitrate)
                                       { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.bitrate = bitrate;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingAudioBitrateChanged([this](uint32_t bitrate)
                                            { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.audioBitrate = bitrate;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingVideoCodecChanged([this](const std::string &codec)
                                          { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.codec = codec;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingAudioCodecChanged([this](const std::string &codec)
                                          { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.audioCodec = codec;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingH264PresetChanged([this](const std::string &preset)
                                          { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.preset = preset;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingH265PresetChanged([this](const std::string &preset)
                                          { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.preset = preset;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingH265ProfileChanged([this](const std::string &profile)
                                           { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.h265Profile = profile;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingH265LevelChanged([this](const std::string &level)
                                         { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.h265Level = level;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingVP8SpeedChanged([this](int speed)
                                        { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.vp8Speed = speed;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingVP9SpeedChanged([this](int speed)
                                        { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.vp9Speed = speed;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingContainerChanged([this](const std::string &container)
                                         { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.container = container;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingOutputPathChanged([this](const std::string &path)
                                          { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.outputPath = path;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingFilenameTemplateChanged([this](const std::string &template_)
                                                { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.filenameTemplate = template_;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

    m_app.m_ui->setOnRecordingIncludeAudioChanged([this](bool include)
                                            { 
        if (m_app.m_recordingManager) {
            RecordingSettings settings = m_app.m_recordingManager->getRecordingSettings();
            settings.includeAudio = include;
            m_app.m_recordingManager->setRecordingSettings(settings);
        } });

}

void UICallbackWiring::wireWebPortalCallbacks()
{
    // Web Portal callbacks
    m_app.m_ui->setOnWebPortalEnabledChanged([this](bool enabled)
                                       {
        m_app.m_webPortalEnabled = enabled;
        // If Web Portal is disabled, also disable HTTPS
        if (!enabled && m_app.m_webPortalHTTPSEnabled) {
            m_app.m_webPortalHTTPSEnabled = false;
            // Update UI to reflect the change
            if (m_app.m_ui) {
                m_app.m_ui->setWebPortalHTTPSEnabled(false);
            }
        }
        // Update in real-time if streaming is active (without restarting)
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->setWebPortalEnabled(enabled);
        } });

    m_app.m_ui->setOnWebPortalHTTPSChanged([this](bool enabled)
                                     {
        m_app.m_webPortalHTTPSEnabled = enabled;
        // If streaming is active, restart to apply HTTPS
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnWebPortalSSLCertPathChanged([this](const std::string &path)
                                           {
        m_app.m_webPortalSSLCertPath = path;
        // If streaming is active, restart to apply new certificate
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnWebPortalSSLKeyPathChanged([this](const std::string &path)
                                          {
        m_app.m_webPortalSSLKeyPath = path;
        // If streaming is active, restart to apply new key
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->stop();
            m_app.m_streamManager->cleanup();
            m_app.m_streamManager.reset();
            m_app.initStreaming();
        } });

    m_app.m_ui->setOnWebPortalTitleChanged([this](const std::string &title)
                                     {
        m_app.m_webPortalTitle = title;
        // Update in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->setWebPortalTitle(title);
        } });

    m_app.m_ui->setOnWebPortalSubtitleChanged([this](const std::string &subtitle)
                                        {
        m_app.m_webPortalSubtitle = subtitle;
        // Update in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->setWebPortalSubtitle(subtitle);
        } });

    m_app.m_ui->setOnWebPortalImagePathChanged([this](const std::string &path)
                                         {
        m_app.m_webPortalImagePath = path;
        // Update in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->setWebPortalImagePath(path);
        } });

    m_app.m_ui->setOnWebPortalBackgroundImagePathChanged([this](const std::string &path)
                                                   {
        m_app.m_webPortalBackgroundImagePath = path;
        // Update in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager) {
            m_app.m_streamManager->setWebPortalBackgroundImagePath(path);
        } });

    m_app.m_ui->setOnWebPortalColorsChanged([this]()
                                      {
        // Update colors in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager && m_app.m_ui) {
            // Synchronize colors from UI to Application (with safety check)
            const float* bg = m_app.m_ui->getWebPortalColorBackground();
            if (bg) {
                memcpy(m_app.m_webPortalColorBackground, bg, 4 * sizeof(float));
            }
            const float* txt = m_app.m_ui->getWebPortalColorText();
            if (txt) {
                memcpy(m_app.m_webPortalColorText, txt, 4 * sizeof(float));
            }
            const float* prim = m_app.m_ui->getWebPortalColorPrimary();
            if (prim) {
                memcpy(m_app.m_webPortalColorPrimary, prim, 4 * sizeof(float));
            }
            const float* primLight = m_app.m_ui->getWebPortalColorPrimaryLight();
            if (primLight) {
                memcpy(m_app.m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
            }
            const float* primDark = m_app.m_ui->getWebPortalColorPrimaryDark();
            if (primDark) {
                memcpy(m_app.m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
            }
            const float* sec = m_app.m_ui->getWebPortalColorSecondary();
            if (sec) {
                memcpy(m_app.m_webPortalColorSecondary, sec, 4 * sizeof(float));
            }
            const float* secHighlight = m_app.m_ui->getWebPortalColorSecondaryHighlight();
            if (secHighlight) {
                memcpy(m_app.m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
            }
            const float* ch = m_app.m_ui->getWebPortalColorCardHeader();
            if (ch) {
                memcpy(m_app.m_webPortalColorCardHeader, ch, 4 * sizeof(float));
            }
            const float* bord = m_app.m_ui->getWebPortalColorBorder();
            if (bord) {
                memcpy(m_app.m_webPortalColorBorder, bord, 4 * sizeof(float));
            }
            const float* succ = m_app.m_ui->getWebPortalColorSuccess();
            if (succ) {
                memcpy(m_app.m_webPortalColorSuccess, succ, 4 * sizeof(float));
            }
            const float* warn = m_app.m_ui->getWebPortalColorWarning();
            if (warn) {
                memcpy(m_app.m_webPortalColorWarning, warn, 4 * sizeof(float));
            }
            const float* dang = m_app.m_ui->getWebPortalColorDanger();
            if (dang) {
                memcpy(m_app.m_webPortalColorDanger, dang, 4 * sizeof(float));
            }
            const float* inf = m_app.m_ui->getWebPortalColorInfo();
            if (inf) {
                memcpy(m_app.m_webPortalColorInfo, inf, 4 * sizeof(float));
            }
            
            // Update in StreamManager
            m_app.m_streamManager->setWebPortalColors(
                m_app.m_webPortalColorBackground, m_app.m_webPortalColorText, m_app.m_webPortalColorPrimary,
                m_app.m_webPortalColorPrimaryLight, m_app.m_webPortalColorPrimaryDark,
                m_app.m_webPortalColorSecondary, m_app.m_webPortalColorSecondaryHighlight,
                m_app.m_webPortalColorCardHeader, m_app.m_webPortalColorBorder,
                m_app.m_webPortalColorSuccess, m_app.m_webPortalColorWarning, m_app.m_webPortalColorDanger, m_app.m_webPortalColorInfo);
        } });

    m_app.m_ui->setOnWebPortalTextsChanged([this]()
                                     {
        // Update texts in real-time if streaming is active
        if (m_app.m_streamingEnabled && m_app.m_streamManager && m_app.m_ui) {
            // Synchronize texts from UI to Application
            m_app.m_webPortalTextStreamInfo = m_app.m_ui->getWebPortalTextStreamInfo();
            m_app.m_webPortalTextQuickActions = m_app.m_ui->getWebPortalTextQuickActions();
            m_app.m_webPortalTextCompatibility = m_app.m_ui->getWebPortalTextCompatibility();
            m_app.m_webPortalTextStatus = m_app.m_ui->getWebPortalTextStatus();
            m_app.m_webPortalTextCodec = m_app.m_ui->getWebPortalTextCodec();
            m_app.m_webPortalTextResolution = m_app.m_ui->getWebPortalTextResolution();
            m_app.m_webPortalTextStreamUrl = m_app.m_ui->getWebPortalTextStreamUrl();
            m_app.m_webPortalTextCopyUrl = m_app.m_ui->getWebPortalTextCopyUrl();
            m_app.m_webPortalTextOpenNewTab = m_app.m_ui->getWebPortalTextOpenNewTab();
            m_app.m_webPortalTextSupported = m_app.m_ui->getWebPortalTextSupported();
            m_app.m_webPortalTextFormat = m_app.m_ui->getWebPortalTextFormat();
            m_app.m_webPortalTextCodecInfo = m_app.m_ui->getWebPortalTextCodecInfo();
            m_app.m_webPortalTextSupportedBrowsers = m_app.m_ui->getWebPortalTextSupportedBrowsers();
            m_app.m_webPortalTextFormatInfo = m_app.m_ui->getWebPortalTextFormatInfo();
            m_app.m_webPortalTextCodecInfoValue = m_app.m_ui->getWebPortalTextCodecInfoValue();
            m_app.m_webPortalTextConnecting = m_app.m_ui->getWebPortalTextConnecting();
            
            // Update in StreamManager
            m_app.m_streamManager->setWebPortalTexts(
                m_app.m_webPortalTextStreamInfo, m_app.m_webPortalTextQuickActions, m_app.m_webPortalTextCompatibility,
                m_app.m_webPortalTextStatus, m_app.m_webPortalTextCodec, m_app.m_webPortalTextResolution,
                m_app.m_webPortalTextStreamUrl, m_app.m_webPortalTextCopyUrl, m_app.m_webPortalTextOpenNewTab,
                m_app.m_webPortalTextSupported, m_app.m_webPortalTextFormat, m_app.m_webPortalTextCodecInfo,
                m_app.m_webPortalTextSupportedBrowsers, m_app.m_webPortalTextFormatInfo, m_app.m_webPortalTextCodecInfoValue,
                m_app.m_webPortalTextConnecting);
        } });

    // Web Portal Start/Stop callback (independent from streaming)
    m_app.m_ui->setOnWebPortalStartStop([this](bool start)
                                  {
        LOG_INFO("[CALLBACK] Web Portal " + std::string(start ? "START" : "STOP") + " - creating thread...");
        
        if (start) {
            // Create separate thread to start portal
            std::thread([this]() {
                try {
                    if (!m_app.initWebPortal()) {
                        LOG_ERROR("Failed to start web portal");
                        if (m_app.m_ui) {
                            m_app.m_ui->setWebPortalActive(false);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception starting web portal: " + std::string(e.what()));
                    if (m_app.m_ui) {
                        m_app.m_ui->setWebPortalActive(false);
                    }
                }
            }).detach();
        } else {
            // Stop portal in separate thread
            std::thread([this]() {
                try {
                    m_app.stopWebPortal();
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception stopping web portal: " + std::string(e.what()));
                }
                
                // Update UI after stopping
                if (m_app.m_ui) {
                    m_app.m_ui->setWebPortalActive(false);
                }
            }).detach();
        }
        
        LOG_INFO("[CALLBACK] Thread criada, retornando (thread principal continua)"); });

}

void UICallbackWiring::wireSourceAndMiscCallbacks()
{
    // Callback for source type change
    m_app.m_ui->setOnSourceTypeChanged([this](UIManager::SourceType sourceType)
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
                                     if (m_app.m_window)
                                     {
                                         m_app.m_window->setVsync(sourceType == UIManager::SourceType::Remote);
                                     }

                                     // #97/#107 — the concrete backend differs per source
                                     // type (factory V4L2/DS/AVF, VideoCaptureScreen,
                                     // VideoCaptureRemote). Reusing the old instance across a
                                     // switch opened a screen target as a V4L2 device
                                     // (segfault) and carried the previous source's device
                                     // path into the new one. When the type no longer matches
                                     // the live backend, tear it down and build the right one,
                                     // dropping the stale device so the new source starts
                                     // clean.
                                     {
                                         using ST = UIManager::SourceType;
                                         const bool wantScreen = (sourceType == ST::Screen);
                                         const bool wantRemote = (sourceType == ST::Remote);
                                         const bool wantTest   = (sourceType == ST::Test); // #149 smoke-test source
                                         const bool wantFactory = !wantScreen && !wantRemote && !wantTest; // V4L2/DS/AVF/None

                                         const bool haveScreen  = dynamic_cast<VideoCaptureScreen *>(m_app.m_capture.get()) != nullptr;
                                         const bool haveRemote  = dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()) != nullptr;
                                         const bool haveTest    = dynamic_cast<VideoCaptureTestPattern *>(m_app.m_capture.get()) != nullptr;
                                         const bool haveFactory = m_app.m_capture && !haveScreen && !haveRemote && !haveTest;

                                         const bool wrongType =
                                             !m_app.m_capture ||
                                             (wantScreen && !haveScreen) ||
                                             (wantRemote && !haveRemote) ||
                                             (wantTest && !haveTest) ||
                                             (wantFactory && !haveFactory);

                                         if (wrongType)
                                         {
                                             LOG_INFO("Source type changed — rebuilding capture backend");
                                             // Drop the UI's raw capture pointer BEFORE the old
                                             // instance is destroyed. setCaptureControls() stashes
                                             // it in UIManager::m_capture (what getCapture() hands
                                             // the Source tab); leaving it dangling across the
                                             // rebuild segfaulted on the next render of the new
                                             // tab. Cleared here, re-pointed at the new backend
                                             // below.
                                             if (m_app.m_ui) m_app.m_ui->setCaptureControls(nullptr);
                                             if (m_app.m_remoteMetaSync) { m_app.m_remoteMetaSync->stop(); m_app.m_remoteMetaSync.reset(); }
                                             if (m_app.m_capture) { m_app.m_capture->stopCapture(); m_app.m_capture->close(); m_app.m_capture.reset(); }
                                             m_app.m_devicePath.clear();
                                             if (m_app.m_ui) m_app.m_ui->setCurrentDeviceSilent("");

                                             if (wantScreen)      m_app.m_capture = std::make_unique<VideoCaptureScreen>();
                                             else if (wantRemote) m_app.m_capture = std::make_unique<VideoCaptureRemote>();
                                             else if (wantTest)   m_app.m_capture = std::make_unique<VideoCaptureTestPattern>();
                                             else                 m_app.m_capture = VideoCaptureFactory::create();

                                             // Factory backend with no device yet → idle dummy
                                             // so there's always valid output (black) until the
                                             // user picks a device, instead of a half-init grab.
                                             if (wantFactory && m_app.m_capture && sourceType != ST::None)
                                             {
                                                 m_app.m_capture->setDummyMode(true);
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                     m_app.m_capture->startCapture();
                                             }

                                             // Re-point the UI at the freshly built backend so
                                             // getCapture() is valid again. For Screen/Remote
                                             // this carries no hardware controls (no-op queries);
                                             // for V4L2/DS it re-enumerates the control set.
                                             if (m_app.m_ui) m_app.m_ui->setCaptureControls(m_app.m_capture.get());
                                         }
                                     }

                                     // #149 — Test: open the synthetic source and start it
                                     // immediately (no device to pick).
                                     if (sourceType == UIManager::SourceType::Test)
                                     {
                                         LOG_INFO("Test source selected — starting synthetic pattern");
                                         if (m_app.m_capture)
                                         {
                                             m_app.m_capture->open("test");
                                             m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0);
                                             m_app.m_capture->startCapture();
                                             if (m_app.m_ui)
                                                 m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                      m_app.m_captureFps, "Test Pattern");
                                         }
                                         return; // Test fully handled
                                     }

                                     // #107 — Screen: auto-open the default target so the
                                     // user sees frames immediately instead of having to
                                     // pick from the dropdown.
                                     if (sourceType == UIManager::SourceType::Screen)
                                     {
                                         LOG_INFO("Screen source selected");
                                         if (m_app.m_capture)
                                         {
                                             std::string target = m_app.m_ui ? m_app.m_ui->getCurrentDevice() : std::string();
                                             if (target.empty())
                                             {
                                                 const auto devs = m_app.m_capture->listDevices();
                                                 if (!devs.empty()) target = devs.front().id;
                                             }
                                             if (!target.empty())
                                             {
                                                 if (auto *screen = dynamic_cast<VideoCaptureScreen *>(m_app.m_capture.get()))
                                                 {
                                                     if (m_app.m_ui)
                                                         screen->setRegion(m_app.m_ui->getScreenRegionX(), m_app.m_ui->getScreenRegionY(),
                                                                           m_app.m_ui->getScreenRegionW(), m_app.m_ui->getScreenRegionH());
                                                 }
                                                 if (m_app.m_capture->open(target))
                                                 {
                                                     m_app.m_capture->startCapture();
                                                     m_app.m_devicePath = target;
                                                     if (m_app.m_ui)
                                                     {
                                                         m_app.m_ui->setCurrentDeviceSilent(target);
                                                         m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                              m_app.m_captureFps, target);
                                                     }
                                                 }
                                             }
                                         }
                                         return; // Screen fully handled
                                     }

                                     if (sourceType == UIManager::SourceType::None)
                                     {
                                         LOG_INFO("None source selected - activating dummy mode");

                                         // Close current device if any
                                         if (m_app.m_capture)
                                         {
                                             m_app.m_capture->stopCapture();
                                             m_app.m_capture->close();
                                             // Activate dummy mode
                                             m_app.m_capture->setDummyMode(true);

                                             // Configure dummy format with current resolution
                                             if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                             {
                                                 if (m_app.m_capture->startCapture())
                                                 {
                                                     LOG_INFO("Dummy mode activated: " + std::to_string(m_app.m_capture->getWidth()) + "x" +
                                                              std::to_string(m_app.m_capture->getHeight()));

                                                     // Update UI information
                                                     if (m_app.m_ui)
                                                     {
                                                         m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                              m_app.m_captureFps, "None (Dummy)");
                                                         m_app.m_ui->setCurrentDevice("");        // String vazia = None
                                                         m_app.m_ui->setCaptureControls(nullptr); // No V4L2 controls when None
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
                                         if (!m_app.m_devicePath.empty() && m_app.m_capture)
                                         {
                                             m_app.m_capture->stopCapture();
                                             m_app.m_capture->close();
                                             m_app.m_capture->setDummyMode(false);

                                             if (m_app.m_capture->open(m_app.m_devicePath))
                                             {
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                 {
                                                     m_app.m_capture->setFramerate(m_app.m_captureFps);
                                                     if (m_app.m_capture->startCapture())
                                                     {
                                                         if (m_app.m_ui)
                                                         {
                                                             m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                                  m_app.m_captureFps, m_app.m_devicePath);
                                                             m_app.m_ui->setCaptureControls(m_app.m_capture.get());
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // If failed to open device, return to dummy mode
                                                 LOG_WARN("Failed to open V4L2 device - activating dummy mode");
                                                 m_app.m_capture->setDummyMode(true);
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                 {
                                                     if (m_app.m_capture->startCapture() && m_app.m_ui)
                                                     {
                                                         m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                              m_app.m_captureFps, "None (Dummy)");
                                                         m_app.m_ui->setCaptureControls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_app.m_capture)
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
                                         std::string devicePath = m_app.m_devicePath;
                                         if (!devicePath.empty() && devicePath.find("/dev/video") == 0)
                                         {
                                             LOG_INFO("Clearing Linux device path: " + devicePath);
                                             devicePath.clear();
                                             m_app.m_devicePath.clear(); // Also update class member
                                         }

                                         // Try to get current device from UIManager if m_devicePath is empty
                                         if (devicePath.empty() && m_app.m_ui)
                                         {
                                             devicePath = m_app.m_ui->getCurrentDevice();
                                             LOG_INFO("Getting current device from UIManager: " + devicePath);
                                         }

                                         // If device is already selected, try to reopen
                                         if (!devicePath.empty() && m_app.m_capture)
                                         {
                                             m_app.m_capture->stopCapture();
                                             m_app.m_capture->close();
                                             m_app.m_capture->setDummyMode(false);

                                             if (m_app.m_capture->open(devicePath))
                                             {
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                 {
                                                     m_app.m_capture->setFramerate(m_app.m_captureFps);
                                                     if (m_app.m_capture->startCapture())
                                                     {
                                                         if (m_app.m_ui)
                                                         {
                                                             m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                                  m_app.m_captureFps, devicePath);
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // If failed to open device, return to dummy mode
                                                 LOG_WARN("Failed to open DirectShow device - activating dummy mode");
                                                 m_app.m_capture->setDummyMode(true);
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                 {
                                                     if (m_app.m_capture->startCapture() && m_app.m_ui)
                                                     {
                                                         m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                              m_app.m_captureFps, "None (Dummy)");
                                                         m_app.m_ui->setCurrentDevice(""); // Empty string = None
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_app.m_capture)
                                         {
                                             // If no device selected but DS was chosen, keep in dummy mode
                                             LOG_INFO("No DirectShow device selected - keeping dummy mode");
                                             if (!m_app.m_capture->isOpen() || !m_app.m_capture->isDummyMode())
                                             {
                                                 m_app.m_capture->stopCapture();
                                                 m_app.m_capture->close();
                                                 m_app.m_capture->setDummyMode(true);
                                                 if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                                                 {
                                                     if (m_app.m_capture->startCapture() && m_app.m_ui)
                                                     {
                                                         m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                                                              m_app.m_captureFps, "None (Dummy)");
                                                         m_app.m_ui->setCurrentDevice(""); // Empty string = None
                                                         m_app.m_ui->setCaptureControls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                     }
#endif

#ifdef __APPLE__
                                     if (sourceType == UIManager::SourceType::AVFoundation)
                                     {
                                         LOG_INFO("AVFoundation source selected");
                                         // Re-arm the UI's capture pointer (the None
                                         // branch above clears it as a side effect of
                                         // setCaptureControls(nullptr)) so the Source
                                         // tab can call listDevices() / setFormatById().
                                         if (m_app.m_ui && m_app.m_capture)
                                         {
                                             m_app.m_ui->setCaptureControls(m_app.m_capture.get());
                                         }

                                         // Auto-open the saved device (or the first
                                         // available one) instead of leaving the user
                                         // in dummy mode with the dropdown showing a
                                         // device that is not actually active. Without
                                         // this, the user has to pick another device
                                         // and come back just to make AVFoundation
                                         // actually capture.
                                         if (m_app.m_capture && m_app.m_capture->isDummyMode())
                                         {
                                             const auto devices = m_app.m_capture->listDevices();
                                             if (!devices.empty())
                                             {
                                                 const std::string saved = m_app.m_ui
                                                     ? m_app.m_ui->getAVFoundationDeviceId()
                                                     : std::string();
                                                 std::string target;
                                                 if (!saved.empty())
                                                 {
                                                     for (const auto &d : devices)
                                                     {
                                                         if (d.id == saved)
                                                         {
                                                             target = saved;
                                                             break;
                                                         }
                                                     }
                                                 }
                                                 if (target.empty())
                                                 {
                                                     target = devices.front().id;
                                                 }
                                                 LOG_INFO("Auto-opening AVFoundation device: " + target);
                                                 if (m_app.m_ui)
                                                 {
                                                     m_app.m_ui->triggerDeviceChange(target);
                                                 }

                                                 // Restore the format the user had
                                                 // selected on this device in a previous
                                                 // session, if any and if the device is
                                                 // the saved one (don't carry a format
                                                 // across devices — different cards
                                                 // expose different format IDs).
                                                 const std::string savedFormatId = m_app.m_ui
                                                     ? m_app.m_ui->getAVFoundationFormatId()
                                                     : std::string();
                                                 if (!savedFormatId.empty() && target == saved &&
                                                     m_app.m_capture)
                                                 {
                                                     LOG_INFO("Restoring AVFoundation format: " + savedFormatId);
                                                     if (!m_app.m_capture->setFormatById(savedFormatId, target))
                                                     {
                                                         LOG_WARN("Saved AVFoundation format id no longer matches a "
                                                                  "format on this device — leaving device at default");
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
                                         if (m_app.m_remoteMetaSync)
                                         {
                                             m_app.m_remoteMetaSync->stop();
                                             m_app.m_remoteMetaSync.reset();
                                         }
                                         if (m_app.m_capture)
                                         {
                                             m_app.m_capture->stopCapture();
                                             m_app.m_capture->close();
                                         }
                                         {
                                             auto remote = std::make_unique<VideoCaptureRemote>();
                                             VideoCaptureRemote::InterpolationMode imode = VideoCaptureRemote::InterpolationMode::Linear;
                                             if (m_app.m_remoteInterpolation == "nearest") imode = VideoCaptureRemote::InterpolationMode::Nearest;
                                             else if (m_app.m_remoteInterpolation == "off") imode = VideoCaptureRemote::InterpolationMode::Off;
                                             remote->setInterpolationMode(imode);
                                             remote->setAudioVolume(m_app.m_remoteAudioGain);
                                             m_app.m_capture = std::move(remote);
                                         }
                                         m_app.m_devicePath.clear();
                                         if (m_app.m_ui)
                                         {
                                             m_app.m_ui->setCaptureInfo(0, 0, 0, "Remote (not connected)");
                                             m_app.m_ui->setCurrentDevice("");
                                             m_app.m_ui->setCaptureControls(nullptr);
                                         }
                                     }
                                 });

    m_app.m_ui->setOnDeviceChanged([this](const std::string &devicePath)
                             {
        // Avoid infinite loop: if already processing "None", do nothing
        static bool processingDeviceChange = false;
        if (processingDeviceChange) {
            return;
        }
        processingDeviceChange = true;

        // #107 — Screen source: the callback carries the capture target
        // ("monitor:N" / "window:<id>" / ""). Replace the capture with a
        // VideoCaptureScreen pointed at it. Empty target == stop.
        if (m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Screen)
        {
            // Dedup: if we're already capturing this exact target, don't
            // tear the portal session down and re-create it (that popped
            // a fresh portal dialog and interrupted PipeWire format
            // negotiation, leaving the stream with no frames). The
            // source-type switch already auto-opened the target, so the
            // UI re-selecting the same one must be a no-op. #107.
            if (!devicePath.empty() && devicePath == m_app.m_devicePath &&
                dynamic_cast<VideoCaptureScreen *>(m_app.m_capture.get()) &&
                m_app.m_capture->isOpen())
            {
                processingDeviceChange = false;
                return;
            }

            LOG_INFO("Screen target change: " +
                     (devicePath.empty() ? std::string("(stop)") : devicePath));
            if (m_app.m_capture)
            {
                m_app.m_capture->stopCapture();
                m_app.m_capture->close();
                m_app.m_capture.reset();
            }
            m_app.m_devicePath = devicePath;

            if (devicePath.empty())
            {
                if (m_app.m_ui) m_app.m_ui->setCaptureInfo(0, 0, 0, "");
                processingDeviceChange = false;
                return;
            }

            auto screen = std::make_unique<VideoCaptureScreen>();
            screen->setRegion(m_app.m_ui->getScreenRegionX(), m_app.m_ui->getScreenRegionY(),
                              m_app.m_ui->getScreenRegionW(), m_app.m_ui->getScreenRegionH());
            m_app.m_capture = std::move(screen);
            if (m_app.m_capture->open(devicePath))
            {
                m_app.m_capture->startCapture();
                const uint32_t w = m_app.m_capture->getWidth();
                const uint32_t h = m_app.m_capture->getHeight();
                if (w > 0 && h > 0) { m_app.m_captureWidth = w; m_app.m_captureHeight = h; }
                if (m_app.m_ui) m_app.m_ui->setCaptureInfo(w, h, m_app.m_captureFps, devicePath);
            }
            else
            {
                LOG_WARN("VideoCaptureScreen: failed to open target " + devicePath);
                if (m_app.m_ui)
                {
                    m_app.m_ui->setCaptureInfo(0, 0, 0, "Screen (failed)");
                    m_app.m_ui->setCurrentDeviceSilent("");
                }
                m_app.m_devicePath.clear();
            }
            processingDeviceChange = false;
            return;
        }

        // Phase 5b/#47: when the active source is Remote, this callback
        // carries the remote BASE URL — not a V4L2 / DirectShow device
        // path. Spin up (or replace) the VideoCaptureRemote and
        // RemoteMetaSync to point at the new host. Empty URL == disconnect.
        if (m_app.m_ui && m_app.m_ui->getSourceType() == UIManager::SourceType::Remote)
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
            if (devicePath.empty() && m_app.m_frameProcessor)
            {
                m_app.m_frameProcessor->deleteTexture();
            }

            // Tear down any existing remote pipeline SYNCHRONOUSLY
            // before we open the next one (#92/#95). This used to run
            // on a detached worker so the main loop wouldn't stall on
            // VideoCaptureRemote's decode-thread join — but that left
            // the old /raw connection (a real "viewer" on the host)
            // alive while the new one opened, so:
            //   - #92: rapidly reconnecting / switching URLs stacked
            //     several live connections; the host counted each as a
            //     separate viewer.
            //   - #95: on Disconnect (empty URL) nothing waited for the
            //     teardown, so the socket / decode thread lingered and
            //     the client stayed "connected".
            // The teardown is actually fast: stopCapture() trips the
            // ffmpeg interrupt callback so the blocking read returns at
            // once, and RemoteMetaSync's poll loop sleeps in 100 ms
            // slices that check its stop flag. Doing it inline closes
            // the old socket (and joins both threads) before the next
            // open — exactly one live connection at any time. The
            // on-screen frame was already wiped above, so the brief
            // (~100-300 ms) join isn't visible as a freeze.
            if (m_app.m_remoteMetaSync)
            {
                m_app.m_remoteMetaSync->stop();
                m_app.m_remoteMetaSync.reset();
            }
            if (m_app.m_capture)
            {
                m_app.m_capture->stopCapture();
                m_app.m_capture->close();
                m_app.m_capture.reset();
            }

            m_app.m_devicePath = devicePath;

            if (devicePath.empty())
            {
                // Mark the UI as cleanly disconnected so the connection
                // window stops showing 'Stream: WxH' from the last
                // session.
                if (m_app.m_ui)
                {
                    m_app.m_ui->setCaptureInfo(0, 0, 0, "");
                }
                // Re-arm the Image seed for the next connection — each
                // session gets one seed from /meta, after which the user
                // owns the values locally.
                m_app.m_remoteImageSeeded = false;
                processingDeviceChange = false;
                return;
            }

            // #49 Phase 3 — pull the bearer token the connection UI
            // (or password modal) stashed for this connect. Consumed
            // exactly once: read here, cleared immediately so a
            // subsequent unprotected connect doesn't accidentally
            // inherit the previous stream's token.
            std::string authToken;
            if (m_app.m_ui)
            {
                authToken = m_app.m_ui->getRemoteAuthToken();
                m_app.m_ui->setRemoteAuthToken("");
            }

            // Recreate the capture as Remote (the existing instance might be
            // V4L2/DS if the user just flipped source type).
            {
                auto remote = std::make_unique<VideoCaptureRemote>();
                VideoCaptureRemote::InterpolationMode imode = VideoCaptureRemote::InterpolationMode::Linear;
                if (m_app.m_remoteInterpolation == "nearest") imode = VideoCaptureRemote::InterpolationMode::Nearest;
                else if (m_app.m_remoteInterpolation == "off") imode = VideoCaptureRemote::InterpolationMode::Off;
                remote->setInterpolationMode(imode);
                remote->setAudioVolume(m_app.m_remoteAudioGain);
                remote->setAuthToken(authToken);
                m_app.m_capture = std::move(remote);
            }
            if (m_app.m_capture->open(devicePath))
            {
                m_app.m_capture->startCapture();

                // Pull the actual stream dimensions from the freshly-opened
                // decoder and sync the application's idea of capture size.
                // Without this the rest of the render pipeline keeps using
                // m_captureWidth / m_captureHeight from the local config
                // (1920x1080 by default), producing the "image fills only
                // half the window" symptom when the host streams smaller.
                const uint32_t actualW = m_app.m_capture->getWidth();
                const uint32_t actualH = m_app.m_capture->getHeight();
                if (actualW > 0 && actualH > 0)
                {
                    m_app.m_captureWidth  = actualW;
                    m_app.m_captureHeight = actualH;
                    LOG_INFO("Remote stream dims: " + std::to_string(actualW) + "x" + std::to_string(actualH));
                }

                if (m_app.m_ui)
                {
                    m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(),
                                         m_app.m_captureFps, devicePath);
                }
                // Re-start the /meta poller against the new host.
                m_app.m_remoteManager->startWorker(devicePath, &authToken);
            }
            else
            {
                LOG_WARN("Failed to connect to remote host " + devicePath);
                if (m_app.m_ui)
                {
                    m_app.m_ui->setCaptureInfo(0, 0, 0, "Remote (failed)");
                    // Clear the current device so the connection window
                    // stops reporting 'connected'. Done via direct
                    // assignment because setCurrentDevice("") would
                    // re-enter this callback and we're still inside its
                    // processingDeviceChange guard.
                    m_app.m_ui->setCurrentDeviceSilent("");
                }
                m_app.m_devicePath.clear();
            }

            processingDeviceChange = false;
            return;
        }

        // If devicePath is empty, it means "None" - activate dummy mode
        if (devicePath.empty())
        {
            // Check if already in dummy mode to avoid loop
            if (m_app.m_devicePath.empty() && m_app.m_capture && m_app.m_capture->isDummyMode() && m_app.m_capture->isOpen()) {
                processingDeviceChange = false;
                return;
            }
            
            LOG_INFO("Disconnecting device (None selected) - activating dummy mode");
            
            // Close current device
            if (m_app.m_capture) {
                m_app.m_capture->stopCapture();
                m_app.m_capture->close();
                // Ativar modo dummy
                m_app.m_capture->setDummyMode(true);
                
                // Configure dummy format with current resolution
                if (m_app.m_capture->setFormat(m_app.m_captureWidth, m_app.m_captureHeight, 0))
                {
                    m_app.m_capture->startCapture();
                    LOG_INFO("Dummy mode activated: " + std::to_string(m_app.m_capture->getWidth()) + "x" +
                             std::to_string(m_app.m_capture->getHeight()));
                }
            }
            
            // Update device path
            m_app.m_devicePath = "";
            
            // Update UI information (without calling setCurrentDevice to avoid loop)
            if (m_app.m_ui) {
                if (m_app.m_capture && m_app.m_capture->isOpen()) {
                    m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(), 
                                        m_app.m_captureFps, "None (Dummy)");
                    // Don't call setCurrentDevice here to avoid loop
                } else {
                    m_app.m_ui->setCaptureInfo(0, 0, 0, "None");
                    // Don't call setCurrentDevice here to avoid loop
                }
                m_app.m_ui->setCaptureControls(nullptr); // No V4L2 controls when no device
            }
            
            LOG_INFO("Dummy mode activated. Select a device to use real capture.");
            processingDeviceChange = false;
            return;
        }
        
        LOG_INFO("Changing device to: " + devicePath);
        
        // Save current settings
        uint32_t oldWidth = m_app.m_captureWidth;
        uint32_t oldHeight = m_app.m_captureHeight;
        uint32_t oldFps = m_app.m_captureFps;
        
        // Close current device (or dummy mode)
        if (m_app.m_capture) {
            m_app.m_capture->stopCapture();
            m_app.m_capture->close();
            // Disable dummy mode when trying to open real device
            m_app.m_capture->setDummyMode(false);
        }

        // #97 — if we're coming from a Remote (or Screen) session, m_capture
        // is still that backend; calling open("/dev/videoN") on it routes the
        // device path through the wrong demuxer ("connecting to remote base
        // URL /dev/videoN"). Rebuild as the local factory capture (V4L2 /
        // DirectShow / AVFoundation) before opening the device.
        if (m_app.m_capture &&
            (dynamic_cast<VideoCaptureRemote *>(m_app.m_capture.get()) ||
             dynamic_cast<VideoCaptureScreen *>(m_app.m_capture.get())))
        {
            LOG_INFO("Device change: active capture is not a local device backend — rebuilding via factory (#97)");
            if (m_app.m_remoteMetaSync) { m_app.m_remoteMetaSync->stop(); m_app.m_remoteMetaSync.reset(); }
            m_app.m_capture = VideoCaptureFactory::create();
            if (m_app.m_ui) m_app.m_ui->setCaptureControls(m_app.m_capture.get());
        }

        // Update device path
        m_app.m_devicePath = devicePath;
        
        // Clear FrameProcessor texture when changing device
        if (m_app.m_frameProcessor) {
            m_app.m_frameProcessor->deleteTexture();
        }
        
        // Reopen with new device
        if (m_app.m_capture && m_app.m_capture->open(devicePath)) {
            LOG_INFO("Device opened successfully, configuring format...");
            // Reconfigure format and framerate
            if (m_app.m_capture->setFormat(oldWidth, oldHeight, 0)) {
                LOG_INFO("Format configured, configuring framerate...");
                m_app.m_capture->setFramerate(oldFps);
                LOG_INFO("Framerate configured, starting capture (startCapture)...");
                if (m_app.m_capture->startCapture()) {
                    LOG_INFO("startCapture() returned true - device should be active (light on)");
                } else {
                    LOG_ERROR("startCapture() returned false - device was NOT activated!");
                }
                
                // Update UI information
                if (m_app.m_ui) {
                    m_app.m_ui->setCaptureInfo(m_app.m_capture->getWidth(), m_app.m_capture->getHeight(), 
                                        m_app.m_captureFps, devicePath);
                    
                    // Reload V4L2 controls
                    m_app.m_ui->setCaptureControls(m_app.m_capture.get());
                }
                
                LOG_INFO("Device changed successfully");
                processingDeviceChange = false;
            } else {
                LOG_ERROR("Failed to configure format on new device");
                // Close device if failed
                m_app.m_capture->close();
                if (m_app.m_ui) {
                    m_app.m_ui->setCaptureInfo(0, 0, 0, "Error");
                }
                processingDeviceChange = false;
            }
        } else {
            LOG_ERROR("Failed to open new device: " + devicePath);
            processingDeviceChange = false;
            if (m_app.m_ui) {
                m_app.m_ui->setCaptureInfo(0, 0, 0, "Error");
            }
        } });

    // Configure current shader
    if (!m_app.m_presetPath.empty())
    {
        fs::path presetPath(m_app.m_presetPath);
        fs::path basePath = fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl";
        fs::path relativePath = fs::relative(presetPath, basePath);
        if (!relativePath.empty() && relativePath != presetPath)
        {
            m_app.m_ui->setCurrentShader(relativePath.string());
        }
        else
        {
            m_app.m_ui->setCurrentShader(m_app.m_presetPath);
        }

        // Callback to save preset
        m_app.m_ui->setOnSavePreset([this](const std::string &path, bool overwrite)
                              {
        if (!m_app.m_shaderEngine || !m_app.m_shaderEngine->isShaderActive()) {
            LOG_WARN("Nenhum preset carregado para salvar");
            return;
        }
        
        // Get custom parameters from ShaderEngine
        auto params = m_app.m_shaderEngine->getShaderParameters();
        std::unordered_map<std::string, float> customParams;
        for (const auto& param : params) {
            // Save all values (even if equal to default, to preserve configuration)
            customParams[param.name] = param.value;
        }
        
        // Save preset
        const ShaderPreset& preset = m_app.m_shaderEngine->getPreset();
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

}
