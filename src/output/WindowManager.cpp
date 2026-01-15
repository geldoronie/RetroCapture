#include "WindowManager.h"
#include "../utils/Logger.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef PLATFORM_LINUX
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

WindowManager::WindowManager()
{
}

WindowManager::~WindowManager()
{
    shutdown();
}

bool WindowManager::init(const WindowConfig &config)
{
    if (m_initialized)
    {
        LOG_WARN("WindowManager already initialized");
        return true;
    }

    if (!glfwInit())
    {
        LOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    // Configure OpenGL
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Determine which monitor to use
    GLFWmonitor *monitor = nullptr;
    if (config.fullscreen)
    {
        if (config.monitorIndex >= 0)
        {
            // Use specific monitor
            int monitorCount;
            GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
            if (config.monitorIndex < monitorCount)
            {
                monitor = monitors[config.monitorIndex];
                const char *monitorName = glfwGetMonitorName(monitor);
                LOG_INFO("Using monitor " + std::to_string(config.monitorIndex) +
                         (monitorName ? (": " + std::string(monitorName)) : ""));
            }
            else
            {
                LOG_WARN("Monitor index " + std::to_string(config.monitorIndex) +
                         " not found (total: " + std::to_string(monitorCount) +
                         "), using primary monitor");
                monitor = glfwGetPrimaryMonitor();
            }
        }
        else
        {
            // Use primary monitor
            monitor = glfwGetPrimaryMonitor();
        }
    }

    GLFWwindow *window = glfwCreateWindow(
        config.width,
        config.height,
        config.title.c_str(),
        monitor,
        nullptr);

    if (!window)
    {
        LOG_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);

#ifdef PLATFORM_LINUX
    // Set WM_CLASS for proper application identification in Linux window managers
    // This ensures the application appears correctly in the taskbar/launcher
    Display *display = glfwGetX11Display();
    Window x11Window = glfwGetX11Window(window);
    if (display && x11Window)
    {
        XClassHint *classHint = XAllocClassHint();
        if (classHint)
        {
            // WM_CLASS consists of two strings: res_name (instance) and res_class (class)
            // res_name is typically the executable name, res_class is the application name
            classHint->res_name = const_cast<char *>("retrocapture");
            classHint->res_class = const_cast<char *>("RetroCapture");
            XSetClassHint(display, x11Window, classHint);
            XFree(classHint);
            LOG_INFO("WM_CLASS set to RetroCapture for proper window manager identification");
        }
    }
#endif

    // Load OpenGL functions (GLAD would be ideal, but for now we'll use directly)
    // On Linux systems with Mesa, it usually works without GLAD

    if (config.vsync)
    {
        glfwSwapInterval(1);
    }
    else
    {
        glfwSwapInterval(0);
    }

    // Resize callback
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow *w, int width, int height)
                                   {
        WindowManager* wm = static_cast<WindowManager*>(glfwGetWindowUserPointer(w));
        if (wm) {
            // Update WindowManager dimensions
            wm->m_width = width;
            wm->m_height = height;
            
            // Call WindowManager callback (if configured)
            // IMPORTANT: This callback can be a lambda that updates the ShaderEngine viewport
            // This ensures the viewport is updated immediately when entering fullscreen
            if (wm->m_resizeCallback) {
                wm->m_resizeCallback(width, height);
            }
        } });

    m_window = window;

    // IMPORTANT: Always get the actual framebuffer dimensions after creating the window
    // This is especially important in fullscreen, where dimensions may be different
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    m_width = fbWidth > 0 ? fbWidth : config.width;
    m_height = fbHeight > 0 ? fbHeight : config.height;

    m_initialized = true;

    LOG_INFO("Window created: " + std::to_string(m_width) + "x" + std::to_string(m_height) +
             (config.fullscreen ? " (fullscreen)" : " (windowed)") +
             " [framebuffer: " + std::to_string(fbWidth) + "x" + std::to_string(fbHeight) + "]");

    return true;
}

void WindowManager::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    if (m_window)
    {
        glfwDestroyWindow(static_cast<GLFWwindow *>(m_window));
        m_window = nullptr;
    }

    glfwTerminate();
    m_initialized = false;
    LOG_INFO("WindowManager shutdown");
}

bool WindowManager::shouldClose() const
{
    if (!m_window)
    {
        return true;
    }
    return glfwWindowShouldClose(static_cast<GLFWwindow *>(m_window));
}

void WindowManager::swapBuffers()
{
    if (m_window)
    {
        glfwSwapBuffers(static_cast<GLFWwindow *>(m_window));
    }
}

void WindowManager::pollEvents()
{
    glfwPollEvents();
}

void WindowManager::makeCurrent()
{
    if (m_window)
    {
        glfwMakeContextCurrent(static_cast<GLFWwindow *>(m_window));
    }
}

void WindowManager::setResizeCallback(std::function<void(int, int)> callback)
{
    m_resizeCallback = callback;
}

void WindowManager::setCursorVisible(bool visible)
{
    if (!m_window)
    {
        return;
    }
    
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);
    
    // Check current cursor mode
    int currentMode = glfwGetInputMode(window, GLFW_CURSOR);
    int desiredMode = visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN;
    
    // Only change if different to avoid unnecessary calls
    if (currentMode != desiredMode)
    {
        glfwSetInputMode(window, GLFW_CURSOR, desiredMode);
    }
}

void WindowManager::setFullscreen(bool fullscreen, int monitorIndex)
{
    if (!m_window || !m_initialized)
    {
        return;
    }

    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);

    if (fullscreen)
    {
        // Enter fullscreen
        GLFWmonitor *monitor = nullptr;
        if (monitorIndex >= 0)
        {
            int monitorCount;
            GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
            if (monitorIndex < monitorCount)
            {
                monitor = monitors[monitorIndex];
            }
            else
            {
                monitor = glfwGetPrimaryMonitor();
            }
        }
        else
        {
            monitor = glfwGetPrimaryMonitor();
        }

        if (monitor)
        {
// IMPORTANT: Disable auto-iconify to prevent window from minimizing when losing focus
// This allows the application to remain in fullscreen even when user interacts with other windows
// Note: glfwSetWindowAttrib requires GLFW 3.3+, if not available, comment this line
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
            glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_FALSE);
#endif

            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
    }
    else
    {
// Exit fullscreen (return to windowed)
// Re-enable auto-iconify when exiting fullscreen (default behavior)
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
        glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_TRUE);
#endif

        // Use previous or default dimensions
        int width = m_width > 0 ? m_width : 1280;
        int height = m_height > 0 ? m_height : 720;
        glfwSetWindowMonitor(window, nullptr, 100, 100, width, height, GLFW_DONT_CARE);
    }

    // Update dimensions after change
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    m_width = fbWidth > 0 ? fbWidth : m_width;
    m_height = fbHeight > 0 ? fbHeight : m_height;

    // IMPORTANT: Do not call resize callback directly here
    // GLFW already calls the resize callback automatically when the window changes
    // Calling here can cause deadlock or freezing
    // The callback will be called by GLFW on the next pollEvents()
}
