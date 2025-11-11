#pragma once

#include <string>
#include <cstdint>

struct WindowConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    std::string title = "RetroCapture";
    bool fullscreen = false;
    bool vsync = true;
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
    
    void makeCurrent();
    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    
    // Callbacks
    void setResizeCallback(void (*callback)(int, int));
    
private:
    void* m_window = nullptr; // GLFWwindow* (opaque pointer)
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
    
    void (*m_resizeCallback)(int, int) = nullptr;
};

