#include "UIManager.h"
#include "UIConfigurationSource.h"
#include "UIConfigurationShader.h"
#include "UIConfigurationImage.h"
#include "UIConfigurationStreaming.h"
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#  include "UIConfigurationVirtualCamera.h"
#endif
#include "UIConfigurationRecording.h"
#include "UIConfigurationWebPortal.h"
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#  include "UIConfigurationAudio.h"
#endif
#include "UIInfoPanel.h"
#include "UIPreferences.h"
#include "UISectionHeader.h"
#include "UIShortcutsHelp.h"
#include "../osd/QuickActionsOverlay.h"
#include "../osd/ConnectionStatusOverlay.h"
#include "../osd/OSDChat.h"
#include "UICredits.h"
#include "UIRemoteConnection.h"
#include "UIDirectoryBrowser.h"
#include "UICapturePresets.h"
#include "UIRecordings.h"
#include "../recording/RecordingProfileManager.h"
#include "../recording/RecordingSettings.h"
#include "../streaming/StreamingProfileManager.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"
#include "../utils/TranslationManager.h"
#include "../utils/ShaderScanner.h"
#ifdef PLATFORM_LINUX
#include "../utils/V4L2DeviceScanner.h"
#endif
#include "../capture/IVideoCapture.h"
#include "../shader/ShaderEngine.h"
#include "../renderer/glad_loader.h"
#include <string>
#ifdef USE_SDL2
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // #107 follow-up: clamp off-screen windows back in
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h> // #107 follow-up: clamp off-screen windows back in
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#endif
#ifdef PLATFORM_LINUX
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "../utils/FilesystemCompat.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

UIManager::UIManager()
    : m_creditsWindow(nullptr)
{
}

UIManager::~UIManager()
{
    // Destruir janelas antes de shutdown
    m_sourceWindow.reset();
    m_shaderWindow.reset();
    m_imageWindow.reset();
    m_streamingWindow.reset();
#if defined(__linux__)
    m_virtcamWindow.reset();
#endif
    m_recordingWindow.reset();
    m_webPortalWindow.reset();
#if defined(__linux__) || defined(__APPLE__)
    m_audioWindow.reset();
#endif
    m_infoWindow.reset();
    m_preferencesWindow.reset();
    m_shortcutsHelpWindow.reset();
    m_connectionOverlay.reset();
    m_quickActionsOverlay.reset();
    m_creditsWindow.reset();
    m_capturePresetsWindow.reset();
    m_recordingsWindow.reset();
    shutdown();
}

bool UIManager::init(void *window)
{
    if (m_initialized)
    {
        return true;
    }

    if (!window)
    {
        LOG_ERROR("Invalid window pointer for ImGui initialization");
        return false;
    }

    m_window = window;

#ifdef USE_SDL2
    // SDL2: window is already current context
    SDL_Window *sdlWindow = static_cast<SDL_Window *>(window);
    if (!sdlWindow)
    {
        LOG_ERROR("Invalid SDL2 window for ImGui initialization");
        return false;
    }
#else
    // GLFW: ensure context is active
    GLFWwindow *glfwWindow = static_cast<GLFWwindow *>(window);
    if (!glfwWindow)
    {
        LOG_ERROR("Invalid GLFW window for ImGui initialization");
        return false;
    }
    glfwMakeContextCurrent(glfwWindow);
#endif

    // Verify OpenGL functions are loaded before initializing ImGui
    if (!glGenVertexArrays)
    {
        LOG_ERROR("OpenGL functions not loaded. Loading now...");
        if (!loadOpenGLFunctions())
        {
            LOG_ERROR("Failed to load OpenGL functions for ImGui");
            return false;
        }
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Move windows only by dragging their title bar (standard desktop
    // window behaviour). Without this, dragging anywhere in a window's
    // body moves it — which fought the region-selector marquee and is
    // generally surprising. Applies to every RetroCapture window.
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Anchor the ImGui ini to the user-data dir so window
    // positions/sizes/collapse state survive across runs regardless
    // of which directory the binary was launched from. CWD-relative
    // worked while everyone ran from build/bin during dev but breaks
    // for installed builds and Wine launches. m_iniPath has to outlive
    // ImGui (it holds the pointer verbatim), so we keep it on the
    // UIManager instance.
    {
        // FilesystemCompat.h on MinGW < 8 (current MXE toolchain runs
        // GCC 5.5) doesn't ship the std::error_code overload of
        // create_directories — it has only the single-arg signature
        // that throws on failure. Wrap in try/catch so a missing dir
        // (typically because the parent was just created by another
        // run) doesn't take down the UI init.
        fs::path userDir = fs::path(Paths::getUserDataDir());
        try { fs::create_directories(userDir); } catch (...) { /* best effort */ }
        m_iniPath = (userDir / "imgui.ini").string();
        io.IniFilename = m_iniPath.c_str();
        LOG_INFO("ImGui ini path: " + m_iniPath);
    }
    // Migrate the legacy CWD-anchored configs out of the way once.
    for (const char *legacy : { "RetroCapture.ini", "imgui.ini" })
    {
        if (fs::exists(legacy))
        {
            fs::remove(legacy);
            LOG_INFO(std::string("Removed legacy ini at CWD: ") + legacy);
        }
    }

    // We previously tried to extend the default font's glyph range
    // to include geometric shapes (U+25A0..U+25FF) so the `●` / `○`
    // status bullets would render. That doesn't work: Dear ImGui's
    // built-in Proggy Clean has no glyph DATA for any of those
    // codepoints, so ranging them in still gets the missing-glyph
    // fallback. The bullets are now drawn via ImDrawList primitives
    // through ui_status_bullet() in UISectionHeader.h — same
    // technique UIDirectoryBrowser::drawPadlockIcon uses.
    //
    // Optional emoji font (#84) — chat messages routinely contain
    // emojis the default font can't render either. We don't bundle a
    // font (Noto Emoji's monochrome TTF is ~530 KB and adding it
    // would grow every release), but we DO honour one if the user
    // drops it under assets/fonts/. The expected file is
    // NotoEmoji-Regular.ttf (download from
    // https://fonts.google.com/noto/specimen/Noto+Emoji). When
    // present we merge a broad emoji + dingbats + supplemental
    // symbols range into the default atlas so chat content lands
    // with proper glyphs instead of fallback boxes.
    {
        const fs::path fontPath =
            fs::path(Paths::getReadOnlyAssetsDir()) / "assets" / "fonts" /
            "NotoEmoji-Regular.ttf";
        if (fs::exists(fontPath))
        {
            ImGuiIO &io = ImGui::GetIO();
            io.Fonts->AddFontDefault();
            ImFontConfig cfg;
            cfg.MergeMode  = true;
            cfg.PixelSnapH = true;
            // ImGui's atlas only supports the Basic Multilingual Plane
            // (U+0000..U+FFFF) plus surrogate-paired codepoints up to
            // U+10FFFF that ImWchar32 can address; the runtime build
            // here uses ImWchar = 16-bit by default. Cover the BMP
            // symbol ranges that don't need surrogates so the merge
            // takes effect even on a stock build.
            static const ImWchar ranges[] = {
                0x2000, 0x206F,   // general punctuation
                0x2190, 0x21FF,   // arrows
                0x2300, 0x23FF,   // misc technical (⏎ etc.)
                0x2500, 0x257F,   // box drawing
                0x2580, 0x259F,   // block elements
                0x25A0, 0x25FF,   // geometric shapes (●, ○, …)
                0x2600, 0x26FF,   // misc symbols (☀, ★, ♥…)
                0x2700, 0x27BF,   // dingbats (✓, ✦…)
                0x2B00, 0x2BFF,   // misc symbols + arrows
                0x3000, 0x303F,   // CJK punctuation
                0xFB00, 0xFB06,   // Latin ligatures
                0,
            };
            io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(),
                                         13.0f, &cfg, ranges);
            LOG_INFO("UIManager: merged emoji font from " + fontPath.string());
        }
    }

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    // Usar versão GLSL dinâmica baseada na versão OpenGL disponível
    std::string glslVersion = getGLSLVersionString();
#ifdef USE_SDL2
    ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window *>(window), nullptr);
    ImGui_ImplOpenGL3_Init(glslVersion.c_str());
#else
    ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow *>(window), true);
    ImGui_ImplOpenGL3_Init(glslVersion.c_str());
#endif

    // Scan for shaders. Env override `RETROCAPTURE_SHADER_PATH` ainda é
    // honrado (AppImage / dev). Caso contrário, usa o assets dir resolvido
    // (CWD em dev tree, install system-wide ou portable).
    const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
    if (envShaderPath && fs::exists(envShaderPath))
    {
        m_shaderBasePath = envShaderPath;
    }
    else
    {
        m_shaderBasePath = (fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl").string();
    }
    scanShaders(m_shaderBasePath);

    loadConfig();

    // i18n bootstrap (#45 Fase B). loadConfig() above already set
    // m_language from config.json (default "en"); now hand it to
    // the TranslationManager so T(...) calls in subsequent UI
    // construction return translated strings from the start. Switch
    // via UIPreferences calls setLanguage() at runtime.
    // Paths::getReadOnlyAssetsDir() returns the install/dev root that
    // contains `assets/`, `shaders/`, `web/` side by side — not the
    // `assets/` directory itself. The i18n bundles live under
    // `assets/i18n/`, so explicitly join the `assets` segment here.
    TranslationManager::instance().init(
        (fs::path(Paths::getReadOnlyAssetsDir()) / "assets").string(),
        m_language);

    // Standalone configuration windows (Fase A of #45) — each used to
    // be a tab inside the unified "RetroCapture Controls" window;
    // they're now separate ImGui windows opened from
    // Configurations / View / File menus.
    m_sourceWindow      = std::make_unique<UIConfigurationSource>(this);
    m_shaderWindow      = std::make_unique<UIConfigurationShader>(this);
    m_imageWindow       = std::make_unique<UIConfigurationImage>(this);
    m_streamingWindow   = std::make_unique<UIConfigurationStreaming>(this);
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    m_virtcamWindow     = std::make_unique<UIConfigurationVirtualCamera>(this);
#endif
    m_recordingWindow   = std::make_unique<UIConfigurationRecording>(this);
    m_webPortalWindow   = std::make_unique<UIConfigurationWebPortal>(this);
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    m_audioWindow       = std::make_unique<UIConfigurationAudio>(this);
#endif
    m_infoWindow        = std::make_unique<UIInfoPanel>(this);
    m_preferencesWindow = std::make_unique<UIPreferences>(this);
    // OSD layer (#68). Lives in src/osd/ — visual elements pinned to
    // viewport corners with no user-movable decoration, distinct from
    // the UI layer (src/ui/) which owns interactive windows.
    m_quickActionsOverlay = std::make_unique<QuickActionsOverlay>(this);
    m_quickActionsOverlay->setVisible(m_quickActionsVisible);
    m_connectionOverlay   = std::make_unique<ConnectionStatusOverlay>(
        this, m_quickActionsOverlay.get());
    // Chat overlay (#84) — needs the ChatClient pointer Application
    // installs via setChatClient(). Always constructed; the overlay
    // self-hides while the client is Idle, so a null/unconfigured
    // pointer renders as nothing.
    m_chatOverlay = std::make_unique<OSDChat>(this, m_chatClient);
    m_chatOverlay->setVisible(m_chatOverlayVisible);
    // Shortcuts orientation widget — UI layer, top-right corner,
    // F12-gated unlike the OSD.
    m_shortcutsHelpWindow = std::make_unique<UIShortcutsHelp>(this);
    m_shortcutsHelpWindow->setVisible(m_shortcutsHelpVisible);

    m_creditsWindow = std::make_unique<UICredits>(this);
    m_capturePresetsWindow = std::make_unique<UICapturePresets>(this);
    m_recordingsWindow = std::make_unique<UIRecordings>(this);
    m_remoteConnectionWindow = std::make_unique<UIRemoteConnection>(this);
    m_directoryBrowserWindow = std::make_unique<UIDirectoryBrowser>(this);
    m_directoryBrowserWindow->setRemoteConnectionWindow(m_remoteConnectionWindow.get());
    m_recordingProfileManager = std::make_unique<RecordingProfileManager>();
    m_streamingProfileManager = std::make_unique<StreamingProfileManager>();

    // Open the most useful window on first launch so the user sees
    // something rather than a bare menu bar. Same role the old
    // "RetroCapture Controls" tab window had.
    if (m_sourceWindow) m_sourceWindow->setVisible(true);

    m_initialized = true;
    LOG_INFO("UIManager initialized");
    return true;
}

void UIManager::setChatClient(ChatClient *chat)
{
    m_chatClient = chat;
    if (m_chatOverlay)
    {
        // The overlay was constructed before init() finished if
        // setChatClient ran first; reconstruct it so it picks up the
        // pointer. Cheaper to swap than to expose another setter that
        // most callers wouldn't touch.
        m_chatOverlay = std::make_unique<OSDChat>(this, m_chatClient);
        m_chatOverlay->setVisible(m_chatOverlayVisible);
    }
}

void UIManager::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::string oldIniPath = "imgui.ini";
    if (fs::exists(oldIniPath))
    {
        fs::remove(oldIniPath);
        LOG_INFO("Old configuration file removed during shutdown: " + oldIniPath);
    }

    // #132 — flush window positions/sizes now. ImGui only auto-saves the
    // ini on a ~5 s timer during the frame loop (io.IniSavingRate), and
    // DestroyContext does NOT save. So a window moved shortly before
    // quitting — or a quit via the tray / window-close path on Windows —
    // lost its layout. Force a save here while the context is still valid.
    // The LOG also surfaces the exact path, which helps if the user-data
    // dir turns out not to be writable on a given platform.
    if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
    {
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        LOG_INFO(std::string("Saved ImGui layout to ") + ImGui::GetIO().IniFilename);
    }

    ImGui_ImplOpenGL3_Shutdown();
#ifdef USE_SDL2
    ImGui_ImplSDL2_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
    ImGui::DestroyContext();

    m_initialized = false;
}

// Keep every movable top-level window reachable: if its position drifted
// outside the viewport (stale imgui.ini after a resolution/monitor change,
// etc.) nudge it back so at least a grabbable strip of the title bar stays
// on screen. Runs each frame on last frame's positions and applies before
// the windows are submitted this frame, so they never paint off-screen.
static void clampImGuiWindowsToViewport()
{
    ImGuiContext *gp = ImGui::GetCurrentContext();
    if (!gp) return;
    ImGuiContext &g = *gp;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    if (!vp) return;
    const float minX0 = vp->WorkPos.x;
    const float minY0 = vp->WorkPos.y;
    const float maxX0 = vp->WorkPos.x + vp->WorkSize.x;
    const float maxY0 = vp->WorkPos.y + vp->WorkSize.y;
    const float keep  = 48.0f; // px of the window that must remain on-screen

    for (ImGuiWindow *w : g.Windows)
    {
        if (!w || !w->WasActive) continue;
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip |
                        ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoMove))
            continue; // children/popups/pinned OSD overlays manage themselves
        if (w->Size.x <= 0.0f || w->Size.y <= 0.0f) continue;

        ImVec2 pos = w->Pos;
        const float minX = minX0 - w->Size.x + keep; // allow off the left/right
        const float maxX = maxX0 - keep;             // edges, keep `keep` visible
        const float minY = minY0;                    // title must stay below top
        const float maxY = maxY0 - keep;
        if (pos.x < minX) pos.x = minX;
        if (pos.x > maxX) pos.x = maxX;
        if (pos.y < minY) pos.y = minY;
        if (pos.y > maxY) pos.y = maxY;

        if (pos.x != w->Pos.x || pos.y != w->Pos.y)
            ImGui::SetWindowPos(w, pos, ImGuiCond_Always);
    }
}

void UIManager::beginFrame()
{
    if (!m_initialized)
    {
        return;
    }

    // Mouse-input gating. NoMouse blocks ImGui from interpreting any
    // pointer activity, used to keep the cursor / clicks from leaking
    // into a hidden UI. But the OSD layer (#68) lives outside the
    // m_uiVisible gate — its buttons must respond to clicks even
    // with F12 hiding the rest of the UI. So gate NoMouse on
    // (UI hidden AND no interactive OSD on screen) rather than on
    // UI alone, and skip the mouse-position scrub that would prevent
    // any widget from seeing hover at all.
    const bool osdInteractive = (m_quickActionsOverlay && m_quickActionsOverlay->isVisible()) ||
                                (m_chatOverlay         && m_chatOverlay->isVisible());
    const bool blockMouse     = !m_uiVisible && !osdInteractive;
    ImGuiIO& io = ImGui::GetIO();
    if (blockMouse)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.MouseDown[0] = false;
        io.MouseDown[1] = false;
        io.MouseDown[2] = false;
        io.MouseDown[3] = false;
        io.MouseDown[4] = false;
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }

    // Always call NewFrame, even when UI is hidden (maintains ImGui state)
    ImGui_ImplOpenGL3_NewFrame();
#ifdef USE_SDL2
    ImGui_ImplSDL2_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();

    // ImGui backends (Glfw/SDL2 NewFrame) may flip the mouse flag and
    // pull a fresh cursor position from the OS — re-apply our gating
    // and scrub the position only when truly blocking input.
    if (blockMouse)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MousePosPrev = ImVec2(-FLT_MAX, -FLT_MAX);
    }

    // Pull any window that drifted off-screen back into the viewport so it
    // can't get lost at a position that no longer exists on screen.
    clampImGuiWindowsToViewport();
}

void UIManager::endFrame()
{
    if (!m_initialized)
    {
        return;
    }

    // Always paint the frame, even when the main UI is hidden.
    // renderConnectionOverlay() runs before render()'s m_uiVisible
    // gate, so when F12 hides the rest of the UI the draw list still
    // has the overlay in it. Calling EndFrame instead of Render
    // discarded that, leaving the user with a frozen-looking stream
    // during reconnects. With nothing added to the draw list this is
    // effectively a no-op anyway.
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    // Mouse input gating. NoMouse blocks ImGui from interpreting
    // clicks, which keeps the cursor from being captured by hidden
    // windows. But the OSD layer (#68) lives outside the m_uiVisible
    // gate — when the quick-actions overlay is on screen, its
    // buttons must respond to clicks even with the rest of the UI
    // hidden via F12. So gate NoMouse on (UI hidden AND no
    // interactive OSD visible) rather than UI alone.
    const bool osdInteractive = (m_quickActionsOverlay && m_quickActionsOverlay->isVisible()) ||
                                (m_chatOverlay         && m_chatOverlay->isVisible());
    ImGuiIO& io = ImGui::GetIO();
    if (!m_uiVisible && !osdInteractive)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
}

void UIManager::setVisible(bool visible)
{
    if (m_uiVisible != visible)
    {
        m_uiVisible = visible;
        if (m_onVisibilityChanged)
        {
            m_onVisibilityChanged(visible);
        }
    }
}

void UIManager::render()
{
    if (!m_initialized) return;

    // OSD layer renders BEFORE the m_uiVisible gate (#68). By
    // definition an OSD shows up regardless of whether the rest of
    // the UI is hidden — the user pressing F12 to clear the screen
    // shouldn't make the quick-actions widget or the connection
    // status indicator vanish too. Quick actions renders first so
    // its rendered-height is up-to-date when the connection overlay
    // queries it for anti-collision math.
    if (m_quickActionsOverlay) m_quickActionsOverlay->render();
    renderConnectionOverlay();
    // Chat overlay (#84) — also outside the m_uiVisible gate so the
    // user can keep an eye on chat while everything else is hidden.
    if (m_chatOverlay) m_chatOverlay->render();
    // Directory browser (#84) — promoted from the regular UI layer
    // to the OSD layer so it stays reachable when F12 hides the
    // config windows. Same one-button entry from QuickActions
    // continues to work; the only behavioural change is "outside
    // m_uiVisible". p_open binding inside UIDirectoryBrowser::render
    // gives it the standard title-bar X.
    if (m_directoryBrowserWindow)
    {
        // Per-frame invariant: a host who's actively streaming
        // shouldn't have the browser open — picking another stream
        // would fight the capture pipeline, and the QuickActions
        // button that opens it is already disabled in that state.
        // Forcing it closed every frame is cheap and avoids the
        // "I opened it before clicking Start, now it's stuck" case.
        if (getStreamingActive() && !isRemoteSource() &&
            m_directoryBrowserWindow->isVisible())
        {
            m_directoryBrowserWindow->setVisible(false);
        }
        m_directoryBrowserWindow->render();
    }

    if (!m_uiVisible)
    {
        return;
    }

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu(T("menu.file").c_str()))
        {
            if (m_preferencesWindow)
            {
                bool visible = m_preferencesWindow->isVisible();
                if (ImGui::MenuItem(T("menu.file.preferences").c_str(), nullptr, visible))
                {
                    m_preferencesWindow->setVisible(!visible);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("menu.file.exit").c_str(), "Esc"))
            {
                if (m_window)
                {
#ifdef USE_SDL2
                    // SDL2: Window close is handled via SDL_QUIT event in pollEvents
                    // Can't directly close window from here, but can set flag
#else
                    glfwSetWindowShouldClose(static_cast<GLFWwindow *>(m_window), GLFW_TRUE);
#endif
                }
            }
            ImGui::EndMenu();
        }

        // Configurations menu — each entry toggles one of the
        // ex-tab windows. Producer-only windows hide entirely in
        // client mode so the user can't accidentally try to
        // configure things that don't apply to the inbound stream.
        const bool clientMode = (m_sourceType == SourceType::Remote) && !m_currentDevice.empty();
        if (ImGui::BeginMenu(T("menu.configurations").c_str()))
        {
            auto toggleItem = [](const std::string &label, auto *window) {
                if (!window) return;
                bool vis = window->isVisible();
                if (ImGui::MenuItem(label.c_str(), nullptr, vis)) window->setVisible(!vis);
            };
            // Shader + Image stay visible in client mode — shader
            // params and brightness/contrast/aspect ratio make
            // sense for a viewer to override.
            toggleItem(T("menu.configurations.shaders"),  m_shaderWindow.get());
            toggleItem(T("menu.configurations.image"),    m_imageWindow.get());
            // Recording is allowed in client mode (#68) — the
            // pipeline is generic and captures whatever the
            // framebuffer holds, which in client mode is the
            // decoded remote /raw. Audio has a caveat (no local
            // loopback by default) that the OSD tooltip surfaces.
            toggleItem(T("menu.configurations.recording"),  m_recordingWindow.get());
            if (!clientMode)
            {
                ImGui::Separator();
                toggleItem(T("menu.configurations.source"),     m_sourceWindow.get());
                toggleItem(T("menu.configurations.streaming"),  m_streamingWindow.get());
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                toggleItem(T("menu.configurations.virtcam"),    m_virtcamWindow.get());
#endif
                toggleItem(T("menu.configurations.webportal"),  m_webPortalWindow.get());
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
                toggleItem(T("menu.configurations.audio"),      m_audioWindow.get());
#endif
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(T("menu.view").c_str()))
        {
            if (ImGui::MenuItem(T("menu.view.toggleui").c_str(), "F12"))
            {
                setVisible(!m_uiVisible);
            }
            ImGui::Separator();
            if (m_quickActionsOverlay)
            {
                bool visible = m_quickActionsOverlay->isVisible();
                if (ImGui::MenuItem(T("menu.view.quickactions").c_str(), nullptr, visible))
                {
                    m_quickActionsOverlay->setVisible(!visible);
                }
            }
            // Profile / Chat / Chat Rooms moved to the top-level
            // "Social" menu — they're a coherent group that's not
            // about toggling local widgets, so the View menu was
            // the wrong drawer for them.
            if (m_shortcutsHelpWindow)
            {
                bool visible = m_shortcutsHelpWindow->isVisible();
                if (ImGui::MenuItem(T("menu.view.shortcuts").c_str(), nullptr, visible))
                {
                    m_shortcutsHelpWindow->setVisible(!visible);
                }
            }
            if (m_infoWindow)
            {
                bool visible = m_infoWindow->isVisible();
                if (ImGui::MenuItem(T("menu.view.info").c_str(), nullptr, visible))
                {
                    m_infoWindow->setVisible(!visible);
                }
            }
            if (!clientMode)
            {
                if (m_capturePresetsWindow)
                {
                    bool visible = m_capturePresetsWindow->isVisible();
                    if (ImGui::MenuItem(T("menu.view.capturepresets").c_str(), nullptr, visible))
                    {
                        m_capturePresetsWindow->setVisible(!visible);
                    }
                }
                if (m_recordingsWindow)
                {
                    bool visible = m_recordingsWindow->isVisible();
                    if (ImGui::MenuItem(T("menu.view.recordings").c_str(), nullptr, visible))
                    {
                        m_recordingsWindow->setVisible(!visible);
                    }
                }
            }
            ImGui::EndMenu();
        }

        // Social — Profile / Chat / Chat Rooms. Conceptually about
        // identity + communication with other users; the View menu
        // was being used as a catch-all and these items got buried.
        if (m_chatOverlay && ImGui::BeginMenu(T("menu.social").c_str()))
        {
            bool profVis = m_chatOverlay->isProfileWindowVisible();
            if (ImGui::MenuItem(T("menu.social.profile").c_str(),
                                nullptr, profVis))
            {
                m_chatOverlay->setProfileWindowVisible(!profVis);
            }
            ImGui::Separator();
            bool chatVis = m_chatOverlay->isVisible();
            if (ImGui::MenuItem(T("menu.social.chat").c_str(),
                                "F8", chatVis))
            {
                m_chatOverlay->setVisible(!chatVis);
            }
            bool roomsVis = m_chatOverlay->isRoomsWindowVisible();
            if (ImGui::MenuItem(T("menu.social.chatrooms").c_str(),
                                nullptr, roomsVis))
            {
                m_chatOverlay->setRoomsWindowVisible(!roomsVis);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(T("menu.remote").c_str()))
        {
            const bool clientEntryAllowed = !m_streamingActive;
            if (ImGui::MenuItem(T("menu.remote.connect").c_str(), nullptr, false, clientEntryAllowed))
            {
                if (m_remoteConnectionWindow)
                {
                    m_remoteConnectionWindow->setVisible(true);
                }
            }
            if (ImGui::MenuItem(T("menu.remote.browse").c_str(), nullptr, false, clientEntryAllowed))
            {
                if (m_directoryBrowserWindow)
                {
                    m_directoryBrowserWindow->setVisible(true);
                }
            }
            if (m_streamingActive && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("%s", T("menu.remote.disabled_streaming_tip").c_str());
            }
            const bool connected = (m_sourceType == SourceType::Remote) && !m_currentDevice.empty();
            if (ImGui::MenuItem(T("menu.remote.disconnect").c_str(), nullptr, false, connected))
            {
                setCurrentDevice("");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu.help").c_str()))
        {
            if (m_creditsWindow)
            {
                bool visible = m_creditsWindow->isVisible();
                if (ImGui::MenuItem(T("menu.help.credits").c_str(), nullptr, visible))
                {
                    m_creditsWindow->setVisible(!visible);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // While in client mode (Remote source + active connection), force the
    // producer-only windows shut so the user can't accidentally have them
    // sitting open from a previous local session — the menu hides their
    // toggles too, so without this they'd be unreachable but still drawing.
    const bool clientModeActive = (m_sourceType == SourceType::Remote) && !m_currentDevice.empty();
    // Symmetric to the menu-item gating above: when we're streaming as
    // host, force the client-side windows shut. Without this the user
    // could have left them open from before they hit Start Streaming
    // and they'd remain visible (and active) on screen.
    if (m_streamingActive)
    {
        if (m_remoteConnectionWindow) m_remoteConnectionWindow->setVisible(false);
        if (m_directoryBrowserWindow) m_directoryBrowserWindow->setVisible(false);
    }
    if (clientModeActive)
    {
        if (m_capturePresetsWindow) m_capturePresetsWindow->setVisible(false);
        if (m_recordingsWindow)     m_recordingsWindow->setVisible(false);
        // Producer-only configuration windows also force-shut in
        // client mode — Source/Streaming/Recording/Web Portal/Audio
        // operate on the local pipeline that isn't running when
        // we're a remote viewer.
        if (m_sourceWindow)    m_sourceWindow->setVisible(false);
        if (m_streamingWindow) m_streamingWindow->setVisible(false);
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        if (m_virtcamWindow)   m_virtcamWindow->setVisible(false);
#endif
        // Recording window stays openable in client mode (#68).
        if (m_webPortalWindow) m_webPortalWindow->setVisible(false);
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        if (m_audioWindow)     m_audioWindow->setVisible(false);
#endif
    }

    // Render the seven configuration windows + Preferences + Info.
    // Each is a no-op when its own m_visible is false.
    if (m_sourceWindow)      m_sourceWindow->render();
    if (m_shaderWindow)      m_shaderWindow->render();
    if (m_imageWindow)       m_imageWindow->render();
    if (m_streamingWindow)   m_streamingWindow->render();
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    if (m_virtcamWindow)     m_virtcamWindow->render();
#endif
    if (m_recordingWindow)   m_recordingWindow->render();
    if (m_webPortalWindow)   m_webPortalWindow->render();
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    if (m_audioWindow)       m_audioWindow->render();
#endif
    if (m_infoWindow)        m_infoWindow->render();
    if (m_preferencesWindow) m_preferencesWindow->render();
    if (m_shortcutsHelpWindow) m_shortcutsHelpWindow->render();
    // m_quickActionsOverlay rendered earlier, above the m_uiVisible gate.

    // Renderizar janela de créditos
    if (m_creditsWindow)
    {
        m_creditsWindow->render();
    }

    // Renderizar janela de conexão remota
    if (m_remoteConnectionWindow)
    {
        m_remoteConnectionWindow->render();
    }

    // Directory browser moved to the OSD layer (rendered above the
    // m_uiVisible gate at the top of this function). No render call
    // here — having two would either double-render or eat input on
    // alternating frames.

    // Renderizar janela de presets
    if (m_capturePresetsWindow)
    {
        m_capturePresetsWindow->render();
    }

    // Renderizar janela de gravações
    if (m_recordingsWindow)
    {
        m_recordingsWindow->render();
    }
}

void UIManager::renderShaderPanel()
{
    ImGui::Text("Shader Preset:");

    // Combo box for shader selection
    if (ImGui::BeginCombo("##shader", m_currentShader.empty() ? "None" : m_currentShader.c_str()))
    {
        if (ImGui::Selectable("None", m_currentShader.empty()))
        {
            m_currentShader = "";
            if (m_onShaderChanged)
            {
                m_onShaderChanged("");
            }
            saveConfig();
        }

        for (size_t i = 0; i < m_scannedShaders.size(); ++i)
        {
            bool isSelected = (m_currentShader == m_scannedShaders[i]);
            if (ImGui::Selectable(m_scannedShaders[i].c_str(), isSelected))
            {
                m_currentShader = m_scannedShaders[i];
                if (m_onShaderChanged)
                {
                    m_onShaderChanged(m_scannedShaders[i]);
                }
                saveConfig();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text("Shaders found: %zu", m_scannedShaders.size());

    // Botões de salvar preset
    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        ImGui::Separator();
        ImGui::Text("Save Preset:");

        std::string currentPreset = m_shaderEngine->getPresetPath();
        if (!currentPreset.empty())
        {
            // Extrair apenas o nome do arquivo
            fs::path presetPath(currentPreset);
            std::string fileName = presetPath.filename();

            if (ImGui::Button("Save"))
            {
                if (m_onSavePreset)
                {
                    m_onSavePreset(currentPreset, true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As..."))
            {
                strncpy(m_savePresetPath, fileName.c_str(), sizeof(m_savePresetPath) - 1);
                m_savePresetPath[sizeof(m_savePresetPath) - 1] = '\0';
                m_showSaveDialog = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No preset loaded");
        }

        // Dialog para "Save As"
        if (m_showSaveDialog)
        {
            ImGui::OpenPopup("Save Preset As");
            m_showSaveDialog = false;
        }

        if (ImGui::BeginPopupModal("Save Preset As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter preset filename:");
            ImGui::InputText("##presetname", m_savePresetPath, sizeof(m_savePresetPath));

            if (ImGui::Button("Save"))
            {
                if (m_onSavePreset && strlen(m_savePresetPath) > 0)
                {
                    // Construir caminho completo
                    fs::path basePath = fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl";
                    fs::path newPath = basePath / m_savePresetPath;
                    if (newPath.extension() != ".glslp")
                    {
                        newPath.replace_extension(".glslp");
                    }
                    m_onSavePreset(newPath.string(), false);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        ImGui::Separator();
        ImGui::Text("Shader Parameters:");

        auto params = m_shaderEngine->getShaderParameters();
        if (params.empty())
        {
            ImGui::TextDisabled("No parameters available");
        }
        else
        {
            for (auto &param : params)
            {
                ImGui::PushID(param.name.c_str());

                if (!param.description.empty())
                {
                    ImGui::Text("%s", param.description.c_str());
                }
                else
                {
                    ImGui::Text("%s", param.name.c_str());
                }

                // Slider para o parâmetro
                float value = param.value;
                if (ImGui::SliderFloat("##param", &value, param.min, param.max, "%.3f"))
                {
                    m_shaderEngine->setShaderParameter(param.name, value);
                }

                ImGui::SameLine();
                if (ImGui::Button("Reset##param"))
                {
                    m_shaderEngine->setShaderParameter(param.name, param.defaultValue);
                }

                ImGui::PopID();
            }
        }
    }
}

void UIManager::renderImageControls()
{
    ImGui::Text("Image Adjustments");
    ImGui::Separator();

    // Brightness
    float brightness = m_brightness;
    if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 2.0f, "%.2f"))
    {
        m_brightness = brightness;
        if (m_onBrightnessChanged)
        {
            m_onBrightnessChanged(brightness);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##brightness"))
    {
        m_brightness = 1.0f;
        if (m_onBrightnessChanged)
        {
            m_onBrightnessChanged(1.0f);
        }
    }

    // Contrast
    float contrast = m_contrast;
    if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 5.0f, "%.2f"))
    {
        m_contrast = contrast;
        if (m_onContrastChanged)
        {
            m_onContrastChanged(contrast);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##contrast"))
    {
        m_contrast = 1.0f;
        if (m_onContrastChanged)
        {
            m_onContrastChanged(1.0f);
        }
    }

    ImGui::Separator();

    // Maintain aspect ratio
    bool maintainAspect = m_maintainAspect;
    if (ImGui::Checkbox("Maintain Aspect Ratio", &maintainAspect))
    {
        m_maintainAspect = maintainAspect;
        if (m_onMaintainAspectChanged)
        {
            m_onMaintainAspectChanged(maintainAspect);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Fullscreen
    bool fullscreen = m_fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen))
    {
        m_fullscreen = fullscreen;
        if (m_onFullscreenChanged)
        {
            m_onFullscreenChanged(fullscreen);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Monitor Index (usado quando fullscreen está ativo)
    ImGui::Separator();
    ImGui::Text("Monitor Index:");
    if (!fullscreen && !m_fullscreen)
    {
        ImGui::TextDisabled("(only used in fullscreen mode)");
    }
    else
    {
        ImGui::TextDisabled("(-1 = primary monitor, 0+ = specific monitor)");
    }
    int monitorIndex = m_monitorIndex;
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("##monitor", &monitorIndex, 1, 5))
    {
        monitorIndex = std::max(-1, monitorIndex); // Não permitir valores negativos menores que -1
        m_monitorIndex = monitorIndex;
        if (m_onMonitorIndexChanged)
        {
            m_onMonitorIndexChanged(monitorIndex);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Reset##monitor"))
    {
        m_monitorIndex = -1;
        if (m_onMonitorIndexChanged)
        {
            m_onMonitorIndexChanged(-1);
        }
    }
}

void UIManager::renderSourcePanel()
{
    ImGui::Text("Source Type:");
    ImGui::Separator();
    ImGui::Spacing();

    // Dropdown para seleção do tipo de fonte
    const char *sourceTypeNames[] = {"None", "V4L2"};
    int currentSourceType = static_cast<int>(m_sourceType);

    if (ImGui::Combo("##sourceType", &currentSourceType, sourceTypeNames, IM_ARRAYSIZE(sourceTypeNames)))
    {
        m_sourceType = static_cast<SourceType>(currentSourceType);
        if (m_onSourceTypeChanged)
        {
            m_onSourceTypeChanged(m_sourceType);
        }
        saveConfig();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Renderizar controles específicos da fonte selecionada
    if (m_sourceType == SourceType::V4L2)
    {
        renderV4L2Controls();
    }
    else if (m_sourceType == SourceType::None)
    {
        ImGui::TextWrapped("No source selected. Pick a source type above.");
    }
}

void UIManager::renderV4L2Controls()
{
    // Sempre mostrar controles, mesmo sem dispositivo
    // Se não houver dispositivo, mostrar mensagem informativa
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("No V4L2 device connected. Select a device below to start capture.");
        ImGui::Separator();
    }

    // Device selection
    ImGui::Text("V4L2 Device:");
    ImGui::Separator();

    // Scan devices if list is empty
    if (m_v4l2Devices.empty())
    {
        refreshV4L2Devices();
    }

    // Combo box for device selection
    // Adicionar "None" como primeira opção
    std::string displayText = m_currentDevice.empty() ? "None (No device)" : m_currentDevice;
    int selectedIndex = -1;

    // Verificar se "None" está selecionado
    if (m_currentDevice.empty())
    {
        selectedIndex = 0; // "None" é o índice 0
    }
    else
    {
        // Procurar dispositivo na lista (índice +1 porque "None" é 0)
        for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
        {
            if (m_v4l2Devices[i] == m_currentDevice)
            {
                selectedIndex = static_cast<int>(i) + 1; // +1 porque "None" é 0
                break;
            }
        }
    }

    if (ImGui::BeginCombo("##device", displayText.c_str()))
    {
        // Opção "None" sempre como primeira opção
        bool isNoneSelected = m_currentDevice.empty();
        if (ImGui::Selectable("None (No device)", isNoneSelected))
        {
            m_currentDevice = ""; // String vazia = None
            if (m_onDeviceChanged)
            {
                m_onDeviceChanged(""); // Passar string vazia para indicar "None"
            }
            saveConfig();
        }
        if (isNoneSelected)
        {
            ImGui::SetItemDefaultFocus();
        }

        // Listar dispositivos disponíveis
        for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i) + 1);
            if (ImGui::Selectable(m_v4l2Devices[i].c_str(), isSelected))
            {
                m_currentDevice = m_v4l2Devices[i];
                if (m_onDeviceChanged)
                {
                    m_onDeviceChanged(m_v4l2Devices[i]);
                }
                saveConfig();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh##devices"))
    {
        refreshV4L2Devices();
    }

    ImGui::Separator();
    ImGui::Text("Capture Resolution & Framerate");
    ImGui::Separator();

    // Controles de resolução
    ImGui::Text("Resolution:");
    int width = static_cast<int>(m_captureWidth);
    int height = static_cast<int>(m_captureHeight);

    ImGui::PushItemWidth(100);
    ImGui::PushID("width");
    bool widthEdited = ImGui::InputInt("Width##capture", &width, 1, 10);
    width = std::max(1, std::min(7680, width)); // Limitar entre 1 e 7680
    bool widthDeactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();

    ImGui::SameLine();

    ImGui::PushID("height");
    bool heightEdited = ImGui::InputInt("Height##capture", &height, 1, 10);
    height = std::max(1, std::min(4320, height)); // Limitar entre 1 e 4320
    bool heightDeactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    ImGui::PopItemWidth();

    // Aplicar mudanças quando qualquer campo perder o foco
    if ((widthDeactivated || heightDeactivated) && (widthEdited || heightEdited))
    {
        if (width != static_cast<int>(m_captureWidth) || height != static_cast<int>(m_captureHeight))
        {
            m_captureWidth = static_cast<uint32_t>(width);
            m_captureHeight = static_cast<uint32_t>(height);
            if (m_onResolutionChanged)
            {
                m_onResolutionChanged(m_captureWidth, m_captureHeight);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }

    // Controle de FPS
    ImGui::Text("Framerate:");
    int fps = static_cast<int>(m_captureFps);
    ImGui::PushItemWidth(100);
    bool fpsEdited = ImGui::InputInt("FPS##capture", &fps, 1, 5);
    fps = std::max(1, std::min(240, fps)); // Limitar entre 1 e 240
    ImGui::PopItemWidth();

    // Aplicar mudanças quando o campo perder o foco
    if (ImGui::IsItemDeactivatedAfterEdit() && fpsEdited)
    {
        if (fps != static_cast<int>(m_captureFps))
        {
            m_captureFps = static_cast<uint32_t>(fps);
            if (m_onFramerateChanged)
            {
                m_onFramerateChanged(m_captureFps);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }

    // FPS comuns (botões rápidos)
    ImGui::Text("Quick FPS:");
    if (ImGui::Button("30"))
    {
        m_captureFps = 30;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(30);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("60"))
    {
        m_captureFps = 60;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(60);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("120"))
    {
        m_captureFps = 120;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(120);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();

    // Resoluções 4:3
    ImGui::Text("4:3 Resolutions:");
    if (ImGui::Button("320x240"))
    {
        m_captureWidth = 320;
        m_captureHeight = 240;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(320, 240);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("640x480"))
    {
        m_captureWidth = 640;
        m_captureHeight = 480;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(640, 480);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("800x600"))
    {
        m_captureWidth = 800;
        m_captureHeight = 600;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(800, 600);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("1024x768"))
    {
        m_captureWidth = 1024;
        m_captureHeight = 768;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1024, 768);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1280x960"))
    {
        m_captureWidth = 1280;
        m_captureHeight = 960;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1280, 960);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1600x1200"))
    {
        m_captureWidth = 1600;
        m_captureHeight = 1200;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1600, 1200);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("2048x1536"))
    {
        m_captureWidth = 2048;
        m_captureHeight = 1536;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2048, 1536);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1920"))
    {
        m_captureWidth = 2560;
        m_captureHeight = 1920;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2560, 1920);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();

    // Resoluções 16:9
    ImGui::Text("16:9 Resolutions:");
    if (ImGui::Button("1280x720"))
    {
        m_captureWidth = 1280;
        m_captureHeight = 720;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1280, 720);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1920x1080"))
    {
        m_captureWidth = 1920;
        m_captureHeight = 1080;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1920, 1080);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1440"))
    {
        m_captureWidth = 2560;
        m_captureHeight = 1440;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2560, 1440);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("3840x2160"))
    {
        m_captureWidth = 3840;
        m_captureHeight = 2160;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(3840, 2160);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();
    ImGui::Text("V4L2 Hardware Controls");
    ImGui::Separator();

    // Renderizar controles dinâmicos (discovered from device)
    for (size_t i = 0; i < m_v4l2Controls.size(); ++i)
    {
        auto &control = m_v4l2Controls[i];
        if (!control.available)
        {
            continue;
        }

        // Use PushID to create unique IDs for each control
        ImGui::PushID(static_cast<int>(i));
        std::string label = control.name + "##dynamic";
        int32_t value = control.value;
        if (ImGui::SliderInt(label.c_str(), &value, control.min, control.max))
        {
            control.value = value;
            if (m_onV4L2ControlChanged)
            {
                m_onV4L2ControlChanged(control.name, value);
            }
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Text("All V4L2 Controls:");
    ImGui::Separator();

    // Helper function para renderizar controle com range do dispositivo ou padrão
    auto renderControl = [this](const char *name, int32_t defaultMin, int32_t defaultMax, int32_t defaultValue)
    {
        if (!m_capture)
            return;

        int32_t value, min, max;

        // Tentar obter valores do dispositivo usando interface genérica
        if (m_capture->getControl(name, value) &&
            m_capture->getControlMin(name, min) &&
            m_capture->getControlMax(name, max))
        {
            // Valores obtidos com sucesso
        }
        else
        {
            // Se não disponível, usar valores padrão
            min = defaultMin;
            max = defaultMax;
            value = defaultValue;
        }

        // Clamp valor
        value = std::max(min, std::min(max, value));

        // Use unique ID with suffix to avoid conflicts with dynamic controls
        std::string label = std::string(name) + "##manual";
        if (ImGui::SliderInt(label.c_str(), &value, min, max))
        {
            value = std::max(min, std::min(max, value));

            if (m_onV4L2ControlChanged)
            {
                m_onV4L2ControlChanged(name, value);
            }
        }
    };

    // Brightness
    renderControl("Brightness", -100, 100, 0);

    // Contrast
    renderControl("Contrast", -100, 100, 0);

    // Saturation
    renderControl("Saturation", -100, 100, 0);

    // Hue
    renderControl("Hue", -100, 100, 0);

    // Gain
    renderControl("Gain", 0, 100, 0);

    // Exposure
    renderControl("Exposure", -13, 1, 0);

    // Sharpness
    renderControl("Sharpness", 0, 6, 0);

    // Gamma
    renderControl("Gamma", 100, 300, 100);

    // White Balance
    renderControl("White Balance", 2800, 6500, 4000);
}

void UIManager::setCaptureControls(IVideoCapture *capture)
{
    m_capture = capture;
    m_v4l2Controls.clear();

    if (!capture)
    {
        return;
    }

    // Se o tipo de fonte for DirectShow, atualizar lista de dispositivos
#ifdef _WIN32
    if (m_sourceType == SourceType::DS)
    {
        // Sempre atualizar lista quando m_capture é setado
        refreshDSDevices();
        LOG_INFO("DirectShow device list updated after setCaptureControls: " + std::to_string(m_dsDevices.size()) + " device(s)");
    }
#endif

    // Lista de controles comuns (usando interface genérica)
    const char *controlNames[] = {
        "Brightness",
        "Contrast",
        "Saturation",
        "Hue",
        "Gain",
        "Exposure",
        "Sharpness",
        "Gamma",
        "White Balance",
    };

    for (const char *name : controlNames)
    {
        V4L2Control ctrl;
        ctrl.name = name;

        // Usar interface genérica para obter informações do controle
        // Verificar se o dispositivo está aberto antes de tentar obter controles
        if (!capture->isOpen())
        {
            ctrl.available = false;
            m_v4l2Controls.push_back(ctrl);
            continue;
        }

        int32_t value, minVal, maxVal;
        if (capture->getControl(name, value) &&
            capture->getControlMin(name, minVal) &&
            capture->getControlMax(name, maxVal))
        {
            ctrl.value = value;
            ctrl.min = minVal;
            ctrl.max = maxVal;
            ctrl.step = 1; // Step não disponível na interface genérica
            ctrl.available = true;
            m_v4l2Controls.push_back(ctrl);
        }
        else
        {
            // Controle não disponível - não adicionar à lista
            ctrl.available = false;
        }
    }
}

void UIManager::renderInfoPanel()
{
    ImGui::Text("Capture Information");
    ImGui::Separator();

    ImGui::Text("Device: %s", m_captureDevice.c_str());
    ImGui::Text("Resolution: %ux%u", m_captureWidth, m_captureHeight);
    ImGui::Text("FPS: %u", m_captureFps);

    ImGui::Separator();
    ImGui::Text("Application Info");
    ImGui::Text("RetroCapture v0.1.0");
    ImGui::Text("ImGui: %s", ImGui::GetVersion());
}

void UIManager::setCaptureInfo(uint32_t width, uint32_t height, uint32_t fps, const std::string &device)
{
    // width/height são o que o V4L2 entregou (depois do ajuste). Vão pra
    // m_actualCaptureWidth/Height. Não sobrescrevemos m_captureWidth/Height
    // (a preferência LÓGICA do usuário) — assim o campo de Resolution na UI
    // continua mostrando a escolha do usuário, mesmo quando o dispositivo
    // adjusta pra mais próxima suportada (e o pipeline faz downscale).
    m_actualCaptureWidth = width;
    m_actualCaptureHeight = height;
    if (m_captureWidth == 0)
    {
        m_captureWidth = width;
    }
    if (m_captureHeight == 0)
    {
        m_captureHeight = height;
    }
    m_captureFps = fps;
    m_captureDevice = device;
    if (m_currentDevice.empty())
    {
        m_currentDevice = device;
    }
}

void UIManager::renderStreamingPanel()
{
    ImGui::Text("HTTP MPEG-TS Streaming (audio + video)");
    ImGui::Separator();

    // Status — bullet drawn via ui_status_indicator() in
    // UISectionHeader.h so it goes through ImDrawList primitives
    // instead of an untyped Unicode glyph the default font can't
    // render.
    ui_status_indicator(m_streamingActive, "Active", "Inactive");

    if (m_streamingActive && !m_streamUrl.empty())
    {
        ImGui::Text("URL: %s", m_streamUrl.c_str());
        ImGui::Text("Clientes conectados: %u", m_streamClientCount);
    }

    ImGui::Separator();
    ImGui::Text("Basic Settings");
    ImGui::Separator();

    // Controles básicos
    int port = static_cast<int>(m_streamingPort);
    if (ImGui::InputInt("Port", &port, 1, 100))
    {
        // Validation is done in setStreamingPort/triggerStreamingPortChange
        if (port >= 1024 && port <= 65535)
        {
            triggerStreamingPortChange(static_cast<uint16_t>(port));
        }
    }

    // Resolução - Dropdown
    const char *resolutions[] = {
        "Captura (0x0)",
        "320x240",
        "640x480",
        "800x600",
        "1024x768",
        "1280x720 (HD)",
        "1280x1024",
        "1920x1080 (Full HD)",
        "2560x1440 (2K)",
        "3840x2160 (4K)"};
    const uint32_t resolutionWidths[] = {0, 320, 640, 800, 1024, 1280, 1280, 1920, 2560, 3840};
    const uint32_t resolutionHeights[] = {0, 240, 480, 600, 768, 720, 1024, 1080, 1440, 2160};

    int currentResIndex = 0;
    for (int i = 0; i < 10; i++)
    {
        if (m_streamingWidth == resolutionWidths[i] && m_streamingHeight == resolutionHeights[i])
        {
            currentResIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Resolution", &currentResIndex, resolutions, 10))
    {
        m_streamingWidth = resolutionWidths[currentResIndex];
        m_streamingHeight = resolutionHeights[currentResIndex];
        if (m_onStreamingWidthChanged)
        {
            m_onStreamingWidthChanged(m_streamingWidth);
        }
        if (m_onStreamingHeightChanged)
        {
            m_onStreamingHeightChanged(m_streamingHeight);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // FPS - Dropdown
    const char *fpsOptions[] = {"Captura (0)", "15", "24", "30", "60", "120"};
    const uint32_t fpsValues[] = {0, 15, 24, 30, 60, 120};

    int currentFpsIndex = 0;
    for (int i = 0; i < 6; i++)
    {
        if (m_streamingFps == fpsValues[i])
        {
            currentFpsIndex = i;
            break;
        }
    }

    if (ImGui::Combo("FPS", &currentFpsIndex, fpsOptions, 6))
    {
        m_streamingFps = fpsValues[currentFpsIndex];
        if (m_onStreamingFpsChanged)
        {
            m_onStreamingFpsChanged(m_streamingFps);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();
    ImGui::Text("Codecs");
    ImGui::Separator();

    // Seleção de codec de vídeo
    const char *videoCodecs[] = {"h264", "h265", "vp8", "vp9"};
    int currentVideoCodecIndex = 0;
    for (int i = 0; i < 4; i++)
    {
        if (m_streamingVideoCodec == videoCodecs[i])
        {
            currentVideoCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Video Codec", &currentVideoCodecIndex, videoCodecs, 4))
    {
        m_streamingVideoCodec = videoCodecs[currentVideoCodecIndex];
        if (m_onStreamingVideoCodecChanged)
        {
            m_onStreamingVideoCodecChanged(m_streamingVideoCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Seleção de codec de áudio
    const char *audioCodecs[] = {"aac", "mp3", "opus"};
    int currentAudioCodecIndex = 0;
    for (int i = 0; i < 3; i++)
    {
        if (m_streamingAudioCodec == audioCodecs[i])
        {
            currentAudioCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Codec de Áudio", &currentAudioCodecIndex, audioCodecs, 3))
    {
        m_streamingAudioCodec = audioCodecs[currentAudioCodecIndex];
        if (m_onStreamingAudioCodecChanged)
        {
            m_onStreamingAudioCodecChanged(m_streamingAudioCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Qualidade H.264 (apenas se codec for h264)
    if (m_streamingVideoCodec == "h264")
    {
        const char *h264Presets[] = {
            "ultrafast",
            "superfast",
            "veryfast",
            "faster",
            "fast",
            "medium",
            "slow",
            "slower",
            "veryslow"};
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++)
        {
            if (m_streamingH264Preset == h264Presets[i])
            {
                currentPresetIndex = i;
                break;
            }
        }

        if (ImGui::Combo("H.264 Quality", &currentPresetIndex, h264Presets, 9))
        {
            m_streamingH264Preset = h264Presets[currentPresetIndex];
            if (m_onStreamingH264PresetChanged)
            {
                m_onStreamingH264PresetChanged(m_streamingH264Preset);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("H.264 encoder preset:\n"
                              "ultrafast/superfast/veryfast: maximum speed, lower quality\n"
                              "fast/medium: balance between speed and quality\n"
                              "slow/slower/veryslow: maximum quality, lower speed");
        }
    }

    // Qualidade H.265 (apenas se codec for h265)
    if (m_streamingVideoCodec == "h265" || m_streamingVideoCodec == "hevc")
    {
        const char *h265Presets[] = {
            "ultrafast",
            "superfast",
            "veryfast",
            "faster",
            "fast",
            "medium",
            "slow",
            "slower",
            "veryslow"};
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++)
        {
            if (m_streamingH265Preset == h265Presets[i])
            {
                currentPresetIndex = i;
                break;
            }
        }

        if (ImGui::Combo("H.265 Quality", &currentPresetIndex, h265Presets, 9))
        {
            m_streamingH265Preset = h265Presets[currentPresetIndex];
            if (m_onStreamingH265PresetChanged)
            {
                m_onStreamingH265PresetChanged(m_streamingH265Preset);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("H.265 encoder preset:\n"
                              "ultrafast/superfast/veryfast: maximum speed, lower quality\n"
                              "fast/medium: balance between speed and quality\n"
                              "slow/slower/veryslow: maximum quality, lower speed");
        }

        // Profile H.265
        const char *h265Profiles[] = {"main", "main10"};
        int currentProfileIndex = 0;
        for (int i = 0; i < 2; i++)
        {
            if (m_streamingH265Profile == h265Profiles[i])
            {
                currentProfileIndex = i;
                break;
            }
        }

        if (ImGui::Combo("H.265 Profile", &currentProfileIndex, h265Profiles, 2))
        {
            m_streamingH265Profile = h265Profiles[currentProfileIndex];
            if (m_onStreamingH265ProfileChanged)
            {
                m_onStreamingH265ProfileChanged(m_streamingH265Profile);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("H.265 encoder profile:\n"
                              "main: 8-bit, maximum compatibility\n"
                              "main10: 10-bit, better quality, HDR support");
        }

        // Level H.265
        const char *h265Levels[] = {
            "auto", "1", "2", "2.1", "3", "3.1",
            "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"};
        int currentLevelIndex = 0;
        for (int i = 0; i < 14; i++)
        {
            if (m_streamingH265Level == h265Levels[i])
            {
                currentLevelIndex = i;
                break;
            }
        }

        if (ImGui::Combo("H.265 Level", &currentLevelIndex, h265Levels, 14))
        {
            m_streamingH265Level = h265Levels[currentLevelIndex];
            if (m_onStreamingH265LevelChanged)
            {
                m_onStreamingH265LevelChanged(m_streamingH265Level);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("H.265 encoder level:\n"
                              "auto: automatic detection (recommended)\n"
                              "1-6.2: specific levels for compatibility\n"
                              "Higher levels support larger resolutions/bitrates");
        }
    }

    // Configurações VP8 (apenas se codec for vp8)
    if (m_streamingVideoCodec == "vp8")
    {
        int currentSpeed = m_streamingVP8Speed;
        if (ImGui::SliderInt("VP8 Speed (0-16)", &currentSpeed, 0, 16))
        {
            m_streamingVP8Speed = currentSpeed;
            if (m_onStreamingVP8SpeedChanged)
            {
                m_onStreamingVP8SpeedChanged(m_streamingVP8Speed);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("VP8 encoder speed:\n"
                              "0: best quality, slower\n"
                              "16: faster, lower quality\n"
                              "12: good balance for streaming");
        }
    }

    // Configurações VP9 (apenas se codec for vp9)
    if (m_streamingVideoCodec == "vp9")
    {
        int currentSpeed = m_streamingVP9Speed;
        if (ImGui::SliderInt("VP9 Speed (0-9)", &currentSpeed, 0, 9))
        {
            m_streamingVP9Speed = currentSpeed;
            if (m_onStreamingVP9SpeedChanged)
            {
                m_onStreamingVP9SpeedChanged(m_streamingVP9Speed);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("VP9 encoder speed:\n"
                              "0: best quality, slower\n"
                              "9: faster, lower quality\n"
                              "6: good balance for streaming");
        }
    }

    ImGui::Separator();
    ImGui::Text("Bitrates");
    ImGui::Separator();

    // Bitrate de vídeo
    int bitrate = static_cast<int>(m_streamingBitrate);
    if (ImGui::InputInt("Video Bitrate (kbps, 0 = auto)", &bitrate, 100, 1000))
    {
        // Limites: 0 (auto) ou 100-100000 kbps
        if (bitrate == 0 || (bitrate >= 100 && bitrate <= 100000))
        {
            m_streamingBitrate = static_cast<uint32_t>(bitrate);
            if (m_onStreamingBitrateChanged)
            {
                m_onStreamingBitrateChanged(m_streamingBitrate);
            }
            saveConfig();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Video bitrate in kbps.\n"
                          "0 = automatic (based on resolution/FPS)\n"
                          "100-100000 kbps: valid range\n"
                          "Recommended: 2000-8000 kbps for streaming");
    }

    // Bitrate de áudio
    int audioBitrate = static_cast<int>(m_streamingAudioBitrate);
    if (ImGui::InputInt("Audio Bitrate (kbps)", &audioBitrate, 8, 32))
    {
        // Limites: 64-320 kbps (32 é muito baixo para qualidade aceitável)
        if (audioBitrate >= 64 && audioBitrate <= 320)
        {
            m_streamingAudioBitrate = static_cast<uint32_t>(audioBitrate);
            if (m_onStreamingAudioBitrateChanged)
            {
                m_onStreamingAudioBitrateChanged(m_streamingAudioBitrate);
            }
            saveConfig();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Audio bitrate in kbps.\n"
                          "64-320 kbps: valid range\n"
                          "Recommended: 128-256 kbps for good quality");
    }

    ImGui::Separator();
    ImGui::Text("Buffer (Advanced)");
    ImGui::Separator();

    // Max Video Buffer Size
    int maxVideoBuffer = static_cast<int>(m_streamingMaxVideoBufferSize);
    if (ImGui::SliderInt("Max Frames in Buffer", &maxVideoBuffer, 1, 50))
    {
        m_streamingMaxVideoBufferSize = static_cast<size_t>(maxVideoBuffer);
        if (m_onStreamingMaxVideoBufferSizeChanged)
        {
            m_onStreamingMaxVideoBufferSizeChanged(m_streamingMaxVideoBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max video frames in the buffer.\n"
                          "1-50 frames: valid range\n"
                          "Default: 10 frames\n"
                          "Higher values = more memory, less risk of dropped frames");
    }

    // Max Audio Buffer Size
    int maxAudioBuffer = static_cast<int>(m_streamingMaxAudioBufferSize);
    if (ImGui::SliderInt("Max Chunks in Buffer", &maxAudioBuffer, 5, 100))
    {
        m_streamingMaxAudioBufferSize = static_cast<size_t>(maxAudioBuffer);
        if (m_onStreamingMaxAudioBufferSizeChanged)
        {
            m_onStreamingMaxAudioBufferSizeChanged(m_streamingMaxAudioBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max audio chunks in the buffer.\n"
                          "5-100 chunks: valid range\n"
                          "Default: 20 chunks\n"
                          "Higher values = more memory, better sync");
    }

    // Max Buffer Time
    int maxBufferTime = static_cast<int>(m_streamingMaxBufferTimeSeconds);
    if (ImGui::SliderInt("Max Buffer Time (seconds)", &maxBufferTime, 1, 30))
    {
        m_streamingMaxBufferTimeSeconds = static_cast<int64_t>(maxBufferTime);
        if (m_onStreamingMaxBufferTimeSecondsChanged)
        {
            m_onStreamingMaxBufferTimeSecondsChanged(m_streamingMaxBufferTimeSeconds);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max buffer time in seconds.\n"
                          "1-30 seconds: valid range\n"
                          "Default: 5 seconds\n"
                          "Controls how much video/audio can be queued before processing");
    }

    // AVIO Buffer Size
    int avioBuffer = static_cast<int>(m_streamingAVIOBufferSize / 1024); // Converter para KB
    if (ImGui::SliderInt("AVIO Buffer (KB)", &avioBuffer, 64, 1024))
    {
        m_streamingAVIOBufferSize = static_cast<size_t>(avioBuffer * 1024);
        if (m_onStreamingAVIOBufferSizeChanged)
        {
            m_onStreamingAVIOBufferSizeChanged(m_streamingAVIOBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("FFmpeg AVIO buffer size in KB.\n"
                          "64-1024 KB: valid range\n"
                          "Default: 256 KB\n"
                          "FFmpeg internal buffer for streaming I/O");
    }

    ImGui::Separator();

    // Botão Start/Stop
    // Desabilitar botão se estiver processando (start/stop em andamento)
    if (m_streamingProcessing)
    {
        ImGui::BeginDisabled();
        if (m_streamingActive)
        {
            ImGui::Button("Parando...", ImVec2(-1, 0));
        }
        else
        {
            ImGui::Button("Iniciando...", ImVec2(-1, 0));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Aguarde o processo terminar");
        }
    }
    else if (m_streamingActive)
    {
        if (ImGui::Button("Stop Streaming", ImVec2(-1, 0)))
        {
            if (m_onStreamingStartStop)
            {
                m_streamingProcessing = true; // Marcar como processando
                m_onStreamingStartStop(false);
            }
        }
    }
    else
    {
        // Desabilitar botão se estiver em cooldown
        if (m_streamingCooldownRemainingMs > 0)
        {
            ImGui::BeginDisabled();
            float cooldownSeconds = m_streamingCooldownRemainingMs / 1000.0f;
            std::string label = "Aguardando (" + std::to_string(static_cast<int>(cooldownSeconds)) + "s)";
            ImGui::Button(label.c_str(), ImVec2(-1, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Aguarde o cooldown terminar antes de iniciar o streaming novamente");
            }
        }
        else
        {
            if (ImGui::Button("Start Streaming", ImVec2(-1, 0)))
            {
                if (m_onStreamingStartStop)
                {
                    m_streamingProcessing = true; // Marcar como processando
                    m_onStreamingStartStop(true);
                }
            }
        }
    }
}

void UIManager::scanV4L2Devices()
{
#ifdef PLATFORM_LINUX
    m_v4l2Devices = V4L2DeviceScanner::scan();
#else
    // Windows não usa V4L2
    m_v4l2Devices.clear();
#endif
}

void UIManager::refreshV4L2Devices()
{
    scanV4L2Devices();
}

void UIManager::refreshDSDevices()
{
    if (!m_capture)
    {
        // Não limpar a lista - pode ter sido populada anteriormente
        // Apenas não atualizar se m_capture não estiver disponível
        return;
    }

    m_dsDevices = m_capture->listDevices();
}

void UIManager::setSourceType(SourceType sourceType)
{
    m_sourceType = sourceType;

    // Atualizar cache de dispositivos quando mudar o tipo de fonte
#ifdef _WIN32
    if (sourceType == SourceType::DS)
    {
        // Garantir que m_capture está disponível antes de atualizar
        if (m_capture)
        {
            refreshDSDevices();
        }
        else
        {
            // Se m_capture não estiver disponível, limpar lista
            m_dsDevices.clear();
        }
    }
#endif

    if (m_onSourceTypeChanged)
    {
        m_onSourceTypeChanged(sourceType);
    }
    saveConfig();
}

void UIManager::setStreamingPort(uint16_t port)
{
    // Validate port range (1024-65535)
    // Note: uint16_t max is 65535, so we only need to check >= 1024
    if (port >= 1024)
    {
        m_streamingPort = port;
    }
    else
    {
        LOG_WARN("Invalid streaming port: " + std::to_string(port) + " (must be between 1024 and 65535)");
    }
}

void UIManager::triggerStreamingPortChange(uint16_t port)
{
    // Validate and set port (validation is done in setStreamingPort)
    setStreamingPort(port);
    if (m_onStreamingPortChanged)
    {
        m_onStreamingPortChanged(m_streamingPort);
    }
    saveConfig();
}

void UIManager::triggerStreamingWidthChange(uint32_t width)
{
    m_streamingWidth = width;
    if (m_onStreamingWidthChanged)
    {
        m_onStreamingWidthChanged(width);
    }
    saveConfig();
}

void UIManager::triggerStreamingHeightChange(uint32_t height)
{
    m_streamingHeight = height;
    if (m_onStreamingHeightChanged)
    {
        m_onStreamingHeightChanged(height);
    }
    saveConfig();
}

void UIManager::triggerStreamingFpsChange(uint32_t fps)
{
    m_streamingFps = fps;
    if (m_onStreamingFpsChanged)
    {
        m_onStreamingFpsChanged(fps);
    }
    saveConfig();
}

void UIManager::triggerStreamingBitrateChange(uint32_t bitrate)
{
    m_streamingBitrate = bitrate;
    if (m_onStreamingBitrateChanged)
    {
        m_onStreamingBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerStreamingAudioBitrateChange(uint32_t bitrate)
{
    m_streamingAudioBitrate = bitrate;
    if (m_onStreamingAudioBitrateChanged)
    {
        m_onStreamingAudioBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerStreamingVideoCodecChange(const std::string &codec)
{
    m_streamingVideoCodec = codec;
    if (m_onStreamingVideoCodecChanged)
    {
        m_onStreamingVideoCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerStreamingAudioCodecChange(const std::string &codec)
{
    m_streamingAudioCodec = codec;
    if (m_onStreamingAudioCodecChanged)
    {
        m_onStreamingAudioCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerStreamingH264PresetChange(const std::string &preset)
{
    m_streamingH264Preset = preset;
    if (m_onStreamingH264PresetChanged)
    {
        m_onStreamingH264PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265PresetChange(const std::string &preset)
{
    m_streamingH265Preset = preset;
    if (m_onStreamingH265PresetChanged)
    {
        m_onStreamingH265PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265ProfileChange(const std::string &profile)
{
    m_streamingH265Profile = profile;
    if (m_onStreamingH265ProfileChanged)
    {
        m_onStreamingH265ProfileChanged(profile);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265LevelChange(const std::string &level)
{
    m_streamingH265Level = level;
    if (m_onStreamingH265LevelChanged)
    {
        m_onStreamingH265LevelChanged(level);
    }
    saveConfig();
}

void UIManager::triggerDeviceChange(const std::string &device)
{
    m_currentDevice = device;
    if (m_onDeviceChanged)
    {
        m_onDeviceChanged(device);
    }
}

void UIManager::triggerStreamingVP8SpeedChange(int speed)
{
    m_streamingVP8Speed = speed;
    if (m_onStreamingVP8SpeedChanged)
    {
        m_onStreamingVP8SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerStreamingVP9SpeedChange(int speed)
{
    m_streamingVP9Speed = speed;
    if (m_onStreamingVP9SpeedChanged)
    {
        m_onStreamingVP9SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerStreamingHardwareEncoderChange(int v)
{
    m_streamingHardwareEncoder = v;
    if (m_onStreamingHardwareEncoderChanged)
    {
        m_onStreamingHardwareEncoderChanged(v);
    }
    saveConfig();
}

void UIManager::triggerStreamingNvencPresetChange(const std::string &v)
{
    m_streamingNvencPreset = v;
    if (m_onStreamingNvencPresetChanged) m_onStreamingNvencPresetChanged(v);
    saveConfig();
}

void UIManager::triggerStreamingVaapiRcModeChange(const std::string &v)
{
    m_streamingVaapiRcMode = v;
    if (m_onStreamingVaapiRcModeChanged) m_onStreamingVaapiRcModeChanged(v);
    saveConfig();
}

void UIManager::triggerStreamingQsvPresetChange(const std::string &v)
{
    m_streamingQsvPreset = v;
    if (m_onStreamingQsvPresetChanged) m_onStreamingQsvPresetChanged(v);
    saveConfig();
}

void UIManager::triggerStreamingAmfQualityChange(const std::string &v)
{
    m_streamingAmfQuality = v;
    if (m_onStreamingAmfQualityChanged) m_onStreamingAmfQualityChanged(v);
    saveConfig();
}

void UIManager::triggerRemoteInterpolationChange(const std::string &v)
{
    m_remoteInterpolation = v;
    if (m_onRemoteInterpolationChanged) m_onRemoteInterpolationChanged(v);
    saveConfig();
}

void UIManager::triggerScreenRegionChange(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    m_screenRegionX = x; m_screenRegionY = y; m_screenRegionW = w; m_screenRegionH = h;
    if (m_onScreenRegionChanged) m_onScreenRegionChanged(x, y, w, h);
    saveConfig();
}

void UIManager::triggerRemoteAudioVolumeChange(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    m_remoteAudioVolume = volume;
    // Moving the slider above zero is an implicit unmute.
    if (volume > 0.0f) m_remoteAudioMuted = false;
    const float gain = m_remoteAudioMuted ? 0.0f : m_remoteAudioVolume;
    if (m_onRemoteAudioVolumeChanged) m_onRemoteAudioVolumeChanged(gain);
    saveConfig();
}

void UIManager::triggerRemoteAudioMuteChange(bool muted)
{
    m_remoteAudioMuted = muted;
    // Mute zeroes the effective gain but keeps m_remoteAudioVolume so
    // the slider position (and the value restored on unmute) survives.
    const float gain = m_remoteAudioMuted ? 0.0f : m_remoteAudioVolume;
    if (m_onRemoteAudioVolumeChanged) m_onRemoteAudioVolumeChanged(gain);
    saveConfig();
}

void UIManager::triggerStreamingMaxVideoBufferSizeChange(size_t size)
{
    m_streamingMaxVideoBufferSize = size;
    if (m_onStreamingMaxVideoBufferSizeChanged)
    {
        m_onStreamingMaxVideoBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingMaxAudioBufferSizeChange(size_t size)
{
    m_streamingMaxAudioBufferSize = size;
    if (m_onStreamingMaxAudioBufferSizeChanged)
    {
        m_onStreamingMaxAudioBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingMaxBufferTimeSecondsChange(int64_t seconds)
{
    m_streamingMaxBufferTimeSeconds = seconds;
    if (m_onStreamingMaxBufferTimeSecondsChanged)
    {
        m_onStreamingMaxBufferTimeSecondsChanged(seconds);
    }
    saveConfig();
}

void UIManager::triggerStreamingAVIOBufferSizeChange(size_t size)
{
    m_streamingAVIOBufferSize = size;
    if (m_onStreamingAVIOBufferSizeChanged)
    {
        m_onStreamingAVIOBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingStartStop(bool start)
{
    if (m_onStreamingStartStop)
    {
        m_onStreamingStartStop(start);
    }
}

void UIManager::triggerWebPortalEnabledChange(bool enabled)
{
    m_webPortalEnabled = enabled;
    if (!enabled && m_webPortalHTTPSEnabled)
    {
        m_webPortalHTTPSEnabled = false;
        if (m_onWebPortalHTTPSChanged)
        {
            m_onWebPortalHTTPSChanged(false);
        }
    }
    if (m_onWebPortalEnabledChanged)
    {
        m_onWebPortalEnabledChanged(enabled);
    }
    saveConfig();
}

void UIManager::triggerWebPortalHTTPSChange(bool enabled)
{
    m_webPortalHTTPSEnabled = enabled;
    if (m_onWebPortalHTTPSChanged)
    {
        m_onWebPortalHTTPSChanged(enabled);
    }
    saveConfig();
}

void UIManager::triggerWebPortalStartStop(bool start)
{
    m_webPortalActive = start;
    if (m_onWebPortalStartStop)
    {
        m_onWebPortalStartStop(start);
    }
}

void UIManager::triggerWebPortalTitleChange(const std::string &title)
{
    m_webPortalTitle = title;
    if (m_onWebPortalTitleChanged)
    {
        m_onWebPortalTitleChanged(title);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSubtitleChange(const std::string &subtitle)
{
    m_webPortalSubtitle = subtitle;
    if (m_onWebPortalSubtitleChanged)
    {
        m_onWebPortalSubtitleChanged(subtitle);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSSLCertPathChange(const std::string &path)
{
    m_webPortalSSLCertPath = path;
    if (m_onWebPortalSSLCertPathChanged)
    {
        m_onWebPortalSSLCertPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSSLKeyPathChange(const std::string &path)
{
    m_webPortalSSLKeyPath = path;
    if (m_onWebPortalSSLKeyPathChanged)
    {
        m_onWebPortalSSLKeyPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalBackgroundImagePathChange(const std::string &path)
{
    m_webPortalBackgroundImagePath = path;
    if (m_onWebPortalBackgroundImagePathChanged)
    {
        m_onWebPortalBackgroundImagePathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalColorsChange()
{
    if (m_onWebPortalColorsChanged)
    {
        m_onWebPortalColorsChanged();
    }
    saveConfig();
}

void UIManager::triggerWebPortalTextsChange()
{
    if (m_onWebPortalTextsChanged)
    {
        m_onWebPortalTextsChanged();
    }
    saveConfig();
}

void UIManager::scanShaders(const std::string &basePath)
{
    m_scannedShaders = ShaderScanner::scan(basePath);
    LOG_INFO("Encontrados " + std::to_string(m_scannedShaders.size()) + " shaders em " + basePath);
}

std::string UIManager::getConfigPath() const
{
    std::string dir = Paths::getUserConfigDir();
    if (!dir.empty())
    {
        return (fs::path(dir) / "config.json").string();
    }
    // Fallback: cwd. Não esperado em uso real (sem $HOME / sem %APPDATA%).
    return "retrocapture_config.json";
}

void UIManager::loadConfig()
{
    std::string configPath = getConfigPath();

    if (!fs::exists(configPath))
    {
        LOG_INFO("Configuration file not found: " + configPath + " (using defaults)");
        return;
    }

    try
    {
        std::ifstream file(configPath);
        if (!file.is_open())
        {
            LOG_WARN("Could not open config file: " + configPath);
            return;
        }

        nlohmann::json config;
        file >> config;
        file.close();

        // Carregar configurações de streaming
        if (config.contains("streaming"))
        {
            auto &streaming = config["streaming"];
            if (streaming.contains("port"))
                m_streamingPort = streaming["port"];
            if (streaming.contains("width"))
                m_streamingWidth = streaming["width"];
            if (streaming.contains("height"))
                m_streamingHeight = streaming["height"];
            if (streaming.contains("fps"))
                m_streamingFps = streaming["fps"];
            if (streaming.contains("bitrate"))
                m_streamingBitrate = streaming["bitrate"];
            if (streaming.contains("audioBitrate"))
                m_streamingAudioBitrate = streaming["audioBitrate"];
            if (streaming.contains("videoCodec"))
                m_streamingVideoCodec = streaming["videoCodec"].get<std::string>();
            if (streaming.contains("audioCodec"))
                m_streamingAudioCodec = streaming["audioCodec"].get<std::string>();
            if (streaming.contains("h264Preset"))
                m_streamingH264Preset = streaming["h264Preset"].get<std::string>();
            if (streaming.contains("h265Preset"))
                m_streamingH265Preset = streaming["h265Preset"].get<std::string>();
            if (streaming.contains("h265Profile"))
                m_streamingH265Profile = streaming["h265Profile"].get<std::string>();
            if (streaming.contains("h265Level"))
                m_streamingH265Level = streaming["h265Level"].get<std::string>();
            if (streaming.contains("vp8Speed"))
                m_streamingVP8Speed = streaming["vp8Speed"].get<int>();
            if (streaming.contains("vp9Speed"))
                m_streamingVP9Speed = streaming["vp9Speed"].get<int>();
            if (streaming.contains("hardwareEncoder"))
                m_streamingHardwareEncoder = streaming["hardwareEncoder"].get<int>();
            if (streaming.contains("nvencPreset"))
                m_streamingNvencPreset = streaming["nvencPreset"].get<std::string>();
            if (streaming.contains("vaapiRcMode"))
                m_streamingVaapiRcMode = streaming["vaapiRcMode"].get<std::string>();
            if (streaming.contains("qsvPreset"))
                m_streamingQsvPreset = streaming["qsvPreset"].get<std::string>();
            if (streaming.contains("amfQuality"))
                m_streamingAmfQuality = streaming["amfQuality"].get<std::string>();
            if (streaming.contains("remoteInterpolation"))
                m_remoteInterpolation = streaming["remoteInterpolation"].get<std::string>();

            // Carregar configurações de buffer
            if (streaming.contains("buffer"))
            {
                auto &buffer = streaming["buffer"];
                if (buffer.contains("maxVideoBufferSize"))
                    m_streamingMaxVideoBufferSize = buffer["maxVideoBufferSize"].get<size_t>();
                if (buffer.contains("maxAudioBufferSize"))
                    m_streamingMaxAudioBufferSize = buffer["maxAudioBufferSize"].get<size_t>();
                if (buffer.contains("maxBufferTimeSeconds"))
                    m_streamingMaxBufferTimeSeconds = buffer["maxBufferTimeSeconds"].get<int64_t>();
                if (buffer.contains("avioBufferSize"))
                    m_streamingAVIOBufferSize = buffer["avioBufferSize"].get<size_t>();
            }
            if (streaming.contains("directory"))
            {
                auto &dir = streaming["directory"];
                if (dir.contains("publishEnabled"))   m_directoryPublishEnabled   = dir["publishEnabled"].get<bool>();
                if (dir.contains("url"))              m_directoryUrl              = dir["url"].get<std::string>();
                if (dir.contains("insecureSkipVerify")) m_directoryInsecureSkipVerify = dir["insecureSkipVerify"].get<bool>();
                if (dir.contains("streamName"))       m_directoryStreamName       = dir["streamName"].get<std::string>();
                if (dir.contains("hostNickname"))     m_directoryHostNickname     = dir["hostNickname"].get<std::string>();
                if (dir.contains("password"))         m_directoryPassword         = dir["password"].get<std::string>();
                if (dir.contains("endpointMode"))     m_directoryEndpointMode     = dir["endpointMode"].get<std::string>();
                if (dir.contains("customEndpoint"))   m_directoryCustomEndpoint   = dir["customEndpoint"].get<std::string>();
                if (dir.contains("tunnelMode"))           m_directoryTunnelMode           = dir["tunnelMode"].get<std::string>();
                if (dir.contains("namedTunnelId"))        m_directoryNamedTunnelId        = dir["namedTunnelId"].get<std::string>();
                if (dir.contains("namedTunnelHostname"))  m_directoryNamedTunnelHostname  = dir["namedTunnelHostname"].get<std::string>();
                if (dir.contains("privacyAcked"))     m_directoryPrivacyAcked     = dir["privacyAcked"].get<bool>();
            }
            // Chat URL (#84) — sibling to the directory block.
            if (streaming.contains("chat"))
            {
                auto &chat = streaming["chat"];
                if (chat.contains("baseUrl"))  m_chatBaseUrl  = chat["baseUrl"].get<std::string>();
                if (chat.contains("nickname")) m_chatNickname = chat["nickname"].get<std::string>();
                if (chat.contains("streamChatEnabled"))
                    m_streamChatEnabled = chat["streamChatEnabled"].get<bool>();
                if (chat.contains("streamRoomTitle"))
                    m_streamRoomTitle   = chat["streamRoomTitle"].get<std::string>();
                if (chat.contains("streamRoomSlug"))
                    m_streamRoomSlug    = chat["streamRoomSlug"].get<std::string>();
            }
            // #85 — Virtual camera config, sibling block.
            if (streaming.contains("virtcam"))
            {
                auto &vc = streaming["virtcam"];
                if (vc.contains("enabled"))
                    m_virtcamEnabled      = vc["enabled"].get<bool>();
                if (vc.contains("devicePath"))
                    m_virtcamDevicePath   = vc["devicePath"].get<std::string>();
                if (vc.contains("outputWidth"))
                    m_virtcamOutputWidth  = vc["outputWidth"].get<uint32_t>();
                if (vc.contains("outputHeight"))
                    m_virtcamOutputHeight = vc["outputHeight"].get<uint32_t>();
                if (vc.contains("outputFps"))
                    m_virtcamOutputFps    = vc["outputFps"].get<uint32_t>();
                if (vc.contains("pixelFormat"))
                    m_virtcamPixelFormat  = vc["pixelFormat"].get<std::string>();
            }
            // #84 — One-shot migration: pre-rework configs stored
            // the directory's display nickname in a separate field
            // (m_directoryHostNickname). The chat Profile now owns
            // it; copy across when the chat nickname is empty so
            // legacy configs don't suddenly look "unnamed".
            if (m_chatNickname.empty() && !m_directoryHostNickname.empty())
            {
                m_chatNickname = m_directoryHostNickname;
            }
        }

        // Preferences (#45 placeholder + window restructure)
        if (config.contains("preferences"))
        {
            auto &prefs = config["preferences"];
            if (prefs.contains("language"))        m_language        = prefs["language"].get<std::string>();
            if (prefs.contains("startFullscreen")) m_startFullscreen = prefs["startFullscreen"].get<bool>();
            // #86 system tray / background operation.
            if (prefs.contains("trayEnabled"))         m_trayEnabled         = prefs["trayEnabled"].get<bool>();
            if (prefs.contains("trayMinimizeOnClose")) m_trayMinimizeOnClose = prefs["trayMinimizeOnClose"].get<bool>();
            if (prefs.contains("trayStartMinimized"))  m_trayStartMinimized  = prefs["trayStartMinimized"].get<bool>();
            if (prefs.contains("trayNotifications"))   m_trayNotifications   = prefs["trayNotifications"].get<bool>();
            // #68 — Quick actions widget visibility persists. Default
            // true so users discover the widget on first launch; once
            // they toggle it off via View, the choice survives across
            // runs.
            if (prefs.contains("quickActionsVisible"))
                m_quickActionsVisible = prefs["quickActionsVisible"].get<bool>();
            if (prefs.contains("quickActionsAutoHide"))
                m_quickActionsAutoHide = prefs["quickActionsAutoHide"].get<bool>();
            if (prefs.contains("shortcutsHelpVisible"))
                m_shortcutsHelpVisible = prefs["shortcutsHelpVisible"].get<bool>();
            // Chat overlay visibility (#84). Default true — same
            // discoverability story as the quick-actions widget.
            if (prefs.contains("chatOverlayVisible"))
                m_chatOverlayVisible = prefs["chatOverlayVisible"].get<bool>();
        }

        // Carregar configurações de captura
        if (config.contains("capture"))
        {
            auto &capture = config["capture"];
            if (capture.contains("width"))
                m_captureWidth = capture["width"].get<uint32_t>();
            if (capture.contains("height"))
                m_captureHeight = capture["height"].get<uint32_t>();
            if (capture.contains("fps"))
                m_captureFps = capture["fps"].get<uint32_t>();
            if (capture.contains("sourceOverscanX"))
                m_sourceOverscanPercentX = capture["sourceOverscanX"].get<float>();
            if (capture.contains("sourceOverscanY"))
                m_sourceOverscanPercentY = capture["sourceOverscanY"].get<float>();
            if (capture.contains("sourceOverscanLocked"))
                m_sourceOverscanLocked = capture["sourceOverscanLocked"].get<bool>();
        }

        // Carregar configurações de imagem
        if (config.contains("image"))
        {
            auto &image = config["image"];
            if (image.contains("brightness"))
                m_brightness = image["brightness"];
            if (image.contains("contrast"))
                m_contrast = image["contrast"];
            if (image.contains("maintainAspect"))
                m_maintainAspect = image["maintainAspect"];
            if (image.contains("fullscreen"))
                m_fullscreen = image["fullscreen"];
            if (image.contains("monitorIndex"))
                m_monitorIndex = image["monitorIndex"];
            if (image.contains("outputWidth"))
                m_outputWidth = image["outputWidth"].get<uint32_t>();
            if (image.contains("outputHeight"))
                m_outputHeight = image["outputHeight"].get<uint32_t>();
        }

        // Carregar configurações do Web Portal
        if (config.contains("webPortal"))
        {
            auto &webPortal = config["webPortal"];
            if (webPortal.contains("enabled"))
                m_webPortalEnabled = webPortal["enabled"];
            if (webPortal.contains("httpsEnabled"))
                m_webPortalHTTPSEnabled = webPortal["httpsEnabled"];
            if (webPortal.contains("sslCertPath"))
                m_webPortalSSLCertPath = webPortal["sslCertPath"].get<std::string>();
            if (webPortal.contains("sslKeyPath"))
                m_webPortalSSLKeyPath = webPortal["sslKeyPath"].get<std::string>();
            if (webPortal.contains("title"))
                m_webPortalTitle = webPortal["title"].get<std::string>();
            if (webPortal.contains("subtitle"))
                m_webPortalSubtitle = webPortal["subtitle"].get<std::string>();
            if (webPortal.contains("imagePath"))
                m_webPortalImagePath = webPortal["imagePath"].get<std::string>();
            if (webPortal.contains("backgroundImagePath"))
                m_webPortalBackgroundImagePath = webPortal["backgroundImagePath"].get<std::string>();

            // Carregar textos editáveis
            if (webPortal.contains("texts"))
            {
                auto &texts = webPortal["texts"];
                if (texts.contains("streamInfo"))
                    m_webPortalTextStreamInfo = texts["streamInfo"].get<std::string>();
                if (texts.contains("quickActions"))
                    m_webPortalTextQuickActions = texts["quickActions"].get<std::string>();
                if (texts.contains("compatibility"))
                    m_webPortalTextCompatibility = texts["compatibility"].get<std::string>();
                if (texts.contains("status"))
                    m_webPortalTextStatus = texts["status"].get<std::string>();
                if (texts.contains("codec"))
                    m_webPortalTextCodec = texts["codec"].get<std::string>();
                if (texts.contains("resolution"))
                    m_webPortalTextResolution = texts["resolution"].get<std::string>();
                if (texts.contains("streamUrl"))
                    m_webPortalTextStreamUrl = texts["streamUrl"].get<std::string>();
                if (texts.contains("copyUrl"))
                    m_webPortalTextCopyUrl = texts["copyUrl"].get<std::string>();
                if (texts.contains("openNewTab"))
                    m_webPortalTextOpenNewTab = texts["openNewTab"].get<std::string>();
                if (texts.contains("supported"))
                    m_webPortalTextSupported = texts["supported"].get<std::string>();
                if (texts.contains("format"))
                    m_webPortalTextFormat = texts["format"].get<std::string>();
                if (texts.contains("codecInfo"))
                    m_webPortalTextCodecInfo = texts["codecInfo"].get<std::string>();
                if (texts.contains("supportedBrowsers"))
                    m_webPortalTextSupportedBrowsers = texts["supportedBrowsers"].get<std::string>();
                if (texts.contains("formatInfo"))
                    m_webPortalTextFormatInfo = texts["formatInfo"].get<std::string>();
                if (texts.contains("codecInfoValue"))
                    m_webPortalTextCodecInfoValue = texts["codecInfoValue"].get<std::string>();
                if (texts.contains("connecting"))
                    m_webPortalTextConnecting = texts["connecting"].get<std::string>();
            }

            // Carregar cores
            if (webPortal.contains("colors"))
            {
                auto &colors = webPortal["colors"];
                if (colors.contains("background"))
                {
                    auto &bg = colors["background"];
                    m_webPortalColorBackground[0] = bg[0];
                    m_webPortalColorBackground[1] = bg[1];
                    m_webPortalColorBackground[2] = bg[2];
                    m_webPortalColorBackground[3] = bg[3];
                }
                if (colors.contains("text"))
                {
                    auto &txt = colors["text"];
                    m_webPortalColorText[0] = txt[0];
                    m_webPortalColorText[1] = txt[1];
                    m_webPortalColorText[2] = txt[2];
                    m_webPortalColorText[3] = txt[3];
                }
                if (colors.contains("primary"))
                {
                    auto &prim = colors["primary"];
                    m_webPortalColorPrimary[0] = prim[0];
                    m_webPortalColorPrimary[1] = prim[1];
                    m_webPortalColorPrimary[2] = prim[2];
                    m_webPortalColorPrimary[3] = prim[3];
                }
                if (colors.contains("primaryLight"))
                {
                    auto &primLight = colors["primaryLight"];
                    m_webPortalColorPrimaryLight[0] = primLight[0];
                    m_webPortalColorPrimaryLight[1] = primLight[1];
                    m_webPortalColorPrimaryLight[2] = primLight[2];
                    m_webPortalColorPrimaryLight[3] = primLight[3];
                }
                if (colors.contains("primaryDark"))
                {
                    auto &primDark = colors["primaryDark"];
                    m_webPortalColorPrimaryDark[0] = primDark[0];
                    m_webPortalColorPrimaryDark[1] = primDark[1];
                    m_webPortalColorPrimaryDark[2] = primDark[2];
                    m_webPortalColorPrimaryDark[3] = primDark[3];
                }
                if (colors.contains("secondary"))
                {
                    auto &sec = colors["secondary"];
                    m_webPortalColorSecondary[0] = sec[0];
                    m_webPortalColorSecondary[1] = sec[1];
                    m_webPortalColorSecondary[2] = sec[2];
                    m_webPortalColorSecondary[3] = sec[3];
                }
                if (colors.contains("secondaryHighlight"))
                {
                    auto &secHighlight = colors["secondaryHighlight"];
                    m_webPortalColorSecondaryHighlight[0] = secHighlight[0];
                    m_webPortalColorSecondaryHighlight[1] = secHighlight[1];
                    m_webPortalColorSecondaryHighlight[2] = secHighlight[2];
                    m_webPortalColorSecondaryHighlight[3] = secHighlight[3];
                }
                if (colors.contains("cardHeader"))
                {
                    auto &ch = colors["cardHeader"];
                    m_webPortalColorCardHeader[0] = ch[0];
                    m_webPortalColorCardHeader[1] = ch[1];
                    m_webPortalColorCardHeader[2] = ch[2];
                    m_webPortalColorCardHeader[3] = ch[3];
                }
                if (colors.contains("border"))
                {
                    auto &b = colors["border"];
                    m_webPortalColorBorder[0] = b[0];
                    m_webPortalColorBorder[1] = b[1];
                    m_webPortalColorBorder[2] = b[2];
                    m_webPortalColorBorder[3] = b[3];
                }
                if (colors.contains("success"))
                {
                    auto &s = colors["success"];
                    m_webPortalColorSuccess[0] = s[0];
                    m_webPortalColorSuccess[1] = s[1];
                    m_webPortalColorSuccess[2] = s[2];
                    m_webPortalColorSuccess[3] = s[3];
                }
                if (colors.contains("warning"))
                {
                    auto &w = colors["warning"];
                    m_webPortalColorWarning[0] = w[0];
                    m_webPortalColorWarning[1] = w[1];
                    m_webPortalColorWarning[2] = w[2];
                    m_webPortalColorWarning[3] = w[3];
                }
                if (colors.contains("danger"))
                {
                    auto &d = colors["danger"];
                    m_webPortalColorDanger[0] = d[0];
                    m_webPortalColorDanger[1] = d[1];
                    m_webPortalColorDanger[2] = d[2];
                    m_webPortalColorDanger[3] = d[3];
                }
                if (colors.contains("info"))
                {
                    auto &inf = colors["info"];
                    m_webPortalColorInfo[0] = inf[0];
                    m_webPortalColorInfo[1] = inf[1];
                    m_webPortalColorInfo[2] = inf[2];
                    m_webPortalColorInfo[3] = inf[3];
                }
            }
        }

        // Carregar shader atual
        if (config.contains("shader"))
        {
            auto &shader = config["shader"];
            if (shader.contains("current") && !shader["current"].is_null())
            {
                m_currentShader = shader["current"].get<std::string>();
            }
            if (shader.contains("pipelineEnabled"))
            {
                m_shaderPipelineEnabled = shader["pipelineEnabled"].get<bool>();
            }
        }

        // Carregar configurações de fonte
        if (config.contains("source"))
        {
            auto &source = config["source"];
            if (source.contains("type"))
            {
                int sourceTypeInt = source["type"].get<int>();
                SourceType loaded  = static_cast<SourceType>(sourceTypeInt);
                // Coerce platform-foreign source types to the local
                // platform's native one: a config saved on Linux
                // with V4L2 shouldn't try to instantiate V4L2 on
                // macOS, and so on. Remote and None always travel.
                const bool isPlatformNative =
                    loaded == SourceType::None ||
                    loaded == SourceType::Remote ||
                    loaded == SourceType::Screen ||   // #107 cross-platform
#ifdef __linux__
                    loaded == SourceType::V4L2;
#elif defined(_WIN32)
                    loaded == SourceType::DS;
#elif defined(__APPLE__)
                    loaded == SourceType::AVFoundation;
#else
                    false;
#endif
                if (isPlatformNative)
                {
                    m_sourceType = loaded;
                }
                // else: keep the platform-default that the
                // constructor / default-init set.
            }
        }

        // Carregar dispositivo V4L2
        if (config.contains("v4l2"))
        {
            auto &v4l2 = config["v4l2"];
            if (v4l2.contains("device") && !v4l2["device"].is_null())
            {
                m_currentDevice = v4l2["device"].get<std::string>();
            }
        }

        // Carregar dispositivo DirectShow
        if (config.contains("directshow"))
        {
            auto &ds = config["directshow"];
            if (ds.contains("device") && !ds["device"].is_null())
            {
                m_currentDevice = ds["device"].get<std::string>();
            }
        }

        // #107 — screen-capture target + region crop. Region always
        // round-trips; the target only seeds m_currentDevice when the
        // active source is Screen (m_currentDevice is shared and the
        // v4l2/ds blocks above also write it).
        if (config.contains("screen"))
        {
            auto &screen = config["screen"];
            if (m_sourceType == SourceType::Screen &&
                screen.contains("target") && !screen["target"].is_null())
            {
                m_currentDevice = screen["target"].get<std::string>();
            }
            auto getU = [&](const char *k, uint32_t &dst) {
                if (screen.contains(k) && screen[k].is_number_unsigned())
                    dst = screen[k].get<uint32_t>();
            };
            getU("regionX", m_screenRegionX);
            getU("regionY", m_screenRegionY);
            getU("regionW", m_screenRegionW);
            getU("regionH", m_screenRegionH);
        }

        // Carregar configurações de áudio
        if (config.contains("audio"))
        {
            auto &audio = config["audio"];
            if (audio.contains("inputSourceId") && !audio["inputSourceId"].is_null())
            {
                m_audioInputSourceId = audio["inputSourceId"].get<std::string>();
            }
            // #77 client-side remote audio volume + mute.
            if (audio.contains("remoteVolume") && audio["remoteVolume"].is_number())
            {
                m_remoteAudioVolume = audio["remoteVolume"].get<float>();
                if (m_remoteAudioVolume < 0.0f) m_remoteAudioVolume = 0.0f;
                if (m_remoteAudioVolume > 1.0f) m_remoteAudioVolume = 1.0f;
            }
            if (audio.contains("remoteMuted") && audio["remoteMuted"].is_boolean())
            {
                m_remoteAudioMuted = audio["remoteMuted"].get<bool>();
            }
        }

        // AVFoundation persistence (macOS device + format selection).
        // Loaded on every platform so the JSON round-trips cleanly;
        // ignored on non-macOS builds where the fields don't drive
        // anything.
        if (config.contains("avfoundation"))
        {
            auto &avf = config["avfoundation"];
            if (avf.contains("deviceId") && !avf["deviceId"].is_null())
            {
                m_avfDeviceId = avf["deviceId"].get<std::string>();
            }
            if (avf.contains("formatId") && !avf["formatId"].is_null())
            {
                m_avfFormatId = avf["formatId"].get<std::string>();
            }
            if (avf.contains("audioDeviceId") && !avf["audioDeviceId"].is_null())
            {
                m_avfAudioDeviceId = avf["audioDeviceId"].get<std::string>();
            }
        }

        // Carregar configurações de gravação
        if (config.contains("recording"))
        {
            auto &recording = config["recording"];
            if (recording.contains("width"))
                m_recordingWidth = recording["width"].get<uint32_t>();
            if (recording.contains("height"))
                m_recordingHeight = recording["height"].get<uint32_t>();
            if (recording.contains("fps"))
                m_recordingFps = recording["fps"].get<uint32_t>();
            if (recording.contains("bitrate"))
                m_recordingBitrate = recording["bitrate"].get<uint32_t>();
            if (recording.contains("audioBitrate"))
                m_recordingAudioBitrate = recording["audioBitrate"].get<uint32_t>();
            if (recording.contains("videoCodec"))
                m_recordingVideoCodec = recording["videoCodec"].get<std::string>();
            if (recording.contains("audioCodec"))
                m_recordingAudioCodec = recording["audioCodec"].get<std::string>();
            if (recording.contains("h264Preset"))
                m_recordingH264Preset = recording["h264Preset"].get<std::string>();
            if (recording.contains("h265Preset"))
                m_recordingH265Preset = recording["h265Preset"].get<std::string>();
            if (recording.contains("h265Profile"))
                m_recordingH265Profile = recording["h265Profile"].get<std::string>();
            if (recording.contains("h265Level"))
                m_recordingH265Level = recording["h265Level"].get<std::string>();
            if (recording.contains("vp8Speed"))
                m_recordingVP8Speed = recording["vp8Speed"].get<int>();
            if (recording.contains("vp9Speed"))
                m_recordingVP9Speed = recording["vp9Speed"].get<int>();
            if (recording.contains("container"))
                m_recordingContainer = recording["container"].get<std::string>();
            if (recording.contains("outputPath"))
                m_recordingOutputPath = recording["outputPath"].get<std::string>();
            if (recording.contains("filenameTemplate"))
                m_recordingFilenameTemplate = recording["filenameTemplate"].get<std::string>();
            if (recording.contains("includeAudio"))
                m_recordingIncludeAudio = recording["includeAudio"];
            if (recording.contains("applyShader"))
                m_recordingApplyShader = recording["applyShader"].get<bool>();
            if (recording.contains("hardwareEncoder"))
                m_recordingHardwareEncoder = recording["hardwareEncoder"].get<int>();
            if (recording.contains("nvencPreset"))
                m_recordingNvencPreset = recording["nvencPreset"].get<std::string>();
            if (recording.contains("vaapiRcMode"))
                m_recordingVaapiRcMode = recording["vaapiRcMode"].get<std::string>();
            if (recording.contains("qsvPreset"))
                m_recordingQsvPreset = recording["qsvPreset"].get<std::string>();
            if (recording.contains("amfQuality"))
                m_recordingAmfQuality = recording["amfQuality"].get<std::string>();
        }

        if (config.contains("streaming"))
        {
            auto &streaming = config["streaming"];
            if (streaming.contains("applyShader"))
                m_streamingApplyShader = streaming["applyShader"].get<bool>();
        }

        LOG_INFO("Configuration loaded from: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error loading configuration: " + std::string(e.what()));
    }
}

void UIManager::saveConfig()
{
    std::string configPath = getConfigPath();

    try
    {
        nlohmann::json config;

        // Salvar configurações de streaming
        config["streaming"] = {
            {"port", m_streamingPort},
            {"width", m_streamingWidth},
            {"height", m_streamingHeight},
            {"fps", m_streamingFps},
            {"bitrate", m_streamingBitrate},
            {"audioBitrate", m_streamingAudioBitrate},
            {"videoCodec", m_streamingVideoCodec},
            {"audioCodec", m_streamingAudioCodec},
            {"h264Preset", m_streamingH264Preset},
            {"h265Preset", m_streamingH265Preset},
            {"h265Profile", m_streamingH265Profile},
            {"h265Level", m_streamingH265Level},
            {"vp8Speed", m_streamingVP8Speed},
            {"vp9Speed", m_streamingVP9Speed},
            {"hardwareEncoder", m_streamingHardwareEncoder},
            {"nvencPreset", m_streamingNvencPreset},
            {"vaapiRcMode", m_streamingVaapiRcMode},
            {"qsvPreset",   m_streamingQsvPreset},
            {"amfQuality",  m_streamingAmfQuality},
            {"remoteInterpolation", m_remoteInterpolation},
            {"applyShader", m_streamingApplyShader},
            {"buffer", {{"maxVideoBufferSize", m_streamingMaxVideoBufferSize}, {"maxAudioBufferSize", m_streamingMaxAudioBufferSize}, {"maxBufferTimeSeconds", m_streamingMaxBufferTimeSeconds}, {"avioBufferSize", m_streamingAVIOBufferSize}}},
            // #49 Phase 2: public directory publish settings.
            // The password is persisted because the user re-uses it
            // across sessions; the runtime streamId + ownerToken are
            // never persisted (per the spec — a new run is a new
            // directory entry).
            {"directory", {
                {"publishEnabled", m_directoryPublishEnabled},
                {"url",            m_directoryUrl},
                {"insecureSkipVerify", m_directoryInsecureSkipVerify},
                {"streamName",     m_directoryStreamName},
                {"hostNickname",   m_directoryHostNickname},
                {"password",       m_directoryPassword},
                {"endpointMode",        m_directoryEndpointMode},
                {"customEndpoint",      m_directoryCustomEndpoint},
                {"tunnelMode",          m_directoryTunnelMode},
                {"namedTunnelId",       m_directoryNamedTunnelId},
                {"namedTunnelHostname", m_directoryNamedTunnelHostname},
                {"privacyAcked",   m_directoryPrivacyAcked},
            }},
            // Chat service URL + persistent nickname (#84).
            {"chat", {
                {"baseUrl",            m_chatBaseUrl},
                {"nickname",           m_chatNickname},
                {"streamChatEnabled",  m_streamChatEnabled},
                {"streamRoomTitle",    m_streamRoomTitle},
                {"streamRoomSlug",     m_streamRoomSlug},
            }},
            // Virtual camera (#85).
            {"virtcam", {
                {"enabled",      m_virtcamEnabled},
                {"devicePath",   m_virtcamDevicePath},
                {"outputWidth",  m_virtcamOutputWidth},
                {"outputHeight", m_virtcamOutputHeight},
                {"outputFps",    m_virtcamOutputFps},
                {"pixelFormat",  m_virtcamPixelFormat},
            }}};

        // Preferences (#45 placeholder + window restructure)
        config["preferences"] = {
            {"language",        m_language},
            {"startFullscreen", m_startFullscreen},
            {"trayEnabled",         m_trayEnabled},
            {"trayMinimizeOnClose", m_trayMinimizeOnClose},
            {"trayStartMinimized",  m_trayStartMinimized},
            {"trayNotifications",   m_trayNotifications},
            {"quickActionsVisible",
             m_quickActionsOverlay ? m_quickActionsOverlay->isVisible()
                                  : m_quickActionsVisible},
            {"quickActionsAutoHide", m_quickActionsAutoHide},
            {"shortcutsHelpVisible",
             m_shortcutsHelpWindow ? m_shortcutsHelpWindow->isVisible()
                                   : m_shortcutsHelpVisible},
            {"chatOverlayVisible",
             m_chatOverlay ? m_chatOverlay->isVisible()
                           : m_chatOverlayVisible},
        };

        // Salvar configurações de imagem
        config["image"] = {
            {"brightness", m_brightness},
            {"contrast", m_contrast},
            {"maintainAspect", m_maintainAspect},
            {"fullscreen", m_fullscreen},
            {"monitorIndex", m_monitorIndex},
            {"outputWidth", m_outputWidth},
            {"outputHeight", m_outputHeight}};

        // Salvar configurações do Web Portal
        config["webPortal"] = {
            {"enabled", m_webPortalEnabled},
            {"httpsEnabled", m_webPortalHTTPSEnabled},
            {"sslCertPath", m_webPortalSSLCertPath},
            {"sslKeyPath", m_webPortalSSLKeyPath},
            {"title", m_webPortalTitle},
            {"subtitle", m_webPortalSubtitle},
            {"imagePath", m_webPortalImagePath},
            {"backgroundImagePath", m_webPortalBackgroundImagePath},
            {"texts", {{"streamInfo", m_webPortalTextStreamInfo}, {"quickActions", m_webPortalTextQuickActions}, {"compatibility", m_webPortalTextCompatibility}, {"status", m_webPortalTextStatus}, {"codec", m_webPortalTextCodec}, {"resolution", m_webPortalTextResolution}, {"streamUrl", m_webPortalTextStreamUrl}, {"copyUrl", m_webPortalTextCopyUrl}, {"openNewTab", m_webPortalTextOpenNewTab}, {"supported", m_webPortalTextSupported}, {"format", m_webPortalTextFormat}, {"codecInfo", m_webPortalTextCodecInfo}, {"supportedBrowsers", m_webPortalTextSupportedBrowsers}, {"formatInfo", m_webPortalTextFormatInfo}, {"codecInfoValue", m_webPortalTextCodecInfoValue}, {"connecting", m_webPortalTextConnecting}}},
            {"colors", {{"background", {m_webPortalColorBackground[0], m_webPortalColorBackground[1], m_webPortalColorBackground[2], m_webPortalColorBackground[3]}}, {"text", {m_webPortalColorText[0], m_webPortalColorText[1], m_webPortalColorText[2], m_webPortalColorText[3]}}, {"primary", {m_webPortalColorPrimary[0], m_webPortalColorPrimary[1], m_webPortalColorPrimary[2], m_webPortalColorPrimary[3]}}, {"primaryLight", {m_webPortalColorPrimaryLight[0], m_webPortalColorPrimaryLight[1], m_webPortalColorPrimaryLight[2], m_webPortalColorPrimaryLight[3]}}, {"primaryDark", {m_webPortalColorPrimaryDark[0], m_webPortalColorPrimaryDark[1], m_webPortalColorPrimaryDark[2], m_webPortalColorPrimaryDark[3]}}, {"secondary", {m_webPortalColorSecondary[0], m_webPortalColorSecondary[1], m_webPortalColorSecondary[2], m_webPortalColorSecondary[3]}}, {"secondaryHighlight", {m_webPortalColorSecondaryHighlight[0], m_webPortalColorSecondaryHighlight[1], m_webPortalColorSecondaryHighlight[2], m_webPortalColorSecondaryHighlight[3]}}, {"cardHeader", {m_webPortalColorCardHeader[0], m_webPortalColorCardHeader[1], m_webPortalColorCardHeader[2], m_webPortalColorCardHeader[3]}}, {"border", {m_webPortalColorBorder[0], m_webPortalColorBorder[1], m_webPortalColorBorder[2], m_webPortalColorBorder[3]}}, {"success", {m_webPortalColorSuccess[0], m_webPortalColorSuccess[1], m_webPortalColorSuccess[2], m_webPortalColorSuccess[3]}}, {"warning", {m_webPortalColorWarning[0], m_webPortalColorWarning[1], m_webPortalColorWarning[2], m_webPortalColorWarning[3]}}, {"danger", {m_webPortalColorDanger[0], m_webPortalColorDanger[1], m_webPortalColorDanger[2], m_webPortalColorDanger[3]}}, {"info", {m_webPortalColorInfo[0], m_webPortalColorInfo[1], m_webPortalColorInfo[2], m_webPortalColorInfo[3]}}}}};

        // Salvar configurações de captura
        config["capture"] = {
            {"width", m_captureWidth},
            {"height", m_captureHeight},
            {"fps", m_captureFps},
            {"sourceOverscanX", m_sourceOverscanPercentX},
            {"sourceOverscanY", m_sourceOverscanPercentY},
            {"sourceOverscanLocked", m_sourceOverscanLocked}};

        // Salvar shader atual
        config["shader"] = {
            {"current", m_currentShader.empty() ? "" : m_currentShader},
            {"pipelineEnabled", m_shaderPipelineEnabled}};

        // Salvar configurações de fonte
        config["source"] = {
            {"type", static_cast<int>(m_sourceType)}};

        // Salvar dispositivo V4L2
        config["v4l2"] = {
            {"device", m_currentDevice.empty() ? "" : m_currentDevice}};

        // Salvar dispositivo DirectShow
        config["directshow"] = {
            {"device", m_currentDevice.empty() ? "" : m_currentDevice}};

        // #107 — screen-capture target + region crop.
        config["screen"] = {
            {"target",  (m_sourceType == SourceType::Screen) ? m_currentDevice : std::string()},
            {"regionX", m_screenRegionX},
            {"regionY", m_screenRegionY},
            {"regionW", m_screenRegionW},
            {"regionH", m_screenRegionH}};

        // Salvar configurações de áudio
        config["audio"] = {
            {"inputSourceId", m_audioInputSourceId.empty() ? "" : m_audioInputSourceId},
            {"remoteVolume", m_remoteAudioVolume},
            {"remoteMuted", m_remoteAudioMuted}};

        // AVFoundation device + format selection (macOS).
        config["avfoundation"] = {
            {"deviceId",      m_avfDeviceId},
            {"formatId",      m_avfFormatId},
            {"audioDeviceId", m_avfAudioDeviceId}};

        // Salvar configurações de gravação
        config["recording"] = {
            {"width", m_recordingWidth},
            {"height", m_recordingHeight},
            {"fps", m_recordingFps},
            {"bitrate", m_recordingBitrate},
            {"audioBitrate", m_recordingAudioBitrate},
            {"videoCodec", m_recordingVideoCodec},
            {"audioCodec", m_recordingAudioCodec},
            {"h264Preset", m_recordingH264Preset},
            {"h265Preset", m_recordingH265Preset},
            {"h265Profile", m_recordingH265Profile},
            {"h265Level", m_recordingH265Level},
            {"vp8Speed", m_recordingVP8Speed},
            {"vp9Speed", m_recordingVP9Speed},
            {"container", m_recordingContainer},
            {"outputPath", m_recordingOutputPath},
            {"filenameTemplate", m_recordingFilenameTemplate},
            {"includeAudio", m_recordingIncludeAudio},
            {"applyShader", m_recordingApplyShader},
            {"hardwareEncoder", m_recordingHardwareEncoder},
            {"nvencPreset", m_recordingNvencPreset},
            {"vaapiRcMode", m_recordingVaapiRcMode},
            {"qsvPreset", m_recordingQsvPreset},
            {"amfQuality", m_recordingAmfQuality}};

        // Escrever arquivo
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            LOG_WARN("Could not create configuration file: " + configPath);
            return;
        }

        file << config.dump(4); // Indentação de 4 espaços para legibilidade
        file.close();

        LOG_INFO("Configuration saved to: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error saving configuration: " + std::string(e.what()));
    }
}

void UIManager::renderWebPortalPanel()
{
    ImGui::Text("Web Portal");
    ImGui::Separator();
    ImGui::Spacing();

    // Web Portal Enable/Disable (configuração)
    bool portalEnabled = m_webPortalEnabled;
    if (ImGui::Checkbox("Enable Web Portal", &portalEnabled))
    {
        m_webPortalEnabled = portalEnabled;
        if (!portalEnabled && m_webPortalHTTPSEnabled)
        {
            m_webPortalHTTPSEnabled = false;
            if (m_onWebPortalHTTPSChanged)
            {
                m_onWebPortalHTTPSChanged(false);
            }
        }
        if (m_onWebPortalEnabledChanged)
        {
            m_onWebPortalEnabledChanged(portalEnabled);
        }
        saveConfig();
    }

    if (!portalEnabled)
    {
        ImGui::Spacing();
        std::string streamUrl = "http://localhost:" + std::to_string(m_streamingPort) + "/stream";
        ImGui::Text("Stream direto: %s", streamUrl.c_str());
        return;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Botão Start/Stop do Portal Web (independente do streaming)
    if (m_webPortalActive)
    {
        if (ImGui::Button("Stop Web Portal", ImVec2(-1, 0)))
        {
            m_webPortalActive = false;
            if (m_onWebPortalStartStop)
            {
                m_onWebPortalStartStop(false);
            }
        }
        ImGui::Spacing();
        std::string portalUrl = (m_webPortalHTTPSEnabled ? "https://" : "http://") +
                                std::string("localhost:") + std::to_string(m_streamingPort);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Portal Web Ativo");
        ImGui::Text("URL: %s", portalUrl.c_str());
    }
    else
    {
        if (ImGui::Button("Start Web Portal", ImVec2(-1, 0)))
        {
            m_webPortalActive = true;
            if (m_onWebPortalStartStop)
            {
                m_onWebPortalStartStop(true);
            }
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Portal Web Inativo");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // HTTPS Enable/Disable
    bool httpsEnabled = m_webPortalHTTPSEnabled;
    if (ImGui::Checkbox("Enable HTTPS", &httpsEnabled))
    {
        m_webPortalHTTPSEnabled = httpsEnabled;
        if (m_onWebPortalHTTPSChanged)
        {
            m_onWebPortalHTTPSChanged(httpsEnabled);
        }
        saveConfig();
    }

    if (httpsEnabled)
    {
        ImGui::Spacing();

        if (!m_foundSSLCertPath.empty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ HTTPS Ativo");
            ImGui::Text("Certificado: %s", m_foundSSLCertPath.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Certificate not found");
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Certificate Settings"))
        {
            char certPathBuffer[512];
            strncpy(certPathBuffer, m_webPortalSSLCertPath.c_str(), sizeof(certPathBuffer) - 1);
            certPathBuffer[sizeof(certPathBuffer) - 1] = '\0';

            ImGui::Text("Caminho do Certificado:");
            if (ImGui::InputText("##SSLCertPath", certPathBuffer, sizeof(certPathBuffer)))
            {
                m_webPortalSSLCertPath = std::string(certPathBuffer);
                if (m_onWebPortalSSLCertPathChanged)
                {
                    m_onWebPortalSSLCertPathChanged(m_webPortalSSLCertPath);
                }
                saveConfig();
            }

            char keyPathBuffer[512];
            strncpy(keyPathBuffer, m_webPortalSSLKeyPath.c_str(), sizeof(keyPathBuffer) - 1);
            keyPathBuffer[sizeof(keyPathBuffer) - 1] = '\0';

            ImGui::Text("Caminho da Chave Privada:");
            if (ImGui::InputText("##SSLKeyPath", keyPathBuffer, sizeof(keyPathBuffer)))
            {
                m_webPortalSSLKeyPath = std::string(keyPathBuffer);
                if (m_onWebPortalSSLKeyPathChanged)
                {
                    m_onWebPortalSSLKeyPathChanged(m_webPortalSSLKeyPath);
                }
                saveConfig();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Personalização
    ImGui::Text("Customization");
    ImGui::Separator();
    ImGui::Spacing();

    // Título
    char titleBuffer[256];
    strncpy(titleBuffer, m_webPortalTitle.c_str(), sizeof(titleBuffer) - 1);
    titleBuffer[sizeof(titleBuffer) - 1] = '\0';
    ImGui::Text("Title:");
    if (ImGui::InputText("##WebPortalTitle", titleBuffer, sizeof(titleBuffer)))
    {
        m_webPortalTitle = std::string(titleBuffer);
        if (m_onWebPortalTitleChanged)
        {
            m_onWebPortalTitleChanged(m_webPortalTitle);
        }
        saveConfig();
    }

    ImGui::Spacing();

    // Subtítulo
    char subtitleBuffer[256];
    strncpy(subtitleBuffer, m_webPortalSubtitle.c_str(), sizeof(subtitleBuffer) - 1);
    subtitleBuffer[sizeof(subtitleBuffer) - 1] = '\0';
    ImGui::Text("Subtitle:");
    if (ImGui::InputText("##WebPortalSubtitle", subtitleBuffer, sizeof(subtitleBuffer)))
    {
        m_webPortalSubtitle = std::string(subtitleBuffer);
        if (m_onWebPortalSubtitleChanged)
        {
            m_onWebPortalSubtitleChanged(m_webPortalSubtitle);
        }
        saveConfig();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Configurações avançadas (colapsável)
    if (ImGui::CollapsingHeader("Advanced"))
    {
        ImGui::Spacing();

        // Imagem de fundo
        char bgImagePathBuffer[512];
        strncpy(bgImagePathBuffer, m_webPortalBackgroundImagePath.c_str(), sizeof(bgImagePathBuffer) - 1);
        bgImagePathBuffer[sizeof(bgImagePathBuffer) - 1] = '\0';
        ImGui::Text("Imagem de Fundo:");
        if (ImGui::InputText("##WebPortalBackgroundImagePath", bgImagePathBuffer, sizeof(bgImagePathBuffer)))
        {
            m_webPortalBackgroundImagePath = std::string(bgImagePathBuffer);
            if (m_onWebPortalBackgroundImagePathChanged)
            {
                m_onWebPortalBackgroundImagePathChanged(m_webPortalBackgroundImagePath);
            }
            saveConfig();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Cores
        ImGui::Text("Cores:");
        ImGui::Spacing();

        bool colorsChanged = false;

        if (ImGui::ColorEdit4("Fundo", m_webPortalColorBackground, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Texto", m_webPortalColorText, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primary", m_webPortalColorPrimary, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primary Light", m_webPortalColorPrimaryLight, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primary Dark", m_webPortalColorPrimaryDark, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Secondary", m_webPortalColorSecondary, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Secondary Highlight", m_webPortalColorSecondaryHighlight, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Header", m_webPortalColorCardHeader, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Bordas", m_webPortalColorBorder, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        ImGui::Spacing();

        if (ImGui::ColorEdit4("Sucesso", m_webPortalColorSuccess, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Aviso", m_webPortalColorWarning, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Erro", m_webPortalColorDanger, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Info", m_webPortalColorInfo, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (colorsChanged)
        {
            if (m_onWebPortalColorsChanged)
            {
                m_onWebPortalColorsChanged();
            }
            saveConfig();
        }

        ImGui::Spacing();
        if (ImGui::Button("Reset to Default Colors"))
        {
            // Restaurar valores padrão do styleguide RetroCapture
            // Dark Background #1D1F21
            m_webPortalColorBackground[0] = 0.114f;
            m_webPortalColorBackground[1] = 0.122f;
            m_webPortalColorBackground[2] = 0.129f;
            m_webPortalColorBackground[3] = 1.0f;
            // Text Light #F8F8F2
            m_webPortalColorText[0] = 0.973f;
            m_webPortalColorText[1] = 0.973f;
            m_webPortalColorText[2] = 0.949f;
            m_webPortalColorText[3] = 1.0f;
            // Primary - Retro Teal #0A7A83
            m_webPortalColorPrimary[0] = 0.039f;
            m_webPortalColorPrimary[1] = 0.478f;
            m_webPortalColorPrimary[2] = 0.514f;
            m_webPortalColorPrimary[3] = 1.0f;
            // Primary Light - Mint Screen Glow #6FC4C0
            m_webPortalColorPrimaryLight[0] = 0.435f;
            m_webPortalColorPrimaryLight[1] = 0.769f;
            m_webPortalColorPrimaryLight[2] = 0.753f;
            m_webPortalColorPrimaryLight[3] = 1.0f;
            // Primary Dark - Deep Retro #0F3E42
            m_webPortalColorPrimaryDark[0] = 0.059f;
            m_webPortalColorPrimaryDark[1] = 0.243f;
            m_webPortalColorPrimaryDark[2] = 0.259f;
            m_webPortalColorPrimaryDark[3] = 1.0f;
            // Secondary - Cyan Oscilloscope #47B3CE
            m_webPortalColorSecondary[0] = 0.278f;
            m_webPortalColorSecondary[1] = 0.702f;
            m_webPortalColorSecondary[2] = 0.808f;
            m_webPortalColorSecondary[3] = 1.0f;
            // Secondary Highlight - Phosphor Glow #C9F2E7
            m_webPortalColorSecondaryHighlight[0] = 0.788f;
            m_webPortalColorSecondaryHighlight[1] = 0.949f;
            m_webPortalColorSecondaryHighlight[2] = 0.906f;
            m_webPortalColorSecondaryHighlight[3] = 1.0f;
            // Card Header (usa Primary Dark)
            m_webPortalColorCardHeader[0] = 0.059f;
            m_webPortalColorCardHeader[1] = 0.243f;
            m_webPortalColorCardHeader[2] = 0.259f;
            m_webPortalColorCardHeader[3] = 1.0f;
            // Border (usa Primary com transparência)
            m_webPortalColorBorder[0] = 0.039f;
            m_webPortalColorBorder[1] = 0.478f;
            m_webPortalColorBorder[2] = 0.514f;
            m_webPortalColorBorder[3] = 0.5f;
            // Success #45D6A4
            m_webPortalColorSuccess[0] = 0.271f;
            m_webPortalColorSuccess[1] = 0.839f;
            m_webPortalColorSuccess[2] = 0.643f;
            m_webPortalColorSuccess[3] = 1.0f;
            // Warning #F3C93E
            m_webPortalColorWarning[0] = 0.953f;
            m_webPortalColorWarning[1] = 0.788f;
            m_webPortalColorWarning[2] = 0.243f;
            m_webPortalColorWarning[3] = 1.0f;
            // Error #D9534F
            m_webPortalColorDanger[0] = 0.851f;
            m_webPortalColorDanger[1] = 0.325f;
            m_webPortalColorDanger[2] = 0.310f;
            m_webPortalColorDanger[3] = 1.0f;
            // Info #4CBCE6
            m_webPortalColorInfo[0] = 0.298f;
            m_webPortalColorInfo[1] = 0.737f;
            m_webPortalColorInfo[2] = 0.902f;
            m_webPortalColorInfo[3] = 1.0f;

            if (m_onWebPortalColorsChanged)
            {
                m_onWebPortalColorsChanged();
            }
            saveConfig();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Portal URL
    std::string protocol = httpsEnabled ? "https" : "http";
    std::string portalUrl = protocol + "://localhost:" + std::to_string(m_streamingPort);
    ImGui::Text("URL: %s", portalUrl.c_str());
}

// Recording trigger methods
void UIManager::triggerRecordingWidthChange(uint32_t width)
{
    m_recordingWidth = width;
    if (m_onRecordingWidthChanged)
    {
        m_onRecordingWidthChanged(width);
    }
    saveConfig();
}

void UIManager::triggerRecordingHeightChange(uint32_t height)
{
    m_recordingHeight = height;
    if (m_onRecordingHeightChanged)
    {
        m_onRecordingHeightChanged(height);
    }
    saveConfig();
}

void UIManager::triggerRecordingFpsChange(uint32_t fps)
{
    m_recordingFps = fps;
    if (m_onRecordingFpsChanged)
    {
        m_onRecordingFpsChanged(fps);
    }
    saveConfig();
}

void UIManager::triggerRecordingBitrateChange(uint32_t bitrate)
{
    m_recordingBitrate = bitrate;
    if (m_onRecordingBitrateChanged)
    {
        m_onRecordingBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerRecordingAudioBitrateChange(uint32_t bitrate)
{
    m_recordingAudioBitrate = bitrate;
    if (m_onRecordingAudioBitrateChanged)
    {
        m_onRecordingAudioBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerRecordingVideoCodecChange(const std::string& codec)
{
    m_recordingVideoCodec = codec;
    if (m_onRecordingVideoCodecChanged)
    {
        m_onRecordingVideoCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerRecordingAudioCodecChange(const std::string& codec)
{
    m_recordingAudioCodec = codec;
    if (m_onRecordingAudioCodecChanged)
    {
        m_onRecordingAudioCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerRecordingH264PresetChange(const std::string& preset)
{
    m_recordingH264Preset = preset;
    if (m_onRecordingH264PresetChanged)
    {
        m_onRecordingH264PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265PresetChange(const std::string& preset)
{
    m_recordingH265Preset = preset;
    if (m_onRecordingH265PresetChanged)
    {
        m_onRecordingH265PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265ProfileChange(const std::string& profile)
{
    m_recordingH265Profile = profile;
    if (m_onRecordingH265ProfileChanged)
    {
        m_onRecordingH265ProfileChanged(profile);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265LevelChange(const std::string& level)
{
    m_recordingH265Level = level;
    if (m_onRecordingH265LevelChanged)
    {
        m_onRecordingH265LevelChanged(level);
    }
    saveConfig();
}

void UIManager::triggerRecordingVP8SpeedChange(int speed)
{
    m_recordingVP8Speed = speed;
    if (m_onRecordingVP8SpeedChanged)
    {
        m_onRecordingVP8SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerRecordingVP9SpeedChange(int speed)
{
    m_recordingVP9Speed = speed;
    if (m_onRecordingVP9SpeedChanged)
    {
        m_onRecordingVP9SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerRecordingContainerChange(const std::string& container)
{
    m_recordingContainer = container;
    if (m_onRecordingContainerChanged)
    {
        m_onRecordingContainerChanged(container);
    }
    saveConfig();
}

void UIManager::triggerRecordingOutputPathChange(const std::string& path)
{
    m_recordingOutputPath = path;
    if (m_onRecordingOutputPathChanged)
    {
        m_onRecordingOutputPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerRecordingFilenameTemplateChange(const std::string& template_)
{
    m_recordingFilenameTemplate = template_;
    if (m_onRecordingFilenameTemplateChanged)
    {
        m_onRecordingFilenameTemplateChanged(template_);
    }
    saveConfig();
}

void UIManager::triggerRecordingIncludeAudioChange(bool include)
{
    m_recordingIncludeAudio = include;
    if (m_onRecordingIncludeAudioChanged)
    {
        m_onRecordingIncludeAudioChanged(include);
    }
    saveConfig();
}

void UIManager::triggerRecordingHardwareEncoderChange(int v)
{
    m_recordingHardwareEncoder = v;
    if (m_onRecordingHardwareEncoderChanged) m_onRecordingHardwareEncoderChanged(v);
    saveConfig();
}

void UIManager::triggerRecordingNvencPresetChange(const std::string &v)
{
    m_recordingNvencPreset = v;
    if (m_onRecordingNvencPresetChanged) m_onRecordingNvencPresetChanged(v);
    saveConfig();
}

void UIManager::triggerRecordingVaapiRcModeChange(const std::string &v)
{
    m_recordingVaapiRcMode = v;
    if (m_onRecordingVaapiRcModeChanged) m_onRecordingVaapiRcModeChanged(v);
    saveConfig();
}

void UIManager::triggerRecordingQsvPresetChange(const std::string &v)
{
    m_recordingQsvPreset = v;
    if (m_onRecordingQsvPresetChanged) m_onRecordingQsvPresetChanged(v);
    saveConfig();
}

void UIManager::triggerRecordingAmfQualityChange(const std::string &v)
{
    m_recordingAmfQuality = v;
    if (m_onRecordingAmfQualityChanged) m_onRecordingAmfQualityChanged(v);
    saveConfig();
}

void UIManager::triggerRecordingStartStop(bool start)
{
    if (m_onRecordingStartStop)
    {
        m_onRecordingStartStop(start);
    }
}

// ----------------------------------------------------------------------
// Recording profiles
// ----------------------------------------------------------------------

namespace {

RecordingSettings collectRecordingSettings(const UIManager &ui)
{
    RecordingSettings s;
    s.width = ui.getRecordingWidth();
    s.height = ui.getRecordingHeight();
    s.fps = ui.getRecordingFps();
    s.bitrate = ui.getRecordingBitrate();
    s.codec = ui.getRecordingVideoCodec();
    // The struct has a single `preset` field; map it from the active codec.
    if (s.codec == "h265" || s.codec == "hevc") s.preset = ui.getRecordingH265Preset();
    else s.preset = ui.getRecordingH264Preset();
    s.h265Profile = ui.getRecordingH265Profile();
    s.h265Level = ui.getRecordingH265Level();
    s.vp8Speed = ui.getRecordingVP8Speed();
    s.vp9Speed = ui.getRecordingVP9Speed();
    s.audioBitrate = ui.getRecordingAudioBitrate();
    s.audioCodec = ui.getRecordingAudioCodec();
    s.container = ui.getRecordingContainer();
    s.outputPath = ui.getRecordingOutputPath();
    s.filenameTemplate = ui.getRecordingFilenameTemplate();
    s.includeAudio = ui.getRecordingIncludeAudio();
    s.hardwareEncoder = ui.getRecordingHardwareEncoder();
    switch (s.hardwareEncoder)
    {
        case 2: s.hwPreset = ui.getRecordingNvencPreset(); break;
        case 3: s.hwPreset = ui.getRecordingVaapiRcMode(); break;
        case 4: s.hwPreset = ui.getRecordingQsvPreset();   break;
        case 5: s.hwPreset = ui.getRecordingAmfQuality();  break;
        default: s.hwPreset.clear(); break;
    }
    return s;
}

} // namespace

std::vector<std::string> UIManager::listRecordingProfiles()
{
    if (!m_recordingProfileManager) return {};
    return m_recordingProfileManager->list();
}

bool UIManager::recordingProfileExists(const std::string &name)
{
    if (!m_recordingProfileManager) return false;
    return m_recordingProfileManager->exists(name);
}

bool UIManager::saveRecordingProfile(const std::string &name)
{
    if (!m_recordingProfileManager) return false;
    RecordingSettings s = collectRecordingSettings(*this);
    return m_recordingProfileManager->save(name, s);
}

bool UIManager::deleteRecordingProfile(const std::string &name)
{
    if (!m_recordingProfileManager) return false;
    return m_recordingProfileManager->remove(name);
}

bool UIManager::loadRecordingProfile(const std::string &name)
{
    if (!m_recordingProfileManager) return false;
    RecordingSettings s;
    if (!m_recordingProfileManager->load(name, s)) return false;

    // Apply via triggerXxxChange for each field — that way the
    // existing callbacks fire and the RecordingManager / on-disk config
    // see the update exactly as if the user had moved each control by hand.
    triggerRecordingVideoCodecChange(s.codec);
    if (s.codec == "h265" || s.codec == "hevc")
    {
        triggerRecordingH265PresetChange(s.preset);
    }
    else
    {
        triggerRecordingH264PresetChange(s.preset);
    }
    triggerRecordingH265ProfileChange(s.h265Profile);
    triggerRecordingH265LevelChange(s.h265Level);
    triggerRecordingVP8SpeedChange(s.vp8Speed);
    triggerRecordingVP9SpeedChange(s.vp9Speed);
    triggerRecordingWidthChange(s.width);
    triggerRecordingHeightChange(s.height);
    triggerRecordingFpsChange(s.fps);
    triggerRecordingBitrateChange(s.bitrate);
    triggerRecordingAudioCodecChange(s.audioCodec);
    triggerRecordingAudioBitrateChange(s.audioBitrate);
    triggerRecordingContainerChange(s.container);
    triggerRecordingOutputPathChange(s.outputPath);
    triggerRecordingFilenameTemplateChange(s.filenameTemplate);
    triggerRecordingIncludeAudioChange(s.includeAudio);
    triggerRecordingHardwareEncoderChange(s.hardwareEncoder);
    // Route the backend-specific preset string to the right per-backend
    // field — same dispatch as the streaming side does.
    switch (s.hardwareEncoder)
    {
        case 2: triggerRecordingNvencPresetChange(s.hwPreset); break;
        case 3: triggerRecordingVaapiRcModeChange(s.hwPreset); break;
        case 4: triggerRecordingQsvPresetChange  (s.hwPreset); break;
        case 5: triggerRecordingAmfQualityChange (s.hwPreset); break;
        default: break;
    }
    return true;
}

// ----------------------------------------------------------------------
// Streaming profiles
// ----------------------------------------------------------------------

namespace {

StreamingSettings collectStreamingSettings(const UIManager &ui)
{
    StreamingSettings s;
    s.port = ui.getStreamingPort();
    s.width = ui.getStreamingWidth();
    s.height = ui.getStreamingHeight();
    s.fps = ui.getStreamingFps();
    s.bitrate = ui.getStreamingBitrate();
    s.audioBitrate = ui.getStreamingAudioBitrate();
    s.videoCodec = ui.getStreamingVideoCodec();
    s.audioCodec = ui.getStreamingAudioCodec();
    s.h264Preset = ui.getStreamingH264Preset();
    s.h265Preset = ui.getStreamingH265Preset();
    s.h265Profile = ui.getStreamingH265Profile();
    s.h265Level = ui.getStreamingH265Level();
    s.vp8Speed = ui.getStreamingVP8Speed();
    s.vp9Speed = ui.getStreamingVP9Speed();
    s.maxVideoBufferSize = ui.getStreamingMaxVideoBufferSize();
    s.maxAudioBufferSize = ui.getStreamingMaxAudioBufferSize();
    s.maxBufferTimeSeconds = ui.getStreamingMaxBufferTimeSeconds();
    s.avioBufferSize = ui.getStreamingAVIOBufferSize();
    return s;
}

} // namespace

std::vector<std::string> UIManager::listStreamingProfiles()
{
    if (!m_streamingProfileManager) return {};
    return m_streamingProfileManager->list();
}

bool UIManager::streamingProfileExists(const std::string &name)
{
    if (!m_streamingProfileManager) return false;
    return m_streamingProfileManager->exists(name);
}

bool UIManager::saveStreamingProfile(const std::string &name)
{
    if (!m_streamingProfileManager) return false;
    StreamingSettings s = collectStreamingSettings(*this);
    return m_streamingProfileManager->save(name, s);
}

bool UIManager::deleteStreamingProfile(const std::string &name)
{
    if (!m_streamingProfileManager) return false;
    return m_streamingProfileManager->remove(name);
}

bool UIManager::loadStreamingProfile(const std::string &name)
{
    if (!m_streamingProfileManager) return false;
    StreamingSettings s;
    if (!m_streamingProfileManager->load(name, s)) return false;

    // Apply via the existing triggerXxxChange setters so callbacks fire
    // and the running streamer / persisted config see the update the
    // same way they would if the user moved each control by hand.
    triggerStreamingPortChange(s.port);
    triggerStreamingVideoCodecChange(s.videoCodec);
    triggerStreamingH264PresetChange(s.h264Preset);
    triggerStreamingH265PresetChange(s.h265Preset);
    triggerStreamingH265ProfileChange(s.h265Profile);
    triggerStreamingH265LevelChange(s.h265Level);
    triggerStreamingVP8SpeedChange(s.vp8Speed);
    triggerStreamingVP9SpeedChange(s.vp9Speed);
    triggerStreamingWidthChange(s.width);
    triggerStreamingHeightChange(s.height);
    triggerStreamingFpsChange(s.fps);
    triggerStreamingBitrateChange(s.bitrate);
    triggerStreamingAudioCodecChange(s.audioCodec);
    triggerStreamingAudioBitrateChange(s.audioBitrate);
    triggerStreamingMaxVideoBufferSizeChange(s.maxVideoBufferSize);
    triggerStreamingMaxAudioBufferSizeChange(s.maxAudioBufferSize);
    triggerStreamingMaxBufferTimeSecondsChange(s.maxBufferTimeSeconds);
    triggerStreamingAVIOBufferSizeChange(s.avioBufferSize);
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Always-on-top connection state overlay.
//
// Reads the same signals UIInfoPanel does (source type, current
// device, capture dims, host-likely-offline mirror) and renders a
// small fixed window in the bottom-right corner whenever the user
// needs to know something is happening:
//
//   Connecting...   — Remote source armed, no frames yet, fresh URL
//   Reconnecting... — Remote source armed, frames stopped or host-likely-offline
//   Disconnecting...— transient ~1.5s after currentDevice clears
//   Connected       — flashes for ~3 s on the first frame
//
// Renders BEFORE UIManager::render()'s m_uiVisible gate so the user
// sees state changes with the full UI hidden too. The window itself
// uses NoInputs so it never steals clicks from the underlying stream
// view.
// ─────────────────────────────────────────────────────────────────────
void UIManager::renderConnectionOverlay()
{
    // Delegates to osd::ConnectionStatusOverlay since the OSD layer
    // pass in #68. UIManager keeps this thin entry point so existing
    // callers (Application's render path) don't need to learn about
    // the new location.
    if (m_connectionOverlay) m_connectionOverlay->render();
}
