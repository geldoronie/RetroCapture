#include "WindowManagerSDL.h"
#include "../utils/Logger.h"

#ifdef USE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_video.h>

WindowManagerSDL::WindowManagerSDL()
{
}

WindowManagerSDL::~WindowManagerSDL()
{
    shutdown();
}

void WindowManagerSDL::configureSDL2ForDirectFB()
{
    // Configurar SDL2 para usar o driver de vídeo apropriado
    const char* display = std::getenv("DISPLAY");
    const char* video_driver = std::getenv("SDL_VIDEODRIVER");
    
    // Detectar arquitetura ARM
    bool isARM = false;
    #if defined(__arm__) || defined(__aarch64__) || defined(ARCH_ARM) || defined(ARCH_ARM32) || defined(ARCH_ARM64)
        isARM = true;
    #endif
    
    if (video_driver) {
        // Usuário especificou explicitamente o driver
        SDL_SetHint(SDL_HINT_VIDEODRIVER, video_driver);
        LOG_INFO("SDL2: Usando driver especificado: " + std::string(video_driver));
        
        // Se DirectFB foi especificado, verificar se está disponível
        if (std::string(video_driver) == "directfb") {
            LOG_INFO("SDL2: DirectFB solicitado - verificando disponibilidade...");
        }
    } else if (display) {
        // Temos DISPLAY (X11 disponível) - usar X11
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
        LOG_INFO("SDL2: Usando X11 (DISPLAY disponível)");
    } else {
        // Sem DISPLAY - priorizar DirectFB em sistemas ARM
        if (isARM) {
            // ARM: DirectFB é preferível para sistemas embarcados
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "directfb");
            LOG_INFO("SDL2: Sistema ARM detectado - tentando DirectFB primeiro (sem DISPLAY)");
        } else {
            // x86: Tentar DirectFB primeiro, depois framebuffer
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "directfb");
            LOG_INFO("SDL2: Tentando DirectFB (sem DISPLAY)");
        }
    }
}

bool WindowManagerSDL::init(const WindowConfig &config)
{
    if (m_initialized)
    {
        LOG_WARN("WindowManagerSDL already initialized");
        return true;
    }

    // Configurar driver de vídeo antes de inicializar
    configureSDL2ForDirectFB();
    
    // Inicializar SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        std::string error = SDL_GetError();
        LOG_ERROR("Failed to initialize SDL2: " + error);
        
        // Se falhou e não foi especificado driver, tentar fallbacks
        const char* video_driver = std::getenv("SDL_VIDEODRIVER");
        if (!video_driver) {
            const char* display = std::getenv("DISPLAY");
            
            if (!display) {
                // Sem DISPLAY - tentar DirectFB primeiro, depois framebuffer
                const char* current_hint = SDL_GetHint(SDL_HINT_VIDEODRIVER);
                std::string current_driver = current_hint ? current_hint : "";
                
                if (current_driver == "directfb" || current_driver.empty()) {
                    // DirectFB falhou, tentar framebuffer
                    LOG_WARN("SDL2: DirectFB falhou: " + error);
                    LOG_INFO("SDL2: Tentando framebuffer como fallback...");
                    SDL_SetHint(SDL_HINT_VIDEODRIVER, "fbcon");
                    SDL_Quit(); // Limpar tentativa anterior
                    
                    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
                        LOG_INFO("SDL2: Framebuffer inicializado com sucesso");
                    } else {
                        LOG_ERROR("SDL2: Framebuffer também falhou: " + std::string(SDL_GetError()));
                        LOG_ERROR("SDL2: Dicas para sistemas ARM:");
                        LOG_ERROR("  1. Instale DirectFB: sudo apt-get install libdirectfb-dev directfb");
                        LOG_ERROR("  2. Verifique framebuffer: ls -l /dev/fb*");
                        LOG_ERROR("  3. Verifique permissões: sudo chmod 666 /dev/fb0");
                        LOG_ERROR("  4. Use X11: export DISPLAY=:0");
                        LOG_ERROR("  5. Para compilar com SDL2: cmake .. -DBUILD_WITH_SDL2=ON");
                        return false;
                    }
                } else {
                    // Framebuffer falhou, já tentamos tudo
                    LOG_ERROR("SDL2: Framebuffer falhou: " + error);
                    LOG_ERROR("SDL2: Dicas para sistemas ARM:");
                    LOG_ERROR("  1. Instale DirectFB: sudo apt-get install libdirectfb-dev directfb");
                    LOG_ERROR("  2. Verifique framebuffer: ls -l /dev/fb*");
                    LOG_ERROR("  3. Use X11: export DISPLAY=:0");
                    return false;
                }
            } else {
                // Tem DISPLAY mas falhou - tentar X11 explicitamente
                LOG_WARN("SDL2: Driver padrão falhou: " + error);
                LOG_INFO("SDL2: Tentando X11 explicitamente...");
                SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
                SDL_Quit(); // Limpar tentativa anterior
                
                if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0) {
                    LOG_INFO("SDL2: X11 inicializado com sucesso");
                } else {
                    LOG_ERROR("SDL2: X11 também falhou: " + std::string(SDL_GetError()));
                    LOG_ERROR("SDL2: Verifique se X11 está funcionando: echo $DISPLAY");
                    return false;
                }
            }
        } else {
            // Driver foi especificado explicitamente mas falhou
            LOG_ERROR("SDL2: Driver '" + std::string(video_driver) + "' falhou: " + error);
            
            if (std::string(video_driver) == "directfb") {
                LOG_ERROR("SDL2: DirectFB não está disponível ou não foi compilado no SDL2");
                LOG_ERROR("SDL2: Dicas para sistemas ARM:");
                LOG_ERROR("  1. Instale DirectFB: sudo apt-get install libdirectfb-dev directfb");
                LOG_ERROR("  2. Verifique se SDL2 foi compilado com suporte a DirectFB");
                LOG_ERROR("  3. Tente framebuffer: export SDL_VIDEODRIVER=fbcon");
                LOG_ERROR("  4. Para compilar com SDL2: cmake .. -DBUILD_WITH_SDL2=ON");
            } else if (std::string(video_driver) == "fbcon") {
                LOG_ERROR("SDL2: Framebuffer não está disponível");
                LOG_ERROR("SDL2: Verifique: ls -l /dev/fb*");
                LOG_ERROR("SDL2: Pode precisar de permissões: sudo chmod 666 /dev/fb0");
                LOG_ERROR("SDL2: Ou tente DirectFB: export SDL_VIDEODRIVER=directfb");
            }
            return false;
        }
    }

    // Configurar atributos OpenGL ANTES de criar a janela
    // Detectar arquitetura ARM para priorizar OpenGL ES
    bool isARM = false;
    #if defined(__arm__) || defined(__aarch64__) || defined(ARCH_ARM) || defined(ARCH_ARM32) || defined(ARCH_ARM64)
        isARM = true;
    #endif
    
    // Em sistemas ARM, tentar OpenGL ES primeiro (mais comum)
    // Em sistemas x86, tentar OpenGL Desktop primeiro
    if (isARM) {
        // ARM: Tentativa 1: OpenGL ES 2.0 (mais comum em sistemas embarcados ARM)
        LOG_INFO("SDL2: Sistema ARM detectado - tentando OpenGL ES primeiro");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    } else {
        // x86: Tentativa 1: OpenGL 3.3 Core (preferido)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    }
    
    // Usar resolução configurada pelo usuário (sem limitações hardcoded)
    uint32_t actualWidth = config.width;
    uint32_t actualHeight = config.height;
    
    // Flags da janela
    Uint32 windowFlags = SDL_WINDOW_OPENGL;
    if (config.fullscreen)
    {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP; // Resolução nativa da tela
    }

    // Criar janela
    m_window = SDL_CreateWindow(
        config.title.c_str(),
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        static_cast<int>(actualWidth),
        static_cast<int>(actualHeight),
        windowFlags);

    if (!m_window)
    {
        LOG_ERROR("Failed to create SDL2 window: " + std::string(SDL_GetError()));
        SDL_Quit();
        return false;
    }

    // Criar contexto OpenGL com fallback para versões mais antigas
    m_glContext = SDL_GL_CreateContext(m_window);
    if (m_glContext) {
        if (isARM) {
            LOG_INFO("SDL2: OpenGL ES 2.0 context created");
        } else {
            LOG_INFO("SDL2: OpenGL 3.3 Core context created");
        }
    } else {
        std::string firstError = SDL_GetError();
        if (isARM) {
            LOG_WARN("SDL2: Failed to create OpenGL ES 2.0 context: " + firstError);
            
            // ARM: Tentativa 2: OpenGL ES 3.0 (se disponível)
            SDL_DestroyWindow(m_window);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            
            m_window = SDL_CreateWindow(
                config.title.c_str(),
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                static_cast<int>(actualWidth),
                static_cast<int>(actualHeight),
                windowFlags);
            
            if (!m_window) {
                LOG_ERROR("Failed to recreate SDL2 window for OpenGL ES 3.0: " + std::string(SDL_GetError()));
                SDL_Quit();
                return false;
            }
            
            m_glContext = SDL_GL_CreateContext(m_window);
            if (m_glContext) {
                LOG_INFO("SDL2: OpenGL ES 3.0 context created (fallback)");
            } else {
                LOG_WARN("SDL2: Failed to create OpenGL ES 3.0 context: " + std::string(SDL_GetError()));
                
                // ARM: Tentativa 3: OpenGL 2.1 (compatibility mode)
                SDL_DestroyWindow(m_window);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0); // Sem profile (compatibility)
                
                m_window = SDL_CreateWindow(
                    config.title.c_str(),
                    SDL_WINDOWPOS_UNDEFINED,
                    SDL_WINDOWPOS_UNDEFINED,
                    static_cast<int>(actualWidth),
                    static_cast<int>(actualHeight),
                    windowFlags);
                
                if (!m_window) {
                    LOG_ERROR("Failed to recreate SDL2 window for OpenGL 2.1: " + std::string(SDL_GetError()));
                    SDL_Quit();
                    return false;
                }
                
                m_glContext = SDL_GL_CreateContext(m_window);
                if (m_glContext) {
                    LOG_INFO("SDL2: OpenGL 2.1 context created (fallback)");
                } else {
                    LOG_ERROR("SDL2: Failed to create OpenGL 2.1 context: " + std::string(SDL_GetError()));
                }
            }
        } else {
            LOG_WARN("SDL2: Failed to create OpenGL 3.3 Core context: " + firstError);
            
            // x86: Tentativa 2: OpenGL 2.1 (mais compatível)
            SDL_DestroyWindow(m_window);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0); // Sem profile (compatibility)
            
            m_window = SDL_CreateWindow(
                config.title.c_str(),
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                static_cast<int>(actualWidth),
                static_cast<int>(actualHeight),
                windowFlags);
            
            if (!m_window) {
                LOG_ERROR("Failed to recreate SDL2 window for OpenGL 2.1: " + std::string(SDL_GetError()));
                SDL_Quit();
                return false;
            }
            
            m_glContext = SDL_GL_CreateContext(m_window);
            if (m_glContext) {
                LOG_INFO("SDL2: OpenGL 2.1 context created (fallback)");
            } else {
                LOG_WARN("SDL2: Failed to create OpenGL 2.1 context: " + std::string(SDL_GetError()));
                
                // x86: Tentativa 3: OpenGL ES 2.0 (último recurso)
                SDL_DestroyWindow(m_window);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
                
                m_window = SDL_CreateWindow(
                    config.title.c_str(),
                    SDL_WINDOWPOS_UNDEFINED,
                    SDL_WINDOWPOS_UNDEFINED,
                    static_cast<int>(actualWidth),
                    static_cast<int>(actualHeight),
                    windowFlags);
                
                if (!m_window) {
                    LOG_ERROR("Failed to recreate SDL2 window for OpenGL ES 2.0: " + std::string(SDL_GetError()));
                    SDL_Quit();
                    return false;
                }
                
                m_glContext = SDL_GL_CreateContext(m_window);
                if (m_glContext) {
                    LOG_INFO("SDL2: OpenGL ES 2.0 context created (fallback)");
                } else {
                    LOG_ERROR("SDL2: Failed to create OpenGL ES 2.0 context: " + std::string(SDL_GetError()));
                }
            }
        }
    }
    
    if (!m_glContext)
    {
        LOG_ERROR("Failed to create any OpenGL context");
        LOG_ERROR("SDL2: Dica: Verifique se há drivers OpenGL instalados");
        LOG_ERROR("SDL2: Para X11: sudo apt-get install mesa-utils libgl1-mesa-dev");
        if (m_window) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }
        SDL_Quit();
        return false;
    }

    // Configurar VSync
    if (config.vsync)
    {
        SDL_GL_SetSwapInterval(1);
    }
    else
    {
        SDL_GL_SetSwapInterval(0);
    }

    // Obter dimensões reais da janela
    int w, h;
    SDL_GL_GetDrawableSize(m_window, &w, &h);
    
    // Detectar qual driver está sendo usado para logging
    const char* current_driver = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    std::string driver_name = current_driver ? current_driver : "unknown";
    
    LOG_INFO("SDL2: Resolução da janela: " + 
             std::to_string(w) + "x" + std::to_string(h) + 
             " (driver: " + driver_name + ")");
    
    m_width = static_cast<uint32_t>(w > 0 ? w : actualWidth);
    m_height = static_cast<uint32_t>(h > 0 ? h : actualHeight);

    m_initialized = true;
    m_shouldClose = false;

    LOG_INFO("SDL2 window created: " + std::to_string(m_width) + "x" + std::to_string(m_height) +
             (config.fullscreen ? " (fullscreen)" : " (windowed)"));

    return true;
}

void WindowManagerSDL::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    if (m_glContext)
    {
        // SDL_GLContext é typedef void*, então podemos usar diretamente
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }

    if (m_window)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    m_initialized = false;
    LOG_INFO("WindowManagerSDL shutdown");
}

bool WindowManagerSDL::shouldClose() const
{
    return m_shouldClose;
}

void WindowManagerSDL::swapBuffers()
{
    if (m_window)
    {
        SDL_GL_SwapWindow(m_window);
    }
}

void WindowManagerSDL::pollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            m_shouldClose = true;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED)
            {
                int w = event.window.data1;
                int h = event.window.data2;
                m_width = static_cast<uint32_t>(w);
                m_height = static_cast<uint32_t>(h);
                
                if (m_resizeCallback)
                {
                    m_resizeCallback(w, h);
                }
            }
            else if (event.window.event == SDL_WINDOWEVENT_ENTER || 
                     event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            {
                // When mouse enters window or window gains focus, cursor visibility
                // should be updated based on UI visibility (handled in main loop)
            }
            break;
        case SDL_KEYDOWN:
            // F12 para toggle UI (similar ao GLFW)
            if (event.key.keysym.sym == SDLK_F12)
            {
                m_f12Pressed = true;
            }
            break;
        case SDL_KEYUP:
            if (event.key.keysym.sym == SDLK_F12)
            {
                m_f12Pressed = false;
            }
            break;
        }
    }
}

void WindowManagerSDL::makeCurrent()
{
    if (m_window && m_glContext)
    {
        // SDL_GLContext é typedef void*, então podemos usar diretamente
        SDL_GL_MakeCurrent(m_window, m_glContext);
    }
}

void WindowManagerSDL::setFullscreen(bool fullscreen, int /* monitorIndex */)
{
    if (!m_window)
    {
        return;
    }

    if (fullscreen)
    {
        SDL_SetWindowFullscreen(m_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    else
    {
        SDL_SetWindowFullscreen(m_window, 0);
    }

    // Atualizar dimensões
    int w, h;
    SDL_GL_GetDrawableSize(m_window, &w, &h);
    m_width = static_cast<uint32_t>(w);
    m_height = static_cast<uint32_t>(h);

    if (m_resizeCallback)
    {
        m_resizeCallback(w, h);
    }
}

void WindowManagerSDL::setResizeCallback(std::function<void(int, int)> callback)
{
    m_resizeCallback = callback;
}

bool WindowManagerSDL::isKeyPressed(int keyCode) const
{
    if (!m_window)
    {
        return false;
    }
    
    // SDL2: Get keyboard state and check specific key
    const Uint8* state = SDL_GetKeyboardState(nullptr);
    SDL_Scancode scancode = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(keyCode));
    return state[scancode] != 0;
}

void WindowManagerSDL::setCursorVisible(bool visible)
{
    if (!m_window)
    {
        return;
    }
    
    // SDL_ShowCursor returns the previous state
    // Force the cursor state by calling it twice if needed
    int currentState = SDL_ShowCursor(SDL_QUERY);
    int desiredState = visible ? SDL_ENABLE : SDL_DISABLE;
    
    if (currentState != desiredState)
    {
        SDL_ShowCursor(desiredState);
    }
}

#endif // USE_SDL2
