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
    
    GLFWwindow* window = glfwCreateWindow(
        config.width,
        config.height,
        config.title.c_str(),
        config.fullscreen ? glfwGetPrimaryMonitor() : nullptr,
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
            wm->m_width = width;
            wm->m_height = height;
            if (wm->m_resizeCallback) {
                wm->m_resizeCallback(width, height);
            }
        }
    });
    
    m_window = window;
    m_width = config.width;
    m_height = config.height;
    m_initialized = true;
    
    LOG_INFO("Janela criada: " + std::to_string(m_width) + "x" + std::to_string(m_height));
    
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

void WindowManager::setResizeCallback(void (*callback)(int, int)) {
    m_resizeCallback = callback;
}


