#include "WindowManager.h"
#include "../utils/Logger.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

WindowManager::WindowManager() {
}

WindowManager::~WindowManager() {
    shutdown();
}

bool WindowManager::init(const WindowConfig& config) {
    if (m_initialized) {
        LOG_WARN("WindowManager já inicializado");
        return true;
    }
    
    if (!glfwInit()) {
        LOG_ERROR("Falha ao inicializar GLFW");
        return false;
    }
    
    // Configurar OpenGL
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Determinar qual monitor usar
    GLFWmonitor* monitor = nullptr;
    if (config.fullscreen) {
        if (config.monitorIndex >= 0) {
            // Usar monitor específico
            int monitorCount;
            GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
            if (config.monitorIndex < monitorCount) {
                monitor = monitors[config.monitorIndex];
                const char* monitorName = glfwGetMonitorName(monitor);
                LOG_INFO("Usando monitor " + std::to_string(config.monitorIndex) + 
                         (monitorName ? (": " + std::string(monitorName)) : ""));
            } else {
                LOG_WARN("Monitor índice " + std::to_string(config.monitorIndex) + 
                         " não encontrado (total: " + std::to_string(monitorCount) + 
                         "), usando monitor primário");
                monitor = glfwGetPrimaryMonitor();
            }
        } else {
            // Usar monitor primário
            monitor = glfwGetPrimaryMonitor();
        }
    }
    
    GLFWwindow* window = glfwCreateWindow(
        config.width,
        config.height,
        config.title.c_str(),
        monitor,
        nullptr
    );
    
    if (!window) {
        LOG_ERROR("Falha ao criar janela GLFW");
        glfwTerminate();
        return false;
    }
    
    glfwMakeContextCurrent(window);
    
    // Carregar funções OpenGL (GLAD seria ideal, mas por enquanto vamos usar diretamente)
    // Em sistemas Linux com Mesa, geralmente funciona sem GLAD
    
    if (config.vsync) {
        glfwSwapInterval(1);
    } else {
        glfwSwapInterval(0);
    }
    
    // Callback de resize
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
        WindowManager* wm = static_cast<WindowManager*>(glfwGetWindowUserPointer(w));
        if (wm) {
            // Atualizar dimensões do WindowManager
            wm->m_width = width;
            wm->m_height = height;
            
            // Chamar callback do WindowManager (se configurado)
            // IMPORTANTE: Este callback pode ser uma lambda que atualiza o viewport do ShaderEngine
            // Isso garante que o viewport seja atualizado imediatamente quando entra em fullscreen
            if (wm->m_resizeCallback) {
                wm->m_resizeCallback(width, height);
            }
        }
    });
    
    m_window = window;
    
    // IMPORTANTE: Sempre obter as dimensões reais do framebuffer após criar a janela
    // Isso é especialmente importante em fullscreen, onde as dimensões podem ser diferentes
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    m_width = fbWidth > 0 ? fbWidth : config.width;
    m_height = fbHeight > 0 ? fbHeight : config.height;
    
    m_initialized = true;
    
    LOG_INFO("Janela criada: " + std::to_string(m_width) + "x" + std::to_string(m_height) + 
             (config.fullscreen ? " (fullscreen)" : " (windowed)") +
             " [framebuffer: " + std::to_string(fbWidth) + "x" + std::to_string(fbHeight) + "]");
    
    return true;
}

void WindowManager::shutdown() {
    if (!m_initialized) {
        return;
    }
    
    if (m_window) {
        glfwDestroyWindow(static_cast<GLFWwindow*>(m_window));
        m_window = nullptr;
    }
    
    glfwTerminate();
    m_initialized = false;
    LOG_INFO("WindowManager encerrado");
}

bool WindowManager::shouldClose() const {
    if (!m_window) {
        return true;
    }
    return glfwWindowShouldClose(static_cast<GLFWwindow*>(m_window));
}

void WindowManager::swapBuffers() {
    if (m_window) {
        glfwSwapBuffers(static_cast<GLFWwindow*>(m_window));
    }
}

void WindowManager::pollEvents() {
    glfwPollEvents();
}

void WindowManager::makeCurrent() {
    if (m_window) {
        glfwMakeContextCurrent(static_cast<GLFWwindow*>(m_window));
    }
}

void WindowManager::setResizeCallback(std::function<void(int, int)> callback) {
    m_resizeCallback = callback;
}

void WindowManager::setFullscreen(bool fullscreen, int monitorIndex) {
    if (!m_window || !m_initialized) {
        return;
    }
    
    GLFWwindow* window = static_cast<GLFWwindow*>(m_window);
    
    if (fullscreen) {
        // Entrar em fullscreen
        GLFWmonitor* monitor = nullptr;
        if (monitorIndex >= 0) {
            int monitorCount;
            GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
            if (monitorIndex < monitorCount) {
                monitor = monitors[monitorIndex];
            } else {
                monitor = glfwGetPrimaryMonitor();
            }
        } else {
            monitor = glfwGetPrimaryMonitor();
        }
        
        if (monitor) {
            // IMPORTANTE: Desabilitar auto-iconify para evitar que a janela minimize quando perder o foco
            // Isso permite que a aplicação permaneça em fullscreen mesmo quando o usuário interage com outras janelas
            glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_FALSE);
            
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
    } else {
        // Sair do fullscreen (voltar para windowed)
        // Reabilitar auto-iconify quando sair do fullscreen (comportamento padrão)
        glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_TRUE);
        
        // Usar dimensões anteriores ou padrão
        int width = m_width > 0 ? m_width : 1280;
        int height = m_height > 0 ? m_height : 720;
        glfwSetWindowMonitor(window, nullptr, 100, 100, width, height, GLFW_DONT_CARE);
    }
    
    // Atualizar dimensões após mudança
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    m_width = fbWidth > 0 ? fbWidth : m_width;
    m_height = fbHeight > 0 ? fbHeight : m_height;
    
    // IMPORTANTE: Não chamar callback de resize diretamente aqui
    // O GLFW já chama o callback de resize automaticamente quando a janela muda
    // Chamar aqui pode causar deadlock ou travamento
    // O callback será chamado pelo GLFW no próximo pollEvents()
}


