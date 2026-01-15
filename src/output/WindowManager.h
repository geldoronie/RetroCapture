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
    
private:
    void* m_window = nullptr; // GLFWwindow* (opaque pointer)
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
    
    std::function<void(int, int)> m_resizeCallback = nullptr;
    void* m_userData = nullptr; // User data para callbacks
};

