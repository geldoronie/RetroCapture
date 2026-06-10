#include "WindowManager.h"
#include "../utils/Logger.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef PLATFORM_LINUX
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <setjmp.h>

// Static variables for SIGSEGV handling in pollEvents
namespace {
    bool sigsegv_handler_installed = false;
    jmp_buf jump_buffer;
    WindowManager* current_wm = nullptr;
    
    // Signal handler function (must be a regular function, not lambda)
    void sigsegv_handler(int sig) {
        if (sig == SIGSEGV && current_wm)
        {
            // Mark window as closed to prevent further operations
            void* window_ptr = current_wm->getWindow();
            if (window_ptr)
            {
                GLFWwindow *w = static_cast<GLFWwindow *>(window_ptr);
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            }
            // Jump back to safe point
            siglongjmp(jump_buffer, 1);
        }
    }
}
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

    // Close-button interception (#86). GLFW sets shouldClose=true on
    // the window-close request by default; when a close callback is
    // installed we veto that (reset the flag) and hand the decision
    // to Application — which either hides to tray or really quits.
    glfwSetWindowCloseCallback(window, [](GLFWwindow *w)
                               {
        WindowManager* wm = static_cast<WindowManager*>(glfwGetWindowUserPointer(w));
        if (wm && wm->m_closeCallback) {
            // Veto GLFW's auto-close; the callback owns the decision.
            glfwSetWindowShouldClose(w, GLFW_FALSE);
            wm->m_closeCallback();
        }
        // No callback installed → leave shouldClose=true (default quit).
        });

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
        GLFWwindow *window = static_cast<GLFWwindow *>(m_window);
        
#ifdef PLATFORM_LINUX
        // Use signal handler to catch SIGSEGV during shutdown
        // This can happen when Wayland connection is invalidated
        if (!sigsegv_handler_installed)
        {
            struct sigaction sa;
            sa.sa_handler = sigsegv_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_NODEFER | SA_RESETHAND;
            sigaction(SIGSEGV, &sa, nullptr);
            sigsegv_handler_installed = true;
        }
        
        current_wm = this;
        
        // Use setjmp/longjmp to recover from SIGSEGV during window destruction
        if (sigsetjmp(jump_buffer, 1) == 0)
        {
            // Try to destroy window - may crash if Wayland connection is invalid
            glfwDestroyWindow(window);
        }
        else
        {
            // Recovered from SIGSEGV - window destruction failed but we continue
            LOG_WARN("SIGSEGV caught during window destruction - continuing shutdown");
        }
        
        current_wm = nullptr;
#else
        try
        {
            glfwDestroyWindow(window);
        }
        catch (...)
        {
            LOG_WARN("Exception during window destruction - continuing shutdown");
        }
#endif
        m_window = nullptr;
    }

    // Terminate GLFW - this should be safe even if window destruction failed
    try
    {
        glfwTerminate();
    }
    catch (...)
    {
        LOG_WARN("Exception during GLFW termination - continuing");
    }
    
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
    if (!m_window)
    {
        return;
    }

    // Hide-to-tray (#86): nothing to present while hidden, and a
    // hidden window's swap can BLOCK (compositors park vsync at 0 Hz
    // for unmapped windows) which would stall the whole main loop.
    // Skipping the swap here gates ALL call sites at once; the GL
    // work that ran before it is harmlessly discarded. The main loop
    // paces itself with a sleep while hidden.
    if (!m_visible)
    {
        return;
    }

    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);

    // Check if window is still valid before swapping
    // This prevents crashes when window is invalidated (e.g., KVM switch)
    if (glfwWindowShouldClose(window))
    {
        return;
    }
    
    try
    {
        glfwSwapBuffers(window);
    }
    catch (...)
    {
        // If swap fails, window is likely invalid - log and continue
        // GLFW will set shouldClose flag automatically
        LOG_WARN("Failed to swap buffers - window may be invalid");
    }
}

void WindowManager::pollEvents()
{
    if (!m_window)
    {
        return;
    }
    
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);
    
    // Check if window should close before polling events
    // This prevents crashes when Wayland connection is invalidated
    if (glfwWindowShouldClose(window))
    {
        return;
    }
    
#ifdef PLATFORM_LINUX
    // Use signal handler to catch SIGSEGV from Wayland/GLFW
    // This happens when USB devices are disconnected and Wayland connection is invalidated
    if (!sigsegv_handler_installed)
    {
        struct sigaction sa;
        sa.sa_handler = sigsegv_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER | SA_RESETHAND; // Reset handler after use
        sigaction(SIGSEGV, &sa, nullptr);
        sigsegv_handler_installed = true;
    }
    
    current_wm = this;
    
    // Use setjmp/longjmp to recover from SIGSEGV
    if (sigsetjmp(jump_buffer, 1) == 0)
    {
        // Try to poll events - may crash if Wayland connection is invalid
        glfwPollEvents();
    }
    else
    {
        // Recovered from SIGSEGV - window is now marked as should close
        LOG_WARN("SIGSEGV caught in pollEvents - Wayland connection invalidated, window marked for close");
    }
    
    current_wm = nullptr;
#else
    // On non-Linux platforms, just call glfwPollEvents normally
    try
    {
        glfwPollEvents();
    }
    catch (...)
    {
        // If pollEvents fails, window may be invalid - log and continue
        LOG_WARN("Error polling events - window may be invalid");
    }
#endif
}

void WindowManager::makeCurrent()
{
    if (m_window)
    {
        glfwMakeContextCurrent(static_cast<GLFWwindow *>(m_window));
    }
}

void WindowManager::setVsync(bool enabled)
{
    if (m_window)
    {
        glfwSwapInterval(enabled ? 1 : 0);
    }
}

bool WindowManager::isFocused() const
{
    if (!m_window) return false;
    return glfwGetWindowAttrib(static_cast<GLFWwindow *>(m_window), GLFW_FOCUSED) != 0;
}

void WindowManager::setResizeCallback(std::function<void(int, int)> callback)
{
    m_resizeCallback = callback;
}

void WindowManager::setCloseCallback(std::function<void()> callback)
{
    m_closeCallback = std::move(callback);
}

void WindowManager::requestClose()
{
    if (m_window)
    {
        glfwSetWindowShouldClose(static_cast<GLFWwindow *>(m_window), GLFW_TRUE);
    }
}

void WindowManager::hide()
{
    if (m_window)
    {
        glfwHideWindow(static_cast<GLFWwindow *>(m_window));
        m_visible = false;
        LOG_INFO("WindowManager: window hidden (minimize to tray)");
    }
}

void WindowManager::show()
{
    if (m_window)
    {
        GLFWwindow *w = static_cast<GLFWwindow *>(m_window);
        glfwShowWindow(w);
        glfwFocusWindow(w);
        m_visible = true;
        LOG_INFO("WindowManager: window shown");
    }
}

bool WindowManager::isVisible() const
{
    // GLFW_VISIBLE reflects the mapped state; fall back to our cached
    // flag if the window is gone.
    if (!m_window)
    {
        return false;
    }
    return glfwGetWindowAttrib(static_cast<GLFWwindow *>(m_window),
                               GLFW_VISIBLE) == GLFW_TRUE;
}

void WindowManager::setCursorVisible(bool visible)
{
    if (!m_window)
    {
        return;
    }
    
    // Simple cache to avoid unnecessary calls
    if (m_cursorStateInitialized && m_cursorVisible == visible)
    {
        return;
    }
    
    m_cursorVisible = visible;
    m_cursorStateInitialized = true;
    
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);
    glfwSetInputMode(window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void WindowManager::forceSetCursorVisible(bool visible)
{
    if (!m_window)
    {
        return;
    }
    
    // Force update ignoring cache (only used when visibility actually changes)
    m_cursorVisible = visible;
    m_cursorStateInitialized = true;
    
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window);
    glfwSetInputMode(window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
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
