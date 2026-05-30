#pragma once

#include <string>
#include <cstdint>
#include <functional>

struct WindowConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    std::string title = "RetroCapture";
    bool fullscreen = false;
    bool vsync = true;
    int monitorIndex = -1; // -1 = usar monitor primário, 0+ = índice do monitor
};

class WindowManager {
public:
    WindowManager();
    ~WindowManager();
    
    bool init(const WindowConfig& config);
    void shutdown();

    bool shouldClose() const;
    void swapBuffers();
    void pollEvents();

    // Hide-to-tray support (#86). hide() unmaps the window without
    // tearing down the GL context — the capture/render pipelines keep
    // running; the main loop skips the viewport swap while hidden.
    // show() re-maps + refocuses.
    void hide();
    void show();
    bool isVisible() const;

    // Close-button interception. When a callback is installed, the
    // GLFW window-close request does NOT set shouldClose; instead the
    // callback runs and decides (hide-to-tray vs real quit). Without
    // a callback, the default GLFW behaviour (shouldClose = true)
    // stands. Returning the callback lets Application implement the
    // "minimize to tray vs quit" preference.
    void setCloseCallback(std::function<void()> callback);

    // Force a real close (used by the tray "Quit" action and by the
    // close callback when the user chose quit-on-close).
    void requestClose();

    // Runtime vsync toggle. Mirrors what config.vsync did at init but
    // lets the rest of the app switch the swap interval mid-session —
    // remote-source playback wants vsync ON so frames land on display
    // refresh boundaries (no judder), while local capture/streaming
    // wants vsync OFF so a backgrounded window doesn't stall the
    // capture/encoder threads waiting on a refresh that never comes.
    void setVsync(bool enabled);

    // Whether the OS reports the window as focused (i.e. receiving
    // input). The main loop uses this to disable vsync when the user
    // alt-tabs away — most compositors park vsync at 0 Hz for
    // backgrounded windows, which would otherwise block swapBuffers
    // forever and stall the entire main loop (queue fills, drops
    // pile up, no recovery until focus returns).
    bool isFocused() const;
    
    void makeCurrent();
    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    
    // Obter ponteiro GLFWwindow (para ImGui, etc)
    void* getWindow() const { return m_window; }
    
    // Toggle fullscreen
    void setFullscreen(bool fullscreen, int monitorIndex = -1);
    
    // Callbacks
    // IMPORTANTE: Usa std::function para permitir lambdas com capture
    void setResizeCallback(std::function<void(int, int)> callback);
    
    // Obter ponteiro para Application (para callbacks)
    void setUserData(void* userData) { m_userData = userData; }
    void* getUserData() const { return m_userData; }
    
    // Controlar visibilidade do cursor
    void setCursorVisible(bool visible);
    void forceSetCursorVisible(bool visible); // Force update ignoring cache
    
private:
    void* m_window = nullptr; // GLFWwindow* (opaque pointer)
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
    
    std::function<void(int, int)> m_resizeCallback = nullptr;
    std::function<void()> m_closeCallback = nullptr; // #86 hide-to-tray
    void* m_userData = nullptr; // User data para callbacks

    bool m_visible = true; // tracked locally; GLFW has no portable query
    
    // Cache cursor visibility state to avoid unnecessary glfwSetInputMode calls
    mutable bool m_cursorVisible = true;
    mutable bool m_cursorStateInitialized = false;
};

