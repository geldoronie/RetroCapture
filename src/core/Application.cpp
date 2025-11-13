#include "Application.h"
#include "../utils/Logger.h"
#include "../capture/VideoCapture.h"
#include "../output/WindowManager.h"
#include "../renderer/OpenGLRenderer.h"
#include "../shader/ShaderEngine.h"
#include "../ui/UIManager.h"
#include "../v4l2/V4L2ControlMapper.h"
#include "../processing/FrameProcessor.h"
#include "../renderer/glad_loader.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <linux/videodev2.h>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <filesystem>

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

    if (!initUI())
    {
        return false;
    }

    m_initialized = true;

    // IMPORTANTE: Após inicialização completa, garantir que o viewport está atualizado
    // Isso é especialmente importante quando a janela é criada em fullscreen
    if (m_window && m_shaderEngine)
    {
        uint32_t currentWidth = m_window->getWidth();
        uint32_t currentHeight = m_window->getHeight();
        m_shaderEngine->setViewport(currentWidth, currentHeight);
    }

    LOG_INFO("Application inicializada com sucesso");
    return true;
}

bool Application::initWindow()
{
    m_window = std::make_unique<WindowManager>();

    WindowConfig config;
    config.width = m_windowWidth;
    config.height = m_windowHeight;
    config.title = "RetroCapture";
    config.fullscreen = m_fullscreen;
    config.monitorIndex = m_monitorIndex;
    config.vsync = true;

    if (!m_window->init(config))
    {
        LOG_ERROR("Falha ao inicializar janela");
        m_window.reset();
        return false;
    }

    m_window->makeCurrent();

    // Armazenar ponteiro da Application no WindowManager para uso em callbacks
    m_window->setUserData(this);

    // Configurar callback de resize para atualizar viewport quando a janela for redimensionada
    // ou entrar em fullscreen
    // IMPORTANTE: Este callback é chamado pelo GLFW quando a janela muda de tamanho,
    // incluindo quando entra em fullscreen
    // IMPORTANTE: O ShaderEngine ainda não foi inicializado aqui, então vamos atualizar
    // o callback depois que o ShaderEngine for criado
    // Por enquanto, vamos apenas armazenar o ponteiro

    return true;
}

bool Application::initRenderer()
{
    // Garantir que o contexto OpenGL está ativo
    if (m_window)
    {
        m_window->makeCurrent();
    }

    m_renderer = std::make_unique<OpenGLRenderer>();

    if (!m_renderer->init())
    {
        LOG_ERROR("Falha ao inicializar renderer");
        m_renderer.reset();
        return false;
    }

    // Inicializar FrameProcessor (depois do renderer estar pronto)
    m_frameProcessor = std::make_unique<FrameProcessor>();

    // Inicializar ShaderEngine
    m_shaderEngine = std::make_unique<ShaderEngine>();
    if (!m_shaderEngine->init())
    {
        LOG_ERROR("Falha ao inicializar ShaderEngine");
        m_shaderEngine.reset();
        // Não é crítico, podemos continuar sem shaders
    }
    else
    {
        // IMPORTANTE: Atualizar viewport do ShaderEngine com as dimensões atuais da janela
        // Isso é especialmente importante quando a janela é criada em fullscreen
        // O callback de resize pode não ser chamado imediatamente na criação
        if (m_window)
        {
            uint32_t currentWidth = m_window->getWidth();
            uint32_t currentHeight = m_window->getHeight();
            m_shaderEngine->setViewport(currentWidth, currentHeight);
        }

        // IMPORTANTE: Agora que o ShaderEngine está inicializado, configurar o callback de resize
        // para atualizar o viewport quando a janela for redimensionada ou entrar em fullscreen
        if (m_window)
        {
            Application *appPtr = this;
            m_window->setResizeCallback([appPtr](int width, int height)
                                        {
                // IMPORTANTE: Atualizar viewport do ShaderEngine imediatamente quando resize acontece
                // Isso é especialmente crítico quando entra em fullscreen
                if (appPtr && appPtr->m_shaderEngine) {
                    appPtr->m_shaderEngine->setViewport(width, height);
                } });
        }

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
    m_capture = std::make_unique<VideoCapture>();

    // Abrir dispositivo especificado (ou padrão /dev/video0)
    if (!m_capture->open(m_devicePath))
    {
        LOG_ERROR("Falha ao abrir dispositivo de captura: " + m_devicePath);
        LOG_INFO("Tente especificar outro dispositivo com --device /dev/videoX");
        m_capture.reset();
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
        m_capture.reset();
        return false;
    }

    // Tentar configurar framerate (não é crítico se falhar)
    if (!m_capture->setFramerate(m_captureFps))
    {
        LOG_WARN("Não foi possível configurar framerate para " + std::to_string(m_captureFps) + "fps");
        LOG_INFO("Usando framerate padrão do dispositivo");
    }

    // Configurar controles V4L2 se especificados
    if (m_v4l2Brightness >= 0)
    {
        if (m_capture->setBrightness(m_v4l2Brightness))
        {
            LOG_INFO("Brilho V4L2 configurado: " + std::to_string(m_v4l2Brightness));
        }
    }
    if (m_v4l2Contrast >= 0)
    {
        if (m_capture->setContrast(m_v4l2Contrast))
        {
            LOG_INFO("Contraste V4L2 configurado: " + std::to_string(m_v4l2Contrast));
        }
    }
    if (m_v4l2Saturation >= 0)
    {
        if (m_capture->setSaturation(m_v4l2Saturation))
        {
            LOG_INFO("Saturação V4L2 configurada: " + std::to_string(m_v4l2Saturation));
        }
    }
    if (m_v4l2Hue >= 0)
    {
        if (m_capture->setHue(m_v4l2Hue))
        {
            LOG_INFO("Matiz V4L2 configurado: " + std::to_string(m_v4l2Hue));
        }
    }
    if (m_v4l2Gain >= 0)
    {
        if (m_capture->setGain(m_v4l2Gain))
        {
            LOG_INFO("Ganho V4L2 configurado: " + std::to_string(m_v4l2Gain));
        }
    }
    if (m_v4l2Exposure >= 0)
    {
        if (m_capture->setExposure(m_v4l2Exposure))
        {
            LOG_INFO("Exposição V4L2 configurada: " + std::to_string(m_v4l2Exposure));
        }
    }
    if (m_v4l2Sharpness >= 0)
    {
        if (m_capture->setSharpness(m_v4l2Sharpness))
        {
            LOG_INFO("Nitidez V4L2 configurada: " + std::to_string(m_v4l2Sharpness));
        }
    }
    if (m_v4l2Gamma >= 0)
    {
        if (m_capture->setGamma(m_v4l2Gamma))
        {
            LOG_INFO("Gama V4L2 configurado: " + std::to_string(m_v4l2Gamma));
        }
    }
    if (m_v4l2WhiteBalance >= 0)
    {
        if (m_capture->setWhiteBalanceTemperature(m_v4l2WhiteBalance))
        {
            LOG_INFO("Balanço de branco V4L2 configurado: " + std::to_string(m_v4l2WhiteBalance));
        }
    }

    if (!m_capture->startCapture())
    {
        LOG_ERROR("Falha ao iniciar captura");
        m_capture.reset();
        return false;
    }

    LOG_INFO("Captura inicializada: " +
             std::to_string(m_capture->getWidth()) + "x" +
             std::to_string(m_capture->getHeight()));

    return true;
}

bool Application::reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps)
{
    if (!m_capture || !m_capture->isOpen())
    {
        LOG_ERROR("Captura não está aberta, não é possível reconfigurar");
        return false;
    }

    LOG_INFO("Reconfigurando captura: " + std::to_string(width) + "x" +
             std::to_string(height) + " @ " + std::to_string(fps) + "fps");

    // Salvar valores atuais para rollback se necessário
    uint32_t oldWidth = m_captureWidth;
    uint32_t oldHeight = m_captureHeight;
    uint32_t oldFps = m_captureFps;
    std::string devicePath = m_devicePath;

    // IMPORTANTE: Fechar e reabrir o dispositivo completamente
    // Isso é necessário porque alguns drivers V4L2 não permitem mudar
    // a resolução sem fechar o dispositivo
    LOG_INFO("Fechando dispositivo para reconfiguração...");
    m_capture->stopCapture();
    m_capture->close();

    // Pequeno delay para garantir que o dispositivo foi liberado
    usleep(100000); // 100ms

    // Reabrir dispositivo
    LOG_INFO("Reabrindo dispositivo...");
    if (!m_capture->open(devicePath))
    {
        LOG_ERROR("Falha ao reabrir dispositivo após reconfiguração");
        return false;
    }

    // Configurar novo formato
    if (!m_capture->setFormat(width, height, V4L2_PIX_FMT_YUYV))
    {
        LOG_ERROR("Falha ao configurar novo formato de captura");
        // Tentar rollback: reabrir com formato anterior
        m_capture->close();
        usleep(100000);
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, V4L2_PIX_FMT_YUYV);
            m_capture->setFramerate(oldFps);
            m_capture->startCapture();
        }
        return false;
    }

    // Obter dimensões reais (o driver pode ter ajustado)
    uint32_t actualWidth = m_capture->getWidth();
    uint32_t actualHeight = m_capture->getHeight();

    // Configurar framerate
    if (!m_capture->setFramerate(fps))
    {
        LOG_WARN("Não foi possível configurar framerate para " + std::to_string(fps) + "fps");
        LOG_INFO("Usando framerate padrão do dispositivo");
    }

    // Reiniciar captura (isso cria os buffers com o novo formato)
    if (!m_capture->startCapture())
    {
        LOG_ERROR("Falha ao reiniciar captura após reconfiguração");
        // Tentar rollback
        m_capture->stopCapture();
        m_capture->close();
        usleep(100000);
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, V4L2_PIX_FMT_YUYV);
            m_capture->setFramerate(oldFps);
            m_capture->startCapture();
        }
        return false;
    }

    // Atualizar dimensões internas com valores reais
    m_captureWidth = actualWidth;
    m_captureHeight = actualHeight;
    m_captureFps = fps;

    // IMPORTANTE: Descarta alguns frames iniciais após reconfiguração
    // Os primeiros frames podem estar com dados antigos ou inválidos
    // Isso garante que quando o loop principal tentar processar, já teremos frames válidos
    LOG_INFO("Descartando frames iniciais após reconfiguração...");
    Frame dummyFrame;
    for (int i = 0; i < 5; ++i)
    {
        m_capture->captureLatestFrame(dummyFrame);
        usleep(10000); // 10ms entre tentativas
    }

    LOG_INFO("Captura reconfigurada com sucesso: " +
             std::to_string(actualWidth) + "x" +
             std::to_string(actualHeight) + " @ " + std::to_string(fps) + "fps");

    return true;
}

bool Application::initUI()
{
    m_ui = std::make_unique<UIManager>();

    // Obter GLFWwindow* do WindowManager
    GLFWwindow *window = static_cast<GLFWwindow *>(m_window->getWindow());
    if (!window)
    {
        LOG_ERROR("Falha ao obter janela GLFW para ImGui");
        m_ui.reset();
        return false;
    }

    if (!m_ui->init(window))
    {
        LOG_ERROR("Falha ao inicializar UIManager");
        m_ui.reset();
        return false;
    }

    // Configurar callbacks
    m_ui->setOnShaderChanged([this](const std::string &shaderPath)
                             {
        if (m_shaderEngine) {
            if (shaderPath.empty()) {
                m_shaderEngine->disableShader();
                LOG_INFO("Shader desabilitado");
            } else {
                std::filesystem::path fullPath = std::filesystem::current_path() / "shaders" / "shaders_glsl" / shaderPath;
                if (m_shaderEngine->loadPreset(fullPath.string())) {
                    LOG_INFO("Shader carregado via UI: " + shaderPath);
                } else {
                    LOG_ERROR("Falha ao carregar shader via UI: " + shaderPath);
                }
            }
        } });

    m_ui->setOnBrightnessChanged([this](float brightness)
                                 { m_brightness = brightness; });

    m_ui->setOnContrastChanged([this](float contrast)
                               { m_contrast = contrast; });

    m_ui->setOnMaintainAspectChanged([this](bool maintain)
                                     { m_maintainAspect = maintain; });

    m_ui->setOnFullscreenChanged([this](bool fullscreen)
                                 {
        LOG_INFO("Fullscreen toggle solicitado: " + std::string(fullscreen ? "ON" : "OFF"));
        if (m_window) {
            m_window->setFullscreen(fullscreen, m_monitorIndex);
            m_fullscreen = fullscreen;
            
            // Atualizar viewport do shader engine após mudança de fullscreen
            if (m_shaderEngine) {
                uint32_t currentWidth = m_window->getWidth();
                uint32_t currentHeight = m_window->getHeight();
                m_shaderEngine->setViewport(currentWidth, currentHeight);
            }
        } });

    m_ui->setOnMonitorIndexChanged([this](int monitorIndex)
                                   {
        LOG_INFO("Monitor index alterado: " + std::to_string(monitorIndex));
        m_monitorIndex = monitorIndex;
        // Se estiver em fullscreen, atualizar para usar o novo monitor
        if (m_fullscreen && m_window) {
            m_window->setFullscreen(true, monitorIndex);
            
            // Atualizar viewport do shader engine após mudança de monitor
            if (m_shaderEngine) {
                uint32_t currentWidth = m_window->getWidth();
                uint32_t currentHeight = m_window->getHeight();
                m_shaderEngine->setViewport(currentWidth, currentHeight);
            }
        } });

    m_ui->setOnV4L2ControlChanged([this](const std::string &name, int32_t value)
                                  {
        if (!m_capture) return;
        
        // Mapear nome para control ID usando V4L2ControlMapper
        uint32_t cid = V4L2ControlMapper::nameToControlId(name);
        
        if (cid != 0) {
            // Obter range real do dispositivo para validar
            int32_t currentValue, min, max, step;
            if (m_capture->getControl(cid, currentValue, min, max, step)) {
                // Alinhar valor com step
                if (step > 1) {
                    value = ((value - min) / step) * step + min;
                }
                // Clamp ao range
                value = std::max(min, std::min(max, value));
            }
            
            m_capture->setControl(cid, value);
        } });

    m_ui->setOnResolutionChanged([this](uint32_t width, uint32_t height)
                                 {
        LOG_INFO("Resolução alterada via UI: " + std::to_string(width) + "x" + std::to_string(height));
        if (reconfigureCapture(width, height, m_captureFps)) {
            // Atualizar textura se necessário (usar valores reais do dispositivo)
            uint32_t actualWidth = m_capture->getWidth();
            uint32_t actualHeight = m_capture->getHeight();
            
            // Sempre deletar e recriar a textura após reconfiguração
            if (m_texture != 0) {
                glDeleteTextures(1, &m_texture);
                m_texture = 0;
            }
            
            // Resetar estado para forçar recriação da textura no próximo frame
            m_textureWidth = 0;
            m_textureHeight = 0;
            m_hasValidFrame = false;
            
            // Atualizar informações na UI com valores reais
            if (m_ui && m_capture) {
                m_ui->setCaptureInfo(actualWidth, actualHeight, 
                                    m_captureFps, m_devicePath);
            }
            
            LOG_INFO("Textura será recriada no próximo frame: " + 
                     std::to_string(actualWidth) + "x" + std::to_string(actualHeight));
        } else {
            // Se falhou, atualizar UI com valores atuais
            if (m_ui && m_capture) {
                m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                    m_captureFps, m_devicePath);
            }
        } });

    m_ui->setOnFramerateChanged([this](uint32_t fps)
                                {
        LOG_INFO("Framerate alterado via UI: " + std::to_string(fps) + "fps");
        if (reconfigureCapture(m_captureWidth, m_captureHeight, fps)) {
            m_captureFps = fps;
            // Atualizar informações na UI
            if (m_ui && m_capture) {
                m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                    m_captureFps, m_devicePath);
            }
        } });

    // Configurar valores iniciais
    m_ui->setBrightness(m_brightness);
    m_ui->setContrast(m_contrast);
    m_ui->setMaintainAspect(m_maintainAspect);
    m_ui->setFullscreen(m_fullscreen);
    m_ui->setMonitorIndex(m_monitorIndex);

    // Configurar controles V4L2
    if (m_capture)
    {
        m_ui->setV4L2Controls(m_capture.get());
    }

    // Configurar informações da captura
    if (m_capture)
    {
        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                             m_captureFps, m_devicePath);
        m_ui->setCurrentDevice(m_devicePath);
    }

    // Callback para mudança de dispositivo
    m_ui->setOnDeviceChanged([this](const std::string &devicePath)
                             {
        LOG_INFO("Mudando dispositivo para: " + devicePath);
        
        // Salvar configurações atuais
        uint32_t oldWidth = m_captureWidth;
        uint32_t oldHeight = m_captureHeight;
        uint32_t oldFps = m_captureFps;
        
        // Fechar dispositivo atual
        if (m_capture) {
            m_capture->stopCapture();
            m_capture->close();
        }
        
        // Atualizar caminho do dispositivo
        m_devicePath = devicePath;
        
        // Reabrir com novo dispositivo
        if (m_capture && m_capture->open(devicePath)) {
            // Reconfigurar formato e framerate
            if (m_capture->setFormat(oldWidth, oldHeight, V4L2_PIX_FMT_YUYV)) {
                m_capture->setFramerate(oldFps);
                m_capture->startCapture();
                
                // Atualizar informações na UI
                m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                    m_captureFps, devicePath);
                
                // Recarregar controles V4L2
                m_ui->setV4L2Controls(m_capture.get());
                
                LOG_INFO("Dispositivo alterado com sucesso");
            } else {
                LOG_ERROR("Falha ao configurar formato no novo dispositivo");
            }
        } else {
            LOG_ERROR("Falha ao abrir novo dispositivo: " + devicePath);
        } });

    // Configurar shader atual
    if (!m_presetPath.empty())
    {
        std::filesystem::path presetPath(m_presetPath);
        std::filesystem::path basePath("shaders/shaders_glsl");
        std::filesystem::path relativePath = std::filesystem::relative(presetPath, basePath);
        if (!relativePath.empty() && relativePath != presetPath)
        {
            m_ui->setCurrentShader(relativePath.string());
        }
        else
        {
            m_ui->setCurrentShader(m_presetPath);
        }
    }

    // Conectar ShaderEngine à UI para parâmetros
    if (m_shaderEngine)
    {
        m_ui->setShaderEngine(m_shaderEngine.get());
    }

    // Callback para salvar preset
    m_ui->setOnSavePreset([this](const std::string &path, bool overwrite)
                          {
        if (!m_shaderEngine || !m_shaderEngine->isShaderActive()) {
            LOG_WARN("Nenhum preset carregado para salvar");
            return;
        }
        
        // Obter parâmetros customizados do ShaderEngine
        auto params = m_shaderEngine->getShaderParameters();
        std::unordered_map<std::string, float> customParams;
        for (const auto& param : params) {
            // Salvar todos os valores (mesmo se iguais ao padrão, para preservar configuração)
            customParams[param.name] = param.value;
        }
        
        // Salvar preset
        const ShaderPreset& preset = m_shaderEngine->getPreset();
        if (overwrite) {
            // Salvar por cima
            if (preset.save(path, customParams)) {
                LOG_INFO("Preset salvo: " + path);
            } else {
                LOG_ERROR("Falha ao salvar preset: " + path);
            }
        } else {
            // Salvar como novo arquivo
            if (preset.saveAs(path, customParams)) {
                LOG_INFO("Preset salvo como: " + path);
            } else {
                LOG_ERROR("Falha ao salvar preset como: " + path);
            }
        } });

    LOG_INFO("UIManager inicializado");
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

    // IMPORTANTE: Garantir que o viewport está atualizado antes do primeiro frame
    // Isso é especialmente importante quando a janela é criada em fullscreen
    if (m_shaderEngine)
    {
        uint32_t currentWidth = m_window->getWidth();
        uint32_t currentHeight = m_window->getHeight();
        m_shaderEngine->setViewport(currentWidth, currentHeight);
    }

    while (!m_window->shouldClose())
    {
        m_window->pollEvents();

        // Processar entrada de teclado (F12 para toggle UI)
        handleKeyInput();

        // Iniciar frame do ImGui
        if (m_ui)
        {
            m_ui->beginFrame();
        }

        // Tentar capturar e processar o frame mais recente (descartando frames antigos)
        bool newFrame = false;
        if (m_capture)
        {
            // Tentar processar frame várias vezes se não temos textura válida
            // Isso é importante após reconfiguração quando a textura foi deletada
            int maxAttempts = (m_texture == 0 && !m_hasValidFrame) ? 5 : 1;
            for (int attempt = 0; attempt < maxAttempts; ++attempt)
            {
                newFrame = processFrame();
                if (newFrame && m_hasValidFrame && m_texture != 0)
                {
                    break; // Frame processado com sucesso
                }
                if (attempt < maxAttempts - 1)
                {
                    usleep(5000); // 5ms entre tentativas
                }
            }
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
                // IMPORTANTE: Sempre usar dimensões atuais, especialmente quando entra em fullscreen
                uint32_t currentWidth = m_window->getWidth();
                uint32_t currentHeight = m_window->getHeight();
                m_shaderEngine->setViewport(currentWidth, currentHeight);

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
                    // Log removido para reduzir verbosidade
                }
            }

            // Limpar o framebuffer da janela antes de renderizar
            // IMPORTANTE: O framebuffer 0 é a janela (default framebuffer)
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // IMPORTANTE: Resetar viewport para o tamanho completo da janela
            // Isso garante que a textura seja renderizada em toda a janela
            // IMPORTANTE: Sempre atualizar viewport com as dimensões atuais da janela
            // Isso é especialmente importante quando entra em fullscreen
            uint32_t currentWidth = m_window->getWidth();
            uint32_t currentHeight = m_window->getHeight();

            // DEBUG: Log para verificar se as dimensões mudaram
            static uint32_t lastViewportWidth = 0, lastViewportHeight = 0;
            if (currentWidth != lastViewportWidth || currentHeight != lastViewportHeight)
            {
                // Log removido para reduzir verbosidade
                lastViewportWidth = currentWidth;
                lastViewportHeight = currentHeight;
            }

            glViewport(0, 0, currentWidth, currentHeight);

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
            // Obter dimensões da textura para calcular aspect ratio
            // IMPORTANTE: Para maintainAspect, sempre usar as dimensões da CAPTURA ORIGINAL
            // porque o shader processa a imagem mas mantém a mesma proporção
            // A textura de saída do shader pode ter dimensões da janela (viewport), não da imagem
            uint32_t renderWidth, renderHeight;
            if (isShaderTexture && m_maintainAspect)
            {
                // Para maintainAspect com shader, usar dimensões da captura original
                // O shader processa mas mantém a proporção da imagem original
                renderWidth = m_textureWidth;
                renderHeight = m_textureHeight;
                // Log removido para reduzir verbosidade
            }
            else if (isShaderTexture)
            {
                // Sem maintainAspect, usar dimensões de saída do shader
                renderWidth = m_shaderEngine->getOutputWidth();
                renderHeight = m_shaderEngine->getOutputHeight();
                if (renderWidth == 0 || renderHeight == 0)
                {
                    LOG_WARN("Dimensões de saída do shader inválidas (0x0), usando dimensões da captura");
                    renderWidth = m_textureWidth;
                    renderHeight = m_textureHeight;
                }
            }
            else
            {
                // Sem shader, usar dimensões da captura
                renderWidth = m_textureWidth;
                renderHeight = m_textureHeight;
            }

            // IMPORTANTE: A imagem da câmera vem invertida (Y invertido)
            // Shaders também renderizam invertido, então ambos precisam de inversão Y
            // flipY: true para ambos (câmera e shader precisam inverter)
            bool shouldFlipY = true;
            m_renderer->renderTexture(textureToRender, m_window->getWidth(), m_window->getHeight(),
                                      shouldFlipY, isShaderTexture, m_brightness, m_contrast,
                                      m_maintainAspect, renderWidth, renderHeight);

            // Renderizar UI
            if (m_ui)
            {
                m_ui->render();
                // IMPORTANTE: endFrame() deve ser chamado ANTES do swapBuffers()
                // para que a UI seja renderizada no buffer correto
                m_ui->endFrame();
            }

            m_window->swapBuffers();
        }
        else
        {
            // Se não há frame válido ainda, fazer um pequeno sleep
            usleep(1000); // 1ms

            // IMPORTANTE: Sempre finalizar o frame do ImGui, mesmo se não renderizarmos nada
            // Isso evita o erro "Forgot to call Render() or EndFrame()"
            if (m_ui)
            {
                m_ui->endFrame();
            }
        }
    }

    LOG_INFO("Loop principal encerrado");
}

bool Application::processFrame()
{
    if (!m_capture || !m_frameProcessor)
    {
        return false;
    }

    Frame frame;
    // Usar captureLatestFrame para descartar frames antigos e pegar apenas o mais recente
    if (!m_capture->captureLatestFrame(frame))
    {
        return false; // Nenhum frame novo disponível
    }

    // Processar frame usando FrameProcessor
    GLuint outputTexture = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    if (!m_frameProcessor->processFrame(frame, 
                                        m_texture,
                                        m_textureWidth,
                                        m_textureHeight,
                                        m_renderer.get(),
                                        outputTexture,
                                        outputWidth,
                                        outputHeight))
    {
        return false; // Falha ao processar frame
    }

    // Atualizar estado com resultados
    m_texture = outputTexture;
    m_textureWidth = outputWidth;
    m_textureHeight = outputHeight;
    m_hasValidFrame = true;

    return true; // Frame processado com sucesso
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
    }

    if (m_shaderEngine)
    {
        m_shaderEngine->shutdown();
    }

    if (m_renderer)
    {
        m_renderer->shutdown();
    }

    if (m_ui)
    {
        m_ui->shutdown();
    }

    if (m_window)
    {
        m_window->shutdown();
    }

    // Smart pointers will automatically clean up when reset or destroyed
    m_capture.reset();
    m_shaderEngine.reset();
    m_renderer.reset();
    m_ui.reset();
    m_window.reset();

    m_initialized = false;
}

void Application::handleKeyInput()
{
    if (!m_ui || !m_window)
        return;

    GLFWwindow *window = static_cast<GLFWwindow *>(m_window->getWindow());
    if (!window)
        return;

    // F12 para toggle UI
    static bool f12Pressed = false;
    if (glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS)
    {
        if (!f12Pressed)
        {
            m_ui->toggle();
            LOG_INFO("UI toggled: " + std::string(m_ui->isVisible() ? "VISIBLE" : "HIDDEN"));
            f12Pressed = true;
        }
    }
    else
    {
        f12Pressed = false;
    }
}
