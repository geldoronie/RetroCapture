#include "Application.h"
#include "../utils/Logger.h"
#include "../capture/VideoCapture.h"
#include "../output/WindowManager.h"
#include "../renderer/OpenGLRenderer.h"
#include "../shader/ShaderEngine.h"
#include "../renderer/glad_loader.h"
#include <linux/videodev2.h>
#include <vector>
#include <cstring>
#include <unistd.h>

Application::Application()
{
}

Application::~Application()
{
    shutdown();
}

bool Application::init()
{
    LOG_INFO("Inicializando Application...");

    if (!initWindow())
    {
        return false;
    }

    if (!initRenderer())
    {
        return false;
    }

    if (!initCapture())
    {
        return false;
    }

    m_initialized = true;
    LOG_INFO("Application inicializada com sucesso");
    return true;
}

bool Application::initWindow()
{
    m_window = new WindowManager();

    WindowConfig config;
    config.width = 1920;
    config.height = 1080;
    config.title = "RetroCapture";
    config.fullscreen = false;
    config.vsync = true;

    if (!m_window->init(config))
    {
        LOG_ERROR("Falha ao inicializar janela");
        delete m_window;
        m_window = nullptr;
        return false;
    }

    m_window->makeCurrent();
    return true;
}

bool Application::initRenderer()
{
    // Garantir que o contexto OpenGL está ativo
    if (m_window)
    {
        m_window->makeCurrent();
    }

    m_renderer = new OpenGLRenderer();

    if (!m_renderer->init())
    {
        LOG_ERROR("Falha ao inicializar renderer");
        delete m_renderer;
        m_renderer = nullptr;
        return false;
    }

    // Inicializar ShaderEngine
    m_shaderEngine = new ShaderEngine();
    if (!m_shaderEngine->init())
    {
        LOG_ERROR("Falha ao inicializar ShaderEngine");
        delete m_shaderEngine;
        m_shaderEngine = nullptr;
        // Não é crítico, podemos continuar sem shaders
    }
    else
    {
        // Carregar shader ou preset se especificado
        if (!m_presetPath.empty())
        {
            if (m_shaderEngine->loadPreset(m_presetPath))
            {
                LOG_INFO("Preset carregado: " + m_presetPath);
            }
            else
            {
                LOG_ERROR("Falha ao carregar preset: " + m_presetPath);
            }
        }
        else if (!m_shaderPath.empty())
        {
            if (m_shaderEngine->loadShader(m_shaderPath))
            {
                LOG_INFO("Shader carregado: " + m_shaderPath);
            }
            else
            {
                LOG_ERROR("Falha ao carregar shader: " + m_shaderPath);
            }
        }
    }

    return true;
}

bool Application::initCapture()
{
    m_capture = new VideoCapture();

    // Abrir dispositivo especificado (ou padrão /dev/video0)
    if (!m_capture->open(m_devicePath))
    {
        LOG_ERROR("Falha ao abrir dispositivo de captura: " + m_devicePath);
        LOG_INFO("Tente especificar outro dispositivo com --device /dev/videoX");
        delete m_capture;
        m_capture = nullptr;
        return false;
    }

    // Configurar formato com parâmetros configuráveis
    LOG_INFO("Configurando captura: " + std::to_string(m_captureWidth) + "x" +
             std::to_string(m_captureHeight) + " @ " + std::to_string(m_captureFps) + "fps");

    if (!m_capture->setFormat(m_captureWidth, m_captureHeight, V4L2_PIX_FMT_YUYV))
    {
        LOG_ERROR("Falha ao configurar formato de captura");
        LOG_WARN("Resolução solicitada pode não ser suportada pelo dispositivo");
        m_capture->close();
        delete m_capture;
        m_capture = nullptr;
        return false;
    }

    // Tentar configurar framerate (não é crítico se falhar)
    if (!m_capture->setFramerate(m_captureFps))
    {
        LOG_WARN("Não foi possível configurar framerate para " + std::to_string(m_captureFps) + "fps");
        LOG_INFO("Usando framerate padrão do dispositivo");
    }

    if (!m_capture->startCapture())
    {
        LOG_ERROR("Falha ao iniciar captura");
        delete m_capture;
        m_capture = nullptr;
        return false;
    }

    LOG_INFO("Captura inicializada: " +
             std::to_string(m_capture->getWidth()) + "x" +
             std::to_string(m_capture->getHeight()));

    return true;
}

void Application::run()
{
    if (!m_initialized)
    {
        LOG_ERROR("Application não inicializada");
        return;
    }

    LOG_INFO("Iniciando loop principal...");

    while (!m_window->shouldClose())
    {
        m_window->pollEvents();

        // Tentar capturar e processar o frame mais recente (descartando frames antigos)
        bool newFrame = false;
        if (m_capture)
        {
            newFrame = processFrame();
        }

        // Sempre renderizar se temos um frame válido
        // Isso garante que estamos sempre mostrando o frame mais recente
        if (m_hasValidFrame && m_texture != 0)
        {
            // Aplicar shader se estiver ativo
            GLuint textureToRender = m_texture;
            bool isShaderTexture = false;

            if (m_shaderEngine && m_shaderEngine->isShaderActive())
            {
                // IMPORTANTE: Atualizar viewport com as dimensões da janela antes de aplicar o shader
                // Isso garante que o último pass renderize para o tamanho correto da janela
                m_shaderEngine->setViewport(m_window->getWidth(), m_window->getHeight());

                textureToRender = m_shaderEngine->applyShader(m_texture, m_textureWidth, m_textureHeight);
                isShaderTexture = true;

                // DEBUG: Verificar textura retornada
                if (textureToRender == 0)
                {
                    LOG_WARN("Shader retornou textura inválida (0), usando textura original");
                    textureToRender = m_texture;
                    isShaderTexture = false;
                }
                else
                {
                    // DEBUG: Log das dimensões
                    LOG_INFO("Renderizando textura do shader: " + std::to_string(textureToRender) +
                             ", janela: " + std::to_string(m_window->getWidth()) + "x" + std::to_string(m_window->getHeight()));
                }
            }

            // Limpar o framebuffer da janela antes de renderizar
            // IMPORTANTE: O framebuffer 0 é a janela (default framebuffer)
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // IMPORTANTE: Resetar viewport para o tamanho completo da janela
            // Isso garante que a textura seja renderizada em toda a janela
            glViewport(0, 0, m_window->getWidth(), m_window->getHeight());

            // IMPORTANTE: Para shaders com alpha (como Game Boy), não limpar com preto opaco
            // Limpar com preto transparente para que o blending funcione corretamente
            if (isShaderTexture)
            {
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Transparente para shaders com alpha
            }
            else
            {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Opaco para captura normal
            }
            glClear(GL_COLOR_BUFFER_BIT);

            // Para texturas do shader (framebuffer), inverter Y (shader renderiza invertido)
            // Para textura original (câmera), não inverter Y (já está correta)
            // IMPORTANTE: Se for textura do shader, pode precisar de blending para alpha
            m_renderer->renderTexture(textureToRender, m_window->getWidth(), m_window->getHeight(), isShaderTexture, isShaderTexture, m_brightness);
            m_window->swapBuffers();
        }
        else
        {
            // Se não há frame válido ainda, fazer um pequeno sleep
            usleep(1000); // 1ms
        }
    }

    LOG_INFO("Loop principal encerrado");
}

bool Application::processFrame()
{
    if (!m_capture)
    {
        return false;
    }

    Frame frame;
    // Usar captureLatestFrame para descartar frames antigos e pegar apenas o mais recente
    if (!m_capture->captureLatestFrame(frame))
    {
        return false; // Nenhum frame novo disponível
    }

    // Se a textura ainda não foi criada ou o tamanho mudou
    bool textureCreated = false;
    if (m_texture == 0 || m_textureWidth != frame.width || m_textureHeight != frame.height)
    {
        if (m_texture != 0)
        {
            glDeleteTextures(1, &m_texture);
        }

        m_textureWidth = frame.width;
        m_textureHeight = frame.height;

        // Criar textura vazia primeiro
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        textureCreated = true;
        LOG_INFO("Textura criada: " + std::to_string(m_textureWidth) + "x" + std::to_string(m_textureHeight));
    }

    // Converter e atualizar textura
    glBindTexture(GL_TEXTURE_2D, m_texture);

    if (frame.format == V4L2_PIX_FMT_YUYV)
    {
        // Converter YUYV para RGB
        std::vector<uint8_t> rgbBuffer(frame.width * frame.height * 3);
        convertYUYVtoRGB(frame.data, rgbBuffer.data(), frame.width, frame.height);

        if (textureCreated)
        {
            // Primeira vez: usar glTexImage2D
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
        }
        else
        {
            // Atualização: usar glTexSubImage2D (mais rápido, não realoca)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
        }
    }
    else
    {
        // Usar formato diretamente (se suportado)
        if (textureCreated)
        {
            m_renderer->updateTexture(m_texture, frame.data, frame.width, frame.height, frame.format);
        }
        else
        {
            // Para outros formatos, ainda precisamos atualizar via renderer
            m_renderer->updateTexture(m_texture, frame.data, frame.width, frame.height, frame.format);
        }
    }

    m_hasValidFrame = true;
    return true; // Frame processado com sucesso
}

void Application::convertYUYVtoRGB(const uint8_t *yuyv, uint8_t *rgb, uint32_t width, uint32_t height)
{
    // Conversão YUYV para RGB
    // YUYV: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            uint32_t idx = y * width * 2 + x * 2;

            int y0 = yuyv[idx];
            int u = yuyv[idx + 1];
            int y1 = yuyv[idx + 2];
            int v = yuyv[idx + 3];

            // Converter primeiro pixel
            int c = y0 - 16;
            int d = u - 128;
            int e = v - 128;

            int r0 = (298 * c + 409 * e + 128) >> 8;
            int g0 = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c + 516 * d + 128) >> 8;

            r0 = (r0 < 0) ? 0 : (r0 > 255) ? 255
                                           : r0;
            g0 = (g0 < 0) ? 0 : (g0 > 255) ? 255
                                           : g0;
            b0 = (b0 < 0) ? 0 : (b0 > 255) ? 255
                                           : b0;

            uint32_t rgbIdx0 = (y * width + x) * 3;
            rgb[rgbIdx0] = r0;
            rgb[rgbIdx0 + 1] = g0;
            rgb[rgbIdx0 + 2] = b0;

            // Converter segundo pixel (mesmo U e V)
            c = y1 - 16;

            int r1 = (298 * c + 409 * e + 128) >> 8;
            int g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c + 516 * d + 128) >> 8;

            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255
                                           : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255
                                           : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255
                                           : b1;

            if (x + 1 < width)
            {
                uint32_t rgbIdx1 = (y * width + x + 1) * 3;
                rgb[rgbIdx1] = r1;
                rgb[rgbIdx1 + 1] = g1;
                rgb[rgbIdx1 + 2] = b1;
            }
        }
    }
}

void Application::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    LOG_INFO("Encerrando Application...");

    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }

    if (m_capture)
    {
        m_capture->stopCapture();
        m_capture->close();
        delete m_capture;
        m_capture = nullptr;
    }

    if (m_shaderEngine)
    {
        m_shaderEngine->shutdown();
        delete m_shaderEngine;
        m_shaderEngine = nullptr;
    }

    if (m_renderer)
    {
        m_renderer->shutdown();
        delete m_renderer;
        m_renderer = nullptr;
    }

    if (m_window)
    {
        m_window->shutdown();
        delete m_window;
        m_window = nullptr;
    }

    m_initialized = false;
}
