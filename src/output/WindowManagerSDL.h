#pragma once

#include "WindowManager.h" // Para WindowConfig
#include <string>
#include <cstdint>
#include <functional>

#ifdef USE_SDL2
// Forward declarations
// SDL_GLContext é um typedef void* definido em SDL_video.h
struct SDL_Window;
// Não podemos fazer forward declaration de typedef, então incluímos o header quando necessário
// ou usamos void* diretamente
#endif

/**
 * WindowManagerSDL - Implementação do WindowManager usando SDL2
 * 
 * Suporta DirectFB e framebuffer Linux através do backend SDL2.
 * Use BUILD_WITH_SDL2=ON no CMake para compilar com esta implementação.
 */
class WindowManagerSDL {
public:
    WindowManagerSDL();
    ~WindowManagerSDL();
    
    bool init(const WindowConfig& config);
    void shutdown();
    
    bool shouldClose() const;
    void swapBuffers();
    void pollEvents();
    
    void makeCurrent();
    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    
    // Obter ponteiro SDL_Window (para ImGui, etc)
    void* getWindow() const { return m_window; }
    
    // Toggle fullscreen
    void setFullscreen(bool fullscreen, int monitorIndex = -1);
    
    // Callbacks
    void setResizeCallback(std::function<void(int, int)> callback);
    
    // Obter ponteiro para Application (para callbacks)
    void setUserData(void* userData) { m_userData = userData; }
    void* getUserData() const { return m_userData; }
    
    // Verificar se tecla específica está pressionada (para compatibilidade com GLFW)
    bool isKeyPressed(int keyCode) const; // keyCode: SDLK_F12, etc.
    
    // Controlar visibilidade do cursor
    void setCursorVisible(bool visible);
    
private:
#ifdef USE_SDL2
    SDL_Window* m_window = nullptr;
    void* m_glContext = nullptr; // SDL_GLContext é typedef void*
#else
    void* m_window = nullptr;
    void* m_glContext = nullptr;
#endif
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
    bool m_shouldClose = false;
    bool m_f12Pressed = false; // Track F12 key state
    
    std::function<void(int, int)> m_resizeCallback = nullptr;
    void* m_userData = nullptr;
    
    // Configuração SDL2
    void configureSDL2ForDirectFB();
};
