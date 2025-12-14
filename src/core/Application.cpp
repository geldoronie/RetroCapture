#include "Application.h"
#include "../utils/Logger.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureFactory.h"
#ifdef PLATFORM_LINUX
#include "../v4l2/V4L2ControlMapper.h"
#include "../processing/FrameProcessor.h"
#include "../renderer/OpenGLRenderer.h"
#else
// Windows: criar stubs ou implementações alternativas
// Nota: OpenGLRenderer e FrameProcessor são específicos do Linux (usam V4L2)
// No Windows, o processamento de frames pode ser diferente
class FrameProcessor
{
public:
    void init(void *) {}
    void deleteTexture() {}
    bool hasValidFrame() const { return false; }
    GLuint getTexture() const { return 0; }
    uint32_t getTextureWidth() const { return 0; }
    uint32_t getTextureHeight() const { return 0; }
    bool processFrame(void *) { return false; }
};

class OpenGLRenderer
{
public:
    bool init() { return true; }
    void shutdown() {}
    void renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight,
                       bool shouldFlipY, bool isShaderTexture, float brightness, float contrast,
                       bool maintainAspect, uint32_t renderWidth, uint32_t renderHeight)
    {
        // Stub para Windows - implementação real seria diferente
        (void)texture;
        (void)windowWidth;
        (void)windowHeight;
        (void)shouldFlipY;
        (void)isShaderTexture;
        (void)brightness;
        (void)contrast;
        (void)maintainAspect;
        (void)renderWidth;
        (void)renderHeight;
    }
};
#endif
#include "../output/WindowManager.h"
#include "../shader/ShaderEngine.h"
#include "../ui/UIManager.h"
#include "../renderer/glad_loader.h"
#include "../streaming/StreamManager.h"
#include "../streaming/HTTPTSStreamer.h"
#include "../audio/IAudioCapture.h"
#include "../audio/AudioCaptureFactory.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef PLATFORM_LINUX
#include <linux/videodev2.h>
#endif
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#ifdef PLATFORM_LINUX
#include <unistd.h>
#endif
#include "../utils/FilesystemCompat.h"
#include <time.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

// swscale removido - resize agora é feito no encoding (HTTPTSStreamer)

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
    LOG_INFO("Janela inicializada");

    if (!initRenderer())
    {
        return false;
    }
    LOG_INFO("Renderer inicializado");

    if (!initCapture())
    {
        LOG_WARN("Falha ao inicializar captura - continuando em modo dummy");
        // Não retornar false, continuar em modo dummy
    }
    else
    {
        LOG_INFO("Captura inicializada");
    }

    if (!initUI())
    {
        return false;
    }
    LOG_INFO("UI inicializada");

    // Conectar ShaderEngine à UI para parâmetros
    if (m_ui && m_shaderEngine)
    {
        m_ui->setShaderEngine(m_shaderEngine.get());
    }

    if (!initStreaming())
    {
        LOG_WARN("Falha ao inicializar streaming - continuando sem streaming");
    }

    // Initialize audio capture (sempre necessário para streaming)
    if (m_streamingEnabled)
    {
        if (!initAudioCapture())
        {
            LOG_WARN("Falha ao inicializar captura de áudio - continuando sem áudio");
        }
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
    // IMPORTANTE: Desabilitar VSync para evitar bloqueio quando janela não está focada
    // VSync pode causar pausa na aplicação quando a janela está em segundo plano
    // Isso garante que captura e streaming continuem funcionando mesmo quando não focada
    config.vsync = false;

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
    LOG_INFO("Inicializando renderer...");
    // Garantir que o contexto OpenGL está ativo
    if (m_window)
    {
        LOG_INFO("Fazendo contexto OpenGL atual...");
        m_window->makeCurrent();
        LOG_INFO("Contexto OpenGL ativado");
    }
    else
    {
        LOG_ERROR("Janela não disponível para inicializar renderer");
        return false;
    }

    LOG_INFO("Criando OpenGLRenderer...");
    m_renderer = std::make_unique<OpenGLRenderer>();
    LOG_INFO("OpenGLRenderer criado");

    LOG_INFO("Inicializando OpenGLRenderer...");
    if (!m_renderer->init())
    {
        LOG_ERROR("Falha ao inicializar renderer");
        m_renderer.reset();
        return false;
    }
    LOG_INFO("OpenGLRenderer inicializado");

    // Inicializar FrameProcessor
    LOG_INFO("Criando FrameProcessor...");
    m_frameProcessor = std::make_unique<FrameProcessor>();
    m_frameProcessor->init(m_renderer.get());
    LOG_INFO("FrameProcessor criado");

    // Inicializar ShaderEngine
    LOG_INFO("Criando ShaderEngine...");
    m_shaderEngine = std::make_unique<ShaderEngine>();
    LOG_INFO("ShaderEngine criado, inicializando...");
    if (!m_shaderEngine->init())
    {
        LOG_ERROR("Falha ao inicializar ShaderEngine");
        m_shaderEngine.reset();
        // Não é crítico, podemos continuar sem shaders
    }
    else
    {
        LOG_INFO("ShaderEngine inicializado");
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
                // IMPORTANTE: Validar dimensões antes de atualizar para evitar problemas
                if (appPtr && appPtr->m_shaderEngine && width > 0 && height > 0 && 
                    width <= 7680 && height <= 4320) {
                    appPtr->m_isResizing = true;
                    {
                        std::lock_guard<std::mutex> lock(appPtr->m_resizeMutex);
                        appPtr->m_shaderEngine->setViewport(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                    }
// Pequeno delay para garantir que o ShaderEngine terminou de recriar framebuffers
#ifdef PLATFORM_LINUX
                    usleep(10000); // 10ms
#else
                    Sleep(10); // 10ms
#endif
                    appPtr->m_isResizing = false;
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
    LOG_INFO("Criando VideoCapture...");
    m_capture = VideoCaptureFactory::create();
    if (!m_capture)
    {
        LOG_ERROR("Falha ao criar VideoCapture para esta plataforma");
        return false;
    }
    LOG_INFO("VideoCapture criado com sucesso");

    // Tentar abrir dispositivo especificado
    // No Windows, m_devicePath pode estar vazio ou ser um índice de dispositivo MF
    // No Linux, m_devicePath é o caminho do dispositivo V4L2 (ex: /dev/video0)
    // Se falhar, ativar modo dummy (gera frames pretos)
    if (m_devicePath.empty())
    {
#ifdef _WIN32
        LOG_INFO("Nenhum dispositivo especificado - ativando modo dummy");
#else
        LOG_INFO("Nenhum dispositivo especificado - usando padrão /dev/video0");
        m_devicePath = "/dev/video0";
#endif
    }

    if (!m_capture->open(m_devicePath))
    {
        LOG_WARN("Falha ao abrir dispositivo de captura: " + (m_devicePath.empty() ? "(nenhum)" : m_devicePath));
        LOG_INFO("Ativando modo dummy: gerando frames pretos na resolução especificada.");
#ifdef __linux__
        LOG_INFO("Selecione um dispositivo na aba V4L2 para usar captura real.");
#elif defined(_WIN32)
        LOG_INFO("Selecione um dispositivo na aba Media Foundation para usar captura real.");
#endif

        // Ativar modo dummy
        m_capture->setDummyMode(true);

        // Configurar formato dummy com resolução padrão
        // Nota: V4L2_PIX_FMT_YUYV é específico do V4L2, mas a interface aceita 0 para default
        if (!m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
        {
            LOG_ERROR("Falha ao configurar formato dummy");
            return false;
        }

        // Iniciar captura dummy
        if (!m_capture->startCapture())
        {
            LOG_ERROR("Falha ao iniciar captura dummy");
            return false;
        }

        LOG_INFO("Modo dummy ativado: " + std::to_string(m_capture->getWidth()) + "x" +
                 std::to_string(m_capture->getHeight()));
        return true;
    }

    // Configurar formato com parâmetros configuráveis
    LOG_INFO("Configurando captura: " + std::to_string(m_captureWidth) + "x" +
             std::to_string(m_captureHeight) + " @ " + std::to_string(m_captureFps) + "fps");

    if (!m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
    {
        LOG_ERROR("Falha ao configurar formato de captura");
        LOG_WARN("Resolução solicitada pode não ser suportada pelo dispositivo");

        // Se não estiver em modo dummy, tentar ativar modo dummy como fallback
        if (!m_capture->isDummyMode())
        {
            LOG_INFO("Tentando ativar modo dummy como fallback...");
            m_capture->close();
            m_capture->setDummyMode(true);

            if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
            {
                if (m_capture->startCapture())
                {
                    LOG_INFO("Modo dummy ativado como fallback: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                    return true;
                }
            }
        }

        m_capture->close();
        // Não resetar m_capture - permitir tentar novamente depois
        LOG_INFO("Dispositivo fechado. Selecione outro dispositivo na aba V4L2.");
        return true; // Continuar sem dispositivo
    }

    // Tentar configurar framerate (não é crítico se falhar)
    // Em modo dummy, isso apenas loga mas não configura dispositivo real
    if (!m_capture->setFramerate(m_captureFps))
    {
        if (!m_capture->isDummyMode())
        {
            LOG_WARN("Não foi possível configurar framerate para " + std::to_string(m_captureFps) + "fps");
            LOG_INFO("Usando framerate padrão do dispositivo");
        }
    }

    // Configurar controles de hardware se especificados (usando interface genérica)
    if (m_v4l2Brightness >= 0)
    {
        m_capture->setControl("Brightness", m_v4l2Brightness);
    }
    if (m_v4l2Contrast >= 0)
    {
        m_capture->setControl("Contrast", m_v4l2Contrast);
    }
    if (m_v4l2Saturation >= 0)
    {
        if (m_capture->setControl("Saturation", m_v4l2Saturation))
        {
            LOG_INFO("Saturação configurada: " + std::to_string(m_v4l2Saturation));
        }
    }
    if (m_v4l2Hue >= 0)
    {
        m_capture->setControl("Hue", m_v4l2Hue);
    }
    if (m_v4l2Gain >= 0)
    {
        m_capture->setControl("Gain", m_v4l2Gain);
    }
    if (m_v4l2Exposure >= 0)
    {
        if (m_capture->setControl("Exposure", m_v4l2Exposure))
        {
            LOG_INFO("Exposição configurada: " + std::to_string(m_v4l2Exposure));
        }
    }
    if (m_v4l2Sharpness >= 0)
    {
        if (m_capture->setControl("Sharpness", m_v4l2Sharpness))
        {
            LOG_INFO("Nitidez configurada: " + std::to_string(m_v4l2Sharpness));
        }
    }
    if (m_v4l2Gamma >= 0)
    {
        m_capture->setControl("Gamma", m_v4l2Gamma);
    }
    if (m_v4l2WhiteBalance >= 0)
    {
        m_capture->setControl("White Balance", m_v4l2WhiteBalance);
    }

    if (!m_capture->startCapture())
    {
        LOG_ERROR("Falha ao iniciar captura");

        // Se não estiver em modo dummy, tentar ativar modo dummy como fallback
        if (!m_capture->isDummyMode())
        {
            LOG_INFO("Tentando ativar modo dummy como fallback...");
            m_capture->close();
            m_capture->setDummyMode(true);

            if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
            {
                if (m_capture->startCapture())
                {
                    LOG_INFO("Modo dummy ativado como fallback: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                    return true;
                }
            }
        }

        m_capture->close();
        // Não resetar m_capture - permitir tentar novamente depois
        LOG_INFO("Dispositivo fechado. Selecione outro dispositivo na aba V4L2.");
        return true; // Continuar sem dispositivo
    }

    // Só logar dimensões se o dispositivo estiver aberto
    if (m_capture->isOpen())
    {
        LOG_INFO("Captura inicializada: " +
                 std::to_string(m_capture->getWidth()) + "x" +
                 std::to_string(m_capture->getHeight()));
    }

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
#ifdef PLATFORM_LINUX
    usleep(100000); // 100ms
#else
    Sleep(100); // 100ms
#endif

    // Reabrir dispositivo
    LOG_INFO("Reabrindo dispositivo...");
    if (!m_capture->open(devicePath))
    {
        LOG_ERROR("Falha ao reabrir dispositivo após reconfiguração");
        return false;
    }

    // Configurar novo formato
    if (!m_capture->setFormat(width, height, 0))
    {
        LOG_ERROR("Falha ao configurar novo formato de captura");
        // Tentar rollback: reabrir com formato anterior
        m_capture->close();
#ifdef PLATFORM_LINUX
        usleep(100000); // 100ms
#else
        Sleep(100); // 100ms
#endif
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, 0);
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
#ifdef PLATFORM_LINUX
        usleep(100000); // 100ms
#else
        Sleep(100); // 100ms
#endif
        if (m_capture->open(devicePath))
        {
            m_capture->setFormat(oldWidth, oldHeight, 0);
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
#ifdef PLATFORM_LINUX
        usleep(10000); // 10ms entre tentativas
#else
        Sleep(10); // 10ms entre tentativas
#endif
    }

    LOG_INFO("Captura reconfigurada com sucesso: " +
             std::to_string(actualWidth) + "x" +
             std::to_string(actualHeight) + " @ " + std::to_string(fps) + "fps");

    return true;
}

bool Application::initUI()
{
    // IMPORTANTE: Garantir que o contexto OpenGL está ativo antes de inicializar ImGui
    // O ImGui precisa de um contexto OpenGL válido para inicializar corretamente
    if (m_window)
    {
        m_window->makeCurrent();
    }
    else
    {
        LOG_ERROR("Janela não disponível para inicializar UI");
        return false;
    }

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
                // Usar RETROCAPTURE_SHADER_PATH se disponível (para AppImage)
                const char* envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
                fs::path shaderBasePath;
                if (envShaderPath && fs::exists(envShaderPath)) {
                    shaderBasePath = fs::path(envShaderPath);
                } else {
                    shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
                }
                fs::path fullPath = shaderBasePath / shaderPath;
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
        // IMPORTANTE: Fazer mudança de fullscreen de forma assíncrona para evitar travamento
        // O callback de resize será chamado automaticamente pelo GLFW quando a janela mudar
        // Não fazer operações bloqueantes aqui
        if (m_window) {
            m_fullscreen = fullscreen;
            // A mudança de fullscreen será feita no próximo frame do loop principal
            // para evitar deadlocks e travamentos
            m_pendingFullscreenChange = true;
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
        
        // Usar interface genérica para definir controle
        int32_t minVal, maxVal;
        if (m_capture->getControlMin(name, minVal) && 
            m_capture->getControlMax(name, maxVal)) {
            // Clamp ao range
            value = std::max(minVal, std::min(maxVal, value));
        }
        
        m_capture->setControl(name, value); });

    m_ui->setOnResolutionChanged([this](uint32_t width, uint32_t height)
                                 {
        LOG_INFO("Resolução alterada via UI: " + std::to_string(width) + "x" + std::to_string(height));
        // Se não houver dispositivo aberto, ativar modo dummy
        if (!m_capture || !m_capture->isOpen()) {
            if (!m_capture) {
                LOG_WARN("VideoCapture não inicializado. Selecione um dispositivo primeiro.");
                return;
            }
            
            // Se não estiver em modo dummy, tentar ativar
            if (!m_capture->isDummyMode()) {
                LOG_INFO("Nenhum dispositivo aberto. Ativando modo dummy...");
                m_capture->setDummyMode(true);
            }
            
            // Configurar formato dummy
            if (m_capture->setFormat(width, height, 0)) {
                if (m_capture->startCapture()) {
                    LOG_INFO("Resolução dummy atualizada: " + std::to_string(width) + "x" + std::to_string(height));
                    if (m_ui) {
                        m_ui->setCaptureInfo(width, height, m_captureFps, "None (Dummy)");
                    }
                    return;
                }
            }
            LOG_WARN("Falha ao configurar resolução dummy. Selecione um dispositivo primeiro.");
            return;
        }
        if (reconfigureCapture(width, height, m_captureFps)) {
            // Atualizar textura se necessário (usar valores reais do dispositivo)
            uint32_t actualWidth = m_capture->getWidth();
            uint32_t actualHeight = m_capture->getHeight();
            
            // Sempre deletar e recriar a textura após reconfiguração
            if (m_frameProcessor) {
                m_frameProcessor->deleteTexture();
            }
            
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
        // Atualizar FPS na configuração
        m_captureFps = fps;
        
        // Se não houver dispositivo aberto, apenas atualizar configuração (modo dummy não precisa reconfigurar)
        if (!m_capture || !m_capture->isOpen()) {
            if (m_capture && m_capture->isDummyMode()) {
                LOG_INFO("Framerate atualizado para modo dummy: " + std::to_string(fps) + "fps");
            } else {
                LOG_WARN("Nenhum dispositivo aberto. FPS será aplicado quando um dispositivo for selecionado.");
            }
            return;
        }
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

    // Verificar tipo de fonte inicial e configurar adequadamente
    if (m_ui->getSourceType() == UIManager::SourceType::None)
    {
        // Se None estiver selecionado, garantir que modo dummy está ativo
        if (m_capture)
        {
            if (!m_capture->isDummyMode() || !m_capture->isOpen())
            {
                m_capture->stopCapture();
                m_capture->close();
                m_capture->setDummyMode(true);
                if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                {
                    if (m_capture->startCapture())
                    {
                        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                             m_captureFps, "None (Dummy)");
                    }
                }
            }
        }
        m_ui->setV4L2Controls(nullptr);
    }
    else
    {
        // Configurar controles V4L2 se houver dispositivo aberto
        if (m_capture && m_capture->isOpen())
        {
            m_ui->setV4L2Controls(m_capture.get());
        }
        else
        {
            // Sem dispositivo, mas ainda permitir seleção
            m_ui->setV4L2Controls(nullptr);
        }
    }

    // Configurar informações da captura
    if (m_capture && m_capture->isOpen())
    {
        m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                             m_captureFps, m_devicePath);
        m_ui->setCurrentDevice(m_devicePath);
    }
    else
    {
        // Sem dispositivo - mostrar "None"
        m_ui->setCaptureInfo(0, 0, 0, "None");
        m_ui->setCurrentDevice(""); // String vazia = None
    }

    // IMPORTANTE: Após init(), o UIManager já carregou as configurações salvas
    // Sincronizar valores do Application com os valores carregados da UI
    // Isso garante que as configurações salvas sejam aplicadas
    m_streamingPort = m_ui->getStreamingPort();
    m_streamingWidth = m_ui->getStreamingWidth();
    m_streamingHeight = m_ui->getStreamingHeight();
    m_streamingFps = m_ui->getStreamingFps();
    m_streamingBitrate = m_ui->getStreamingBitrate();
    m_streamingAudioBitrate = m_ui->getStreamingAudioBitrate();
    m_streamingVideoCodec = m_ui->getStreamingVideoCodec();
    m_streamingAudioCodec = m_ui->getStreamingAudioCodec();
    m_streamingH264Preset = m_ui->getStreamingH264Preset();
    m_streamingH265Preset = m_ui->getStreamingH265Preset();
    m_streamingH265Profile = m_ui->getStreamingH265Profile();
    m_streamingH265Level = m_ui->getStreamingH265Level();
    m_streamingVP8Speed = m_ui->getStreamingVP8Speed();
    m_streamingVP9Speed = m_ui->getStreamingVP9Speed();

    // Carregar parâmetros de buffer de streaming
    m_streamingMaxVideoBufferSize = m_ui->getStreamingMaxVideoBufferSize();
    m_streamingMaxAudioBufferSize = m_ui->getStreamingMaxAudioBufferSize();
    m_streamingMaxBufferTimeSeconds = m_ui->getStreamingMaxBufferTimeSeconds();
    m_streamingAVIOBufferSize = m_ui->getStreamingAVIOBufferSize();

    // Carregar configurações de buffer (já carregadas pelo UIManager do arquivo de config)

    // Carregar configurações do Web Portal
    m_webPortalEnabled = m_ui->getWebPortalEnabled();
    m_webPortalHTTPSEnabled = m_ui->getWebPortalHTTPSEnabled();
    m_webPortalSSLCertPath = m_ui->getWebPortalSSLCertPath();
    m_webPortalSSLKeyPath = m_ui->getWebPortalSSLKeyPath();
    m_webPortalTitle = m_ui->getWebPortalTitle();
    m_webPortalSubtitle = m_ui->getWebPortalSubtitle();
    m_webPortalImagePath = m_ui->getWebPortalImagePath();
    m_webPortalBackgroundImagePath = m_ui->getWebPortalBackgroundImagePath();

    // Carregar textos editáveis
    m_webPortalTextStreamInfo = m_ui->getWebPortalTextStreamInfo();
    m_webPortalTextQuickActions = m_ui->getWebPortalTextQuickActions();
    m_webPortalTextCompatibility = m_ui->getWebPortalTextCompatibility();
    m_webPortalTextStatus = m_ui->getWebPortalTextStatus();
    m_webPortalTextCodec = m_ui->getWebPortalTextCodec();
    m_webPortalTextResolution = m_ui->getWebPortalTextResolution();
    m_webPortalTextStreamUrl = m_ui->getWebPortalTextStreamUrl();
    m_webPortalTextCopyUrl = m_ui->getWebPortalTextCopyUrl();
    m_webPortalTextOpenNewTab = m_ui->getWebPortalTextOpenNewTab();
    m_webPortalTextSupported = m_ui->getWebPortalTextSupported();
    m_webPortalTextFormat = m_ui->getWebPortalTextFormat();
    m_webPortalTextCodecInfo = m_ui->getWebPortalTextCodecInfo();
    m_webPortalTextSupportedBrowsers = m_ui->getWebPortalTextSupportedBrowsers();
    m_webPortalTextFormatInfo = m_ui->getWebPortalTextFormatInfo();
    m_webPortalTextCodecInfoValue = m_ui->getWebPortalTextCodecInfoValue();
    m_webPortalTextConnecting = m_ui->getWebPortalTextConnecting();

    // Carregar cores (com verificação de segurança)
    const float *bg = m_ui->getWebPortalColorBackground();
    if (bg)
    {
        memcpy(m_webPortalColorBackground, bg, 4 * sizeof(float));
    }
    const float *txt = m_ui->getWebPortalColorText();
    if (txt)
    {
        memcpy(m_webPortalColorText, txt, 4 * sizeof(float));
    }
    const float *prim = m_ui->getWebPortalColorPrimary();
    if (prim)
    {
        memcpy(m_webPortalColorPrimary, prim, 4 * sizeof(float));
    }
    const float *primLight = m_ui->getWebPortalColorPrimaryLight();
    if (primLight)
    {
        memcpy(m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
    }
    const float *primDark = m_ui->getWebPortalColorPrimaryDark();
    if (primDark)
    {
        memcpy(m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
    }
    const float *sec = m_ui->getWebPortalColorSecondary();
    if (sec)
    {
        memcpy(m_webPortalColorSecondary, sec, 4 * sizeof(float));
    }
    const float *secHighlight = m_ui->getWebPortalColorSecondaryHighlight();
    if (secHighlight)
    {
        memcpy(m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
    }
    const float *ch = m_ui->getWebPortalColorCardHeader();
    if (ch)
    {
        memcpy(m_webPortalColorCardHeader, ch, 4 * sizeof(float));
    }
    const float *bord = m_ui->getWebPortalColorBorder();
    if (bord)
    {
        memcpy(m_webPortalColorBorder, bord, 4 * sizeof(float));
    }
    const float *succ = m_ui->getWebPortalColorSuccess();
    if (succ)
    {
        memcpy(m_webPortalColorSuccess, succ, 4 * sizeof(float));
    }
    const float *warn = m_ui->getWebPortalColorWarning();
    if (warn)
    {
        memcpy(m_webPortalColorWarning, warn, 4 * sizeof(float));
    }
    const float *dang = m_ui->getWebPortalColorDanger();
    if (dang)
    {
        memcpy(m_webPortalColorDanger, dang, 4 * sizeof(float));
    }
    const float *inf = m_ui->getWebPortalColorInfo();
    if (inf)
    {
        memcpy(m_webPortalColorInfo, inf, 4 * sizeof(float));
    }

    // Também sincronizar configurações de imagem
    m_brightness = m_ui->getBrightness();
    m_contrast = m_ui->getContrast();
    m_maintainAspect = m_ui->getMaintainAspect();
    m_fullscreen = m_ui->getFullscreen();
    m_monitorIndex = m_ui->getMonitorIndex();

    // Aplicar shader carregado se houver
    std::string loadedShader = m_ui->getCurrentShader();
    if (!loadedShader.empty() && m_shaderEngine)
    {
        // Usar RETROCAPTURE_SHADER_PATH se disponível (para AppImage)
        const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
        fs::path shaderBasePath;
        if (envShaderPath && fs::exists(envShaderPath))
        {
            shaderBasePath = fs::path(envShaderPath);
        }
        else
        {
            shaderBasePath = fs::current_path() / "shaders" / "shaders_glsl";
        }
        fs::path fullPath = shaderBasePath / loadedShader;
        if (m_shaderEngine->loadPreset(fullPath.string()))
        {
            LOG_INFO("Shader carregado da configuração: " + loadedShader);
        }
    }

    // Aplicar configurações de imagem
    // FrameProcessor aplica brightness/contrast durante o processamento, não precisa setar aqui

    // Aplicar fullscreen se necessário
    if (m_fullscreen && m_window)
    {
        m_window->setFullscreen(m_fullscreen, m_monitorIndex);
    }

    m_ui->setOnStreamingStartStop([this](bool start)
                                  {
        // CRÍTICO: Este callback é executado na thread principal (ImGui render thread)
        // NÃO fazer NENHUMA operação bloqueante aqui - apenas marcar flag e criar thread
        // NÃO acessar m_streamManager ou outros recursos compartilhados aqui
        
        LOG_INFO("[CALLBACK] Streaming " + std::string(start ? "START" : "STOP") + " - criando thread...");
        
        if (start) {
            // Verificar se pode iniciar (não está em cooldown)
            if (m_ui && !m_ui->canStartStreaming()) {
                int64_t cooldownMs = m_ui->getStreamingCooldownRemainingMs();
                int cooldownSeconds = static_cast<int>(cooldownMs / 1000);
                LOG_WARN("Tentativa de iniciar streaming bloqueada - ainda em cooldown. Aguarde " + 
                         std::to_string(cooldownSeconds) + " segundos");
                if (m_ui) {
                    m_ui->setStreamingProcessing(false); // Resetar flag se bloqueado
                }
                return; // Não iniciar se estiver em cooldown
            }
            
            // Apenas marcar flag - thread separada fará todo o trabalho
            m_streamingEnabled = true;
            
            // Atualizar status imediatamente para "iniciando" (será atualizado novamente quando realmente iniciar)
            if (m_ui) {
                m_ui->setStreamingActive(false); // Ainda não está ativo, mas está iniciando
            }
            
            // Criar thread separada imediatamente - não esperar nada
            std::thread([this]() {
                // Todas as operações bloqueantes devem estar aqui, na thread separada
                bool success = false;
                try {
                    if (initStreaming()) {
                        // Initialize audio capture (sempre necessário para streaming)
                        if (!m_audioCapture) {
                            if (!initAudioCapture()) {
                                LOG_WARN("Falha ao inicializar captura de áudio - continuando sem áudio");
                            }
                        }
                        success = true;
                    } else {
                        LOG_ERROR("Falha ao iniciar streaming");
                        m_streamingEnabled = false;
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exceção ao iniciar streaming: " + std::string(e.what()));
                    m_streamingEnabled = false;
                }
                
                // Atualizar UI após inicialização (pode ser chamado de qualquer thread)
                // IMPORTANTE: Verificar se m_streamManager existe antes de chamar isActive()
                if (m_ui) {
                    bool active = success && m_streamManager && m_streamManager->isActive();
                    m_ui->setStreamingActive(active);
                    m_ui->setStreamingProcessing(false); // Processamento concluído
                }
            }).detach();
        } else {
            // Parar streaming também em thread separada para não bloquear a UI
            m_streamingEnabled = false;
            
            // Atualizar status imediatamente quando parar
            if (m_ui) {
                m_ui->setStreamingActive(false);
                m_ui->setStreamUrl("");
                m_ui->setStreamClientCount(0);
            }
            
            // Criar thread separada imediatamente - não esperar nada
            std::thread([this]() {
                try {
                    if (m_streamManager) {
                        // Ordem correta: stop() primeiro, depois cleanup()
                        m_streamManager->stop();
                        m_streamManager->cleanup();
                        m_streamManager.reset();
                        m_currentStreamer = nullptr; // Limpar referência ao streamer
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exceção ao parar streaming: " + std::string(e.what()));
                }
                
                // Garantir que o status está atualizado após parar
                if (m_ui) {
                    m_ui->setStreamingActive(false);
                    m_ui->setStreamUrl("");
                    m_ui->setStreamClientCount(0);
                    m_ui->setStreamingProcessing(false); // Processamento concluído
                }
                
                // NÃO reiniciar o portal web automaticamente quando o streaming para.
                // O portal web pode estar habilitado mas não necessariamente precisa
                // estar rodando independentemente. Se o usuário quiser o portal ativo,
                // ele pode iniciá-lo manualmente pela UI.
                // A reinicialização automática causava problemas quando o portal
                // não estava ativo antes do streaming começar.
            }).detach();
        }
        
        LOG_INFO("[CALLBACK] Thread criada, retornando (thread principal continua)"); });

    m_ui->setOnStreamingPortChanged([this](uint16_t port)
                                    {
        m_streamingPort = port;
        // Se streaming estiver ativo, reiniciar
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingWidthChanged([this](uint32_t width)
                                     { m_streamingWidth = width; });

    m_ui->setOnStreamingHeightChanged([this](uint32_t height)
                                      { m_streamingHeight = height; });

    m_ui->setOnStreamingFpsChanged([this](uint32_t fps)
                                   { m_streamingFps = fps; });

    m_ui->setOnStreamingBitrateChanged([this](uint32_t bitrate)
                                       {
        m_streamingBitrate = bitrate;
        // Atualizar bitrate do streamer se estiver ativo
        if (m_streamManager && m_streamManager->isActive()) {
            // Reiniciar streaming com novo bitrate
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingAudioBitrateChanged([this](uint32_t bitrate)
                                            {
        m_streamingAudioBitrate = bitrate;
        // Se streaming estiver ativo, reiniciar
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVideoCodecChanged([this](const std::string &codec)
                                          {
        m_streamingVideoCodec = codec;
        // Se streaming estiver ativo, reiniciar
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingAudioCodecChanged([this](const std::string &codec)
                                          {
        m_streamingAudioCodec = codec;
        // Se streaming estiver ativo, reiniciar
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH264PresetChanged([this](const std::string &preset)
                                          {
        m_streamingH264Preset = preset;
        // Se streaming estiver ativo, reiniciar para aplicar novo preset
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265PresetChanged([this](const std::string &preset)
                                          {
        m_streamingH265Preset = preset;
        // Se streaming estiver ativo, reiniciar para aplicar novo preset
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265ProfileChanged([this](const std::string &profile)
                                           {
        m_streamingH265Profile = profile;
        // Se streaming estiver ativo, reiniciar para aplicar novo profile
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingH265LevelChanged([this](const std::string &level)
                                         {
        m_streamingH265Level = level;
        // Se streaming estiver ativo, reiniciar para aplicar novo level
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVP8SpeedChanged([this](int speed)
                                        {
        m_streamingVP8Speed = speed;
        // Se streaming estiver ativo, reiniciar para aplicar novo speed
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnStreamingVP9SpeedChanged([this](int speed)
                                        {
        m_streamingVP9Speed = speed;
        // Se streaming estiver ativo, reiniciar para aplicar novo speed
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    // Callbacks para configurações de buffer
    m_ui->setOnStreamingMaxVideoBufferSizeChanged([this](size_t size)
                                                  {
        m_streamingMaxVideoBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingMaxAudioBufferSizeChanged([this](size_t size)
                                                  {
        m_streamingMaxAudioBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingMaxBufferTimeSecondsChanged([this](int64_t seconds)
                                                    {
        m_streamingMaxBufferTimeSeconds = seconds;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    m_ui->setOnStreamingAVIOBufferSizeChanged([this](size_t size)
                                              {
        m_streamingAVIOBufferSize = size;
        if (m_currentStreamer) {
            m_currentStreamer->setBufferConfig(
                m_streamingMaxVideoBufferSize,
                m_streamingMaxAudioBufferSize,
                m_streamingMaxBufferTimeSeconds,
                m_streamingAVIOBufferSize);
        } });

    // Web Portal callbacks
    m_ui->setOnWebPortalEnabledChanged([this](bool enabled)
                                       {
        m_webPortalEnabled = enabled;
        // Se Web Portal for desabilitado, também desabilitar HTTPS
        if (!enabled && m_webPortalHTTPSEnabled) {
            m_webPortalHTTPSEnabled = false;
            // Atualizar UI para refletir a mudança
            if (m_ui) {
                m_ui->setWebPortalHTTPSEnabled(false);
            }
        }
        // Atualizar em tempo real se streaming estiver ativo (sem reiniciar)
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalEnabled(enabled);
        } });

    m_ui->setOnWebPortalHTTPSChanged([this](bool enabled)
                                     {
        m_webPortalHTTPSEnabled = enabled;
        // Se streaming estiver ativo, reiniciar para aplicar HTTPS
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalSSLCertPathChanged([this](const std::string &path)
                                           {
        m_webPortalSSLCertPath = path;
        // Se streaming estiver ativo, reiniciar para aplicar novo certificado
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalSSLKeyPathChanged([this](const std::string &path)
                                          {
        m_webPortalSSLKeyPath = path;
        // Se streaming estiver ativo, reiniciar para aplicar nova chave
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->stop();
            m_streamManager->cleanup();
            m_streamManager.reset();
            initStreaming();
        } });

    m_ui->setOnWebPortalTitleChanged([this](const std::string &title)
                                     {
        m_webPortalTitle = title;
        // Atualizar em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalTitle(title);
        } });

    m_ui->setOnWebPortalSubtitleChanged([this](const std::string &subtitle)
                                        {
        m_webPortalSubtitle = subtitle;
        // Atualizar em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalSubtitle(subtitle);
        } });

    m_ui->setOnWebPortalImagePathChanged([this](const std::string &path)
                                         {
        m_webPortalImagePath = path;
        // Atualizar em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalImagePath(path);
        } });

    m_ui->setOnWebPortalBackgroundImagePathChanged([this](const std::string &path)
                                                   {
        m_webPortalBackgroundImagePath = path;
        // Atualizar em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager) {
            m_streamManager->setWebPortalBackgroundImagePath(path);
        } });

    m_ui->setOnWebPortalColorsChanged([this]()
                                      {
        // Atualizar cores em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager && m_ui) {
            // Sincronizar cores da UI para Application (com verificação de segurança)
            const float* bg = m_ui->getWebPortalColorBackground();
            if (bg) {
                memcpy(m_webPortalColorBackground, bg, 4 * sizeof(float));
            }
            const float* txt = m_ui->getWebPortalColorText();
            if (txt) {
                memcpy(m_webPortalColorText, txt, 4 * sizeof(float));
            }
            const float* prim = m_ui->getWebPortalColorPrimary();
            if (prim) {
                memcpy(m_webPortalColorPrimary, prim, 4 * sizeof(float));
            }
            const float* primLight = m_ui->getWebPortalColorPrimaryLight();
            if (primLight) {
                memcpy(m_webPortalColorPrimaryLight, primLight, 4 * sizeof(float));
            }
            const float* primDark = m_ui->getWebPortalColorPrimaryDark();
            if (primDark) {
                memcpy(m_webPortalColorPrimaryDark, primDark, 4 * sizeof(float));
            }
            const float* sec = m_ui->getWebPortalColorSecondary();
            if (sec) {
                memcpy(m_webPortalColorSecondary, sec, 4 * sizeof(float));
            }
            const float* secHighlight = m_ui->getWebPortalColorSecondaryHighlight();
            if (secHighlight) {
                memcpy(m_webPortalColorSecondaryHighlight, secHighlight, 4 * sizeof(float));
            }
            const float* ch = m_ui->getWebPortalColorCardHeader();
            if (ch) {
                memcpy(m_webPortalColorCardHeader, ch, 4 * sizeof(float));
            }
            const float* bord = m_ui->getWebPortalColorBorder();
            if (bord) {
                memcpy(m_webPortalColorBorder, bord, 4 * sizeof(float));
            }
            const float* succ = m_ui->getWebPortalColorSuccess();
            if (succ) {
                memcpy(m_webPortalColorSuccess, succ, 4 * sizeof(float));
            }
            const float* warn = m_ui->getWebPortalColorWarning();
            if (warn) {
                memcpy(m_webPortalColorWarning, warn, 4 * sizeof(float));
            }
            const float* dang = m_ui->getWebPortalColorDanger();
            if (dang) {
                memcpy(m_webPortalColorDanger, dang, 4 * sizeof(float));
            }
            const float* inf = m_ui->getWebPortalColorInfo();
            if (inf) {
                memcpy(m_webPortalColorInfo, inf, 4 * sizeof(float));
            }
            
            // Atualizar no StreamManager
            m_streamManager->setWebPortalColors(
                m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
                m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
                m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
                m_webPortalColorCardHeader, m_webPortalColorBorder,
                m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
        } });

    m_ui->setOnWebPortalTextsChanged([this]()
                                     {
        // Atualizar textos em tempo real se streaming estiver ativo
        if (m_streamingEnabled && m_streamManager && m_ui) {
            // Sincronizar textos da UI para Application
            m_webPortalTextStreamInfo = m_ui->getWebPortalTextStreamInfo();
            m_webPortalTextQuickActions = m_ui->getWebPortalTextQuickActions();
            m_webPortalTextCompatibility = m_ui->getWebPortalTextCompatibility();
            m_webPortalTextStatus = m_ui->getWebPortalTextStatus();
            m_webPortalTextCodec = m_ui->getWebPortalTextCodec();
            m_webPortalTextResolution = m_ui->getWebPortalTextResolution();
            m_webPortalTextStreamUrl = m_ui->getWebPortalTextStreamUrl();
            m_webPortalTextCopyUrl = m_ui->getWebPortalTextCopyUrl();
            m_webPortalTextOpenNewTab = m_ui->getWebPortalTextOpenNewTab();
            m_webPortalTextSupported = m_ui->getWebPortalTextSupported();
            m_webPortalTextFormat = m_ui->getWebPortalTextFormat();
            m_webPortalTextCodecInfo = m_ui->getWebPortalTextCodecInfo();
            m_webPortalTextSupportedBrowsers = m_ui->getWebPortalTextSupportedBrowsers();
            m_webPortalTextFormatInfo = m_ui->getWebPortalTextFormatInfo();
            m_webPortalTextCodecInfoValue = m_ui->getWebPortalTextCodecInfoValue();
            m_webPortalTextConnecting = m_ui->getWebPortalTextConnecting();
            
            // Atualizar no StreamManager
            m_streamManager->setWebPortalTexts(
                m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
                m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
                m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
                m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
                m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
                m_webPortalTextConnecting);
        } });

    // Web Portal Start/Stop callback (independent from streaming)
    m_ui->setOnWebPortalStartStop([this](bool start)
                                  {
        LOG_INFO("[CALLBACK] Portal Web " + std::string(start ? "START" : "STOP") + " - criando thread...");
        
        if (start) {
            // Criar thread separada para iniciar portal
            std::thread([this]() {
                try {
                    if (!initWebPortal()) {
                        LOG_ERROR("Falha ao iniciar portal web");
                        if (m_ui) {
                            m_ui->setWebPortalActive(false);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Exceção ao iniciar portal web: " + std::string(e.what()));
                    if (m_ui) {
                        m_ui->setWebPortalActive(false);
                    }
                }
            }).detach();
        } else {
            // Parar portal em thread separada
            std::thread([this]() {
                try {
                    stopWebPortal();
                } catch (const std::exception& e) {
                    LOG_ERROR("Exceção ao parar portal web: " + std::string(e.what()));
                }
                
                // Atualizar UI após parar
                if (m_ui) {
                    m_ui->setWebPortalActive(false);
                }
            }).detach();
        }
        
        LOG_INFO("[CALLBACK] Thread criada, retornando (thread principal continua)"); });

    // Callback para mudança de tipo de fonte
    m_ui->setOnSourceTypeChanged([this](UIManager::SourceType sourceType)
                                 {
                                     LOG_INFO("Tipo de fonte alterado via UI");

                                     if (sourceType == UIManager::SourceType::None)
                                     {
                                         LOG_INFO("Fonte None selecionada - ativando modo dummy");

                                         // Fechar dispositivo atual se houver
                                         if (m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             // Ativar modo dummy
                                             m_capture->setDummyMode(true);

                                             // Configurar formato dummy com resolução atual
                                             if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                             {
                                                 if (m_capture->startCapture())
                                                 {
                                                     LOG_INFO("Modo dummy ativado: " + std::to_string(m_capture->getWidth()) + "x" +
                                                              std::to_string(m_capture->getHeight()));

                                                     // Atualizar informações na UI
                                                     if (m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setV4L2Controls(nullptr); // Sem controles V4L2 quando None
                                                     }
                                                 }
                                             }
                                         }
                                     }
#ifdef __linux__
                                     if (sourceType == UIManager::SourceType::V4L2)
                                     {
                                         LOG_INFO("Fonte V4L2 selecionada");
                                         // Se já houver um dispositivo selecionado, tentar reabrir
                                         if (!m_devicePath.empty() && m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             m_capture->setDummyMode(false);

                                             if (m_capture->open(m_devicePath))
                                             {
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     m_capture->setFramerate(m_captureFps);
                                                     if (m_capture->startCapture())
                                                     {
                                                         if (m_ui)
                                                         {
                                                             m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                                  m_captureFps, m_devicePath);
                                                             m_ui->setV4L2Controls(m_capture.get());
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // Se falhar ao abrir dispositivo, voltar para modo dummy
                                                 LOG_WARN("Falha ao abrir dispositivo V4L2 - ativando modo dummy");
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setV4L2Controls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_capture)
                                         {
                                             // Se não houver dispositivo selecionado mas V4L2 foi escolhido, manter em modo dummy
                                             LOG_INFO("Nenhum dispositivo V4L2 selecionado - mantendo modo dummy");
                                         }
                                     }
#endif
#ifdef _WIN32
                                     if (sourceType == UIManager::SourceType::MF)
                                     {
                                         LOG_INFO("Fonte Media Foundation selecionada");

                                         // No Windows, m_devicePath pode ser vazio ou conter caminho Linux (/dev/video0)
                                         // Limpar se for caminho Linux
                                         std::string devicePath = m_devicePath;
                                         if (!devicePath.empty() && devicePath.find("/dev/video") == 0)
                                         {
                                             LOG_INFO("Limpando caminho de dispositivo Linux: " + devicePath);
                                             devicePath.clear();
                                             m_devicePath.clear(); // Atualizar também o membro da classe
                                         }

                                         // Se já houver um dispositivo selecionado, tentar reabrir
                                         if (!devicePath.empty() && m_capture)
                                         {
                                             m_capture->stopCapture();
                                             m_capture->close();
                                             m_capture->setDummyMode(false);

                                             if (m_capture->open(devicePath))
                                             {
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     m_capture->setFramerate(m_captureFps);
                                                     if (m_capture->startCapture())
                                                     {
                                                         if (m_ui)
                                                         {
                                                             m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                                  m_captureFps, devicePath);
                                                         }
                                                     }
                                                 }
                                             }
                                             else
                                             {
                                                 // Se falhar ao abrir dispositivo, voltar para modo dummy
                                                 LOG_WARN("Falha ao abrir dispositivo Media Foundation - ativando modo dummy");
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                     }
                                                 }
                                             }
                                         }
                                         else if (m_capture)
                                         {
                                             // Se não houver dispositivo selecionado mas MF foi escolhido, manter em modo dummy
                                             LOG_INFO("Nenhum dispositivo Media Foundation selecionado - mantendo modo dummy");
                                             if (!m_capture->isOpen() || !m_capture->isDummyMode())
                                             {
                                                 m_capture->stopCapture();
                                                 m_capture->close();
                                                 m_capture->setDummyMode(true);
                                                 if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                                                 {
                                                     if (m_capture->startCapture() && m_ui)
                                                     {
                                                         m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(),
                                                                              m_captureFps, "None (Dummy)");
                                                         m_ui->setV4L2Controls(nullptr);
                                                     }
                                                 }
                                             }
                                         }
                                     }
#endif
                                 });

    m_ui->setOnDeviceChanged([this](const std::string &devicePath)
                             {
        // Se devicePath estiver vazio, significa "None" - ativar modo dummy
        if (devicePath.empty())
        {
            LOG_INFO("Desconectando dispositivo (None selecionado) - ativando modo dummy");
            
            // Fechar dispositivo atual
            if (m_capture) {
                m_capture->stopCapture();
                m_capture->close();
                // Ativar modo dummy
                m_capture->setDummyMode(true);
                
                // Configurar formato dummy com resolução atual
                if (m_capture->setFormat(m_captureWidth, m_captureHeight, 0))
                {
                    m_capture->startCapture();
                    LOG_INFO("Modo dummy ativado: " + std::to_string(m_capture->getWidth()) + "x" +
                             std::to_string(m_capture->getHeight()));
                }
            }
            
            // Atualizar caminho do dispositivo
            m_devicePath = "";
            
            // Atualizar informações na UI
            if (m_ui) {
                if (m_capture && m_capture->isOpen()) {
                    m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                        m_captureFps, "None (Dummy)");
                } else {
                    m_ui->setCaptureInfo(0, 0, 0, "None");
                }
                m_ui->setV4L2Controls(nullptr); // Sem controles V4L2 quando não há dispositivo
            }
            
            LOG_INFO("Modo dummy ativado. Selecione um dispositivo para usar captura real.");
            return;
        }
        
        LOG_INFO("Mudando dispositivo para: " + devicePath);
        
        // Salvar configurações atuais
        uint32_t oldWidth = m_captureWidth;
        uint32_t oldHeight = m_captureHeight;
        uint32_t oldFps = m_captureFps;
        
        // Fechar dispositivo atual (ou modo dummy)
        if (m_capture) {
            m_capture->stopCapture();
            m_capture->close();
            // Desativar modo dummy ao tentar abrir dispositivo real
            m_capture->setDummyMode(false);
        }
        
        // Atualizar caminho do dispositivo
        m_devicePath = devicePath;
        
        // Limpar textura do FrameProcessor ao trocar dispositivo
        if (m_frameProcessor) {
            m_frameProcessor->deleteTexture();
        }
        
        // Reabrir com novo dispositivo
        if (m_capture && m_capture->open(devicePath)) {
            // Reconfigurar formato e framerate
            if (m_capture->setFormat(oldWidth, oldHeight, 0)) {
                m_capture->setFramerate(oldFps);
                m_capture->startCapture();
                
                // Atualizar informações na UI
                if (m_ui) {
                    m_ui->setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                        m_captureFps, devicePath);
                    
                    // Recarregar controles V4L2
                    m_ui->setV4L2Controls(m_capture.get());
                }
                
                LOG_INFO("Dispositivo alterado com sucesso");
            } else {
                LOG_ERROR("Falha ao configurar formato no novo dispositivo");
                // Fechar dispositivo se falhou
                m_capture->close();
                if (m_ui) {
                    m_ui->setCaptureInfo(0, 0, 0, "Error");
                }
            }
        } else {
            LOG_ERROR("Falha ao abrir novo dispositivo: " + devicePath);
            if (m_ui) {
                m_ui->setCaptureInfo(0, 0, 0, "Error");
            }
        } });

    // Configurar shader atual
    if (!m_presetPath.empty())
    {
        fs::path presetPath(m_presetPath);
        fs::path basePath("shaders/shaders_glsl");
        fs::path relativePath = fs::relative(presetPath, basePath);
        if (!relativePath.empty() && relativePath != presetPath)
        {
            m_ui->setCurrentShader(relativePath.string());
        }
        else
        {
            m_ui->setCurrentShader(m_presetPath);
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
    }

    return true;
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

bool Application::initStreaming()
{
    if (!m_streamingEnabled)
    {
        return true; // Streaming não habilitado, não é erro
    }

    // Se o portal web estiver ativo independentemente, pará-lo antes de iniciar streaming
    // O streaming incluirá o portal web se estiver habilitado
    if (m_webPortalActive && m_webPortalServer)
    {
        LOG_INFO("Parando Portal Web independente antes de iniciar streaming...");
        stopWebPortal();
    }

    // Log removido para reduzir ruído - streaming já loga internamente

    // OPÇÃO A: Não há mais thread de streaming para limpar

    // IMPORTANTE: Limpar streamManager existente ANTES de criar um novo
    // Isso previne problemas de double free quando há mudanças de configuração
    // CRÍTICO: Estas operações já estão em uma thread separada, mas ainda podem bloquear
    // Reduzir ao mínimo necessário e evitar esperas longas
    if (m_streamManager)
    {
        LOG_INFO("Limpando StreamManager existente antes de reinicializar...");
        // Parar e limpar de forma segura
        if (m_streamManager->isActive())
        {
            m_streamManager->stop();
        }
        m_streamManager->cleanup();
        m_streamManager.reset();
        m_currentStreamer = nullptr; // Limpar referência ao streamer

        // IMPORTANTE: Aguardar um pouco para garantir que todas as threads terminaram
        // e recursos foram liberados antes de criar um novo StreamManager
        // Reduzir tempo de espera ao mínimo - threads detached devem terminar rapidamente
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Reduzido para 10ms
    }

    m_streamManager = std::make_unique<StreamManager>();

    // IMPORTANTE: Resolução de streaming deve ser fixa, baseada nas configurações da aba de streaming
    // Se não configurada, usar resolução de captura (NUNCA usar resolução da janela que pode mudar)
    // IMPORTANTE: Verificar se dispositivo está aberto antes de acessar getWidth/getHeight
    uint32_t streamWidth = m_streamingWidth > 0 ? m_streamingWidth : (m_capture && m_capture->isOpen() ? m_capture->getWidth() : m_captureWidth);
    uint32_t streamHeight = m_streamingHeight > 0 ? m_streamingHeight : (m_capture && m_capture->isOpen() ? m_capture->getHeight() : m_captureHeight);
    uint32_t streamFps = m_streamingFps > 0 ? m_streamingFps : m_captureFps;

    LOG_INFO("initStreaming: Using resolution " + std::to_string(streamWidth) + "x" +
             std::to_string(streamHeight) + " @ " + std::to_string(streamFps) + "fps");
    LOG_INFO("initStreaming: m_streamingWidth=" + std::to_string(m_streamingWidth) +
             ", m_streamingHeight=" + std::to_string(m_streamingHeight));

    // Sempre usar MPEG-TS streamer (áudio + vídeo obrigatório)
    auto tsStreamer = std::make_unique<HTTPTSStreamer>();

    // Configurar bitrate de vídeo
    if (m_streamingBitrate > 0)
    {
        tsStreamer->setVideoBitrate(m_streamingBitrate * 1000); // Converter kbps para bps
    }

    // Configurar bitrate de áudio
    if (m_streamingAudioBitrate > 0)
    {
        tsStreamer->setAudioBitrate(m_streamingAudioBitrate * 1000); // Converter kbps para bps
    }

    // Configurar codecs
    tsStreamer->setVideoCodec(m_streamingVideoCodec);
    tsStreamer->setAudioCodec(m_streamingAudioCodec);

    // Configurar preset H.264 (se aplicável)
    if (m_streamingVideoCodec == "h264")
    {
        tsStreamer->setH264Preset(m_streamingH264Preset);
    }
    // Configurar preset, profile e level H.265 (se aplicável)
    else if (m_streamingVideoCodec == "h265" || m_streamingVideoCodec == "hevc")
    {
        tsStreamer->setH265Preset(m_streamingH265Preset);
        tsStreamer->setH265Profile(m_streamingH265Profile);
        tsStreamer->setH265Level(m_streamingH265Level);
    }
    // Configurar speed VP8 (se aplicável)
    else if (m_streamingVideoCodec == "vp8")
    {
        tsStreamer->setVP8Speed(m_streamingVP8Speed);
    }
    // Configurar speed VP9 (se aplicável)
    else if (m_streamingVideoCodec == "vp9")
    {
        tsStreamer->setVP9Speed(m_streamingVP9Speed);
    }

    // Configurar tamanho do buffer de áudio
    // setAudioBufferSize removido - buffer é gerenciado automaticamente (OBS style)

    // Configurar formato de áudio para corresponder ao AudioCapture
    if (m_audioCapture && m_audioCapture->isOpen())
    {
        tsStreamer->setAudioFormat(m_audioCapture->getSampleRate(), m_audioCapture->getChannels());
    }

    // Configurar parâmetros de buffer (carregados da configuração)
    tsStreamer->setBufferConfig(
        m_ui->getStreamingMaxVideoBufferSize(),
        m_ui->getStreamingMaxAudioBufferSize(),
        m_ui->getStreamingMaxBufferTimeSeconds(),
        m_ui->getStreamingAVIOBufferSize());

    // Configurar Web Portal
    tsStreamer->enableWebPortal(m_webPortalEnabled);
    tsStreamer->setWebPortalTitle(m_webPortalTitle);

    // Configurar API Controller
    tsStreamer->setApplicationForAPI(this);
    tsStreamer->setUIManagerForAPI(m_ui.get());
    tsStreamer->setWebPortalSubtitle(m_webPortalSubtitle);
    tsStreamer->setWebPortalImagePath(m_webPortalImagePath);
    tsStreamer->setWebPortalBackgroundImagePath(m_webPortalBackgroundImagePath);
    // IMPORTANTE: Passar cores apenas se arrays estiverem inicializados corretamente
    // Os arrays são inicializados com valores padrão no construtor, então são sempre válidos
    tsStreamer->setWebPortalColors(
        m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
        m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
        m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
        m_webPortalColorCardHeader, m_webPortalColorBorder,
        m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
    // Configurar textos editáveis
    tsStreamer->setWebPortalTexts(
        m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
        m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
        m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
        m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
        m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
        m_webPortalTextConnecting);

    // Configurar HTTPS do Web Portal
    if (m_webPortalHTTPSEnabled && !m_webPortalSSLCertPath.empty() && !m_webPortalSSLKeyPath.empty())
    {
        // Os caminhos serão resolvidos no HTTPTSStreamer::start() que busca em vários locais
        // Aqui apenas passamos os caminhos como configurados na UI
        tsStreamer->setSSLCertificatePath(m_webPortalSSLCertPath, m_webPortalSSLKeyPath);
        tsStreamer->enableHTTPS(true);
        LOG_INFO("HTTPS habilitado na configuração. Certificados serão buscados no diretório de execução.");
    }

    // Armazenar referência ao streamer antes de movê-lo para o StreamManager
    m_currentStreamer = tsStreamer.get();
    m_streamManager->addStreamer(std::move(tsStreamer));
    LOG_INFO("Usando HTTP MPEG-TS streamer (áudio + vídeo)");

    if (!m_streamManager->initialize(m_streamingPort, streamWidth, streamHeight, streamFps))
    {
        LOG_ERROR("Falha ao inicializar StreamManager");
        m_streamManager.reset();
        return false;
    }

    if (!m_streamManager->start())
    {
        LOG_ERROR("Falha ao iniciar streaming");
        m_streamManager.reset();
        return false;
    }

    LOG_INFO("Streaming iniciado na porta " + std::to_string(m_streamingPort));
    auto urls = m_streamManager->getStreamUrls();
    for (const auto &url : urls)
    {
        LOG_INFO("Stream disponível: " + url);
    }

    // Limpar caminhos encontrados (serão atualizados quando o streaming realmente iniciar)
    // Os caminhos são encontrados no HTTPTSStreamer::start(), mas não temos acesso direto aqui
    // Vamos atualizar a UI periodicamente no loop principal
    m_foundSSLCertPath.clear();
    m_foundSSLKeyPath.clear();

    // Initialize audio capture if not already initialized (sempre necessário para streaming)
    if (!m_audioCapture)
    {
        if (!initAudioCapture())
        {
            LOG_WARN("Falha ao inicializar captura de áudio - continuando sem áudio");
        }
    }

    return true;
}

bool Application::initWebPortal()
{
    if (m_webPortalActive && m_webPortalServer)
    {
        LOG_INFO("Portal Web já está ativo");
        return true;
    }

    if (!m_webPortalEnabled)
    {
        LOG_WARN("Portal Web está desabilitado na configuração");
        return false;
    }

    LOG_INFO("Iniciando Portal Web independente...");

    // Criar HTTPTSStreamer apenas para o portal (sem streaming)
    m_webPortalServer = std::make_unique<HTTPTSStreamer>();

    // Configurar Web Portal
    m_webPortalServer->enableWebPortal(true);
    m_webPortalServer->setWebPortalTitle(m_webPortalTitle);
    m_webPortalServer->setWebPortalSubtitle(m_webPortalSubtitle);
    m_webPortalServer->setWebPortalImagePath(m_webPortalImagePath);
    m_webPortalServer->setWebPortalBackgroundImagePath(m_webPortalBackgroundImagePath);
    m_webPortalServer->setWebPortalColors(
        m_webPortalColorBackground, m_webPortalColorText, m_webPortalColorPrimary,
        m_webPortalColorPrimaryLight, m_webPortalColorPrimaryDark,
        m_webPortalColorSecondary, m_webPortalColorSecondaryHighlight,
        m_webPortalColorCardHeader, m_webPortalColorBorder,
        m_webPortalColorSuccess, m_webPortalColorWarning, m_webPortalColorDanger, m_webPortalColorInfo);
    m_webPortalServer->setWebPortalTexts(
        m_webPortalTextStreamInfo, m_webPortalTextQuickActions, m_webPortalTextCompatibility,
        m_webPortalTextStatus, m_webPortalTextCodec, m_webPortalTextResolution,
        m_webPortalTextStreamUrl, m_webPortalTextCopyUrl, m_webPortalTextOpenNewTab,
        m_webPortalTextSupported, m_webPortalTextFormat, m_webPortalTextCodecInfo,
        m_webPortalTextSupportedBrowsers, m_webPortalTextFormatInfo, m_webPortalTextCodecInfoValue,
        m_webPortalTextConnecting);

    // Configurar API Controller
    m_webPortalServer->setApplicationForAPI(this);
    m_webPortalServer->setUIManagerForAPI(m_ui.get());

    // Configurar HTTPS
    if (m_webPortalHTTPSEnabled && !m_webPortalSSLCertPath.empty() && !m_webPortalSSLKeyPath.empty())
    {
        m_webPortalServer->setSSLCertificatePath(m_webPortalSSLCertPath, m_webPortalSSLKeyPath);
        m_webPortalServer->enableHTTPS(true);
        LOG_INFO("HTTPS habilitado para Portal Web. Certificados serão buscados no diretório de execução.");
    }

    // Inicializar com dimensões dummy (não usadas para portal sem streaming)
    if (!m_webPortalServer->initialize(m_streamingPort, 640, 480, 30))
    {
        LOG_ERROR("Falha ao inicializar Portal Web");
        m_webPortalServer.reset();
        return false;
    }

    // Iniciar apenas o servidor HTTP (sem encoding thread)
    if (!m_webPortalServer->startWebPortalServer())
    {
        LOG_ERROR("Falha ao iniciar servidor HTTP do Portal Web");
        m_webPortalServer.reset();
        return false;
    }

    m_webPortalActive = true;
    LOG_INFO("Portal Web iniciado na porta " + std::to_string(m_streamingPort));
    std::string portalUrl = (m_webPortalHTTPSEnabled ? "https://" : "http://") +
                            std::string("localhost:") + std::to_string(m_streamingPort);
    LOG_INFO("Portal Web disponível: " + portalUrl);

    // Atualizar UI
    if (m_ui)
    {
        m_ui->setWebPortalActive(true);
    }

    return true;
}

void Application::stopWebPortal()
{
    if (!m_webPortalActive || !m_webPortalServer)
    {
        return;
    }

    LOG_INFO("Parando Portal Web...");

    // Parar servidor HTTP
    m_webPortalServer->stop();
    m_webPortalServer.reset();
    m_webPortalActive = false;

    LOG_INFO("Portal Web parado");

    // Atualizar UI
    if (m_ui)
    {
        m_ui->setWebPortalActive(false);
    }
}

bool Application::initAudioCapture()
{
    if (!m_streamingEnabled)
    {
        return true; // Audio não habilitado, não é erro
    }

    m_audioCapture = AudioCaptureFactory::create();
    if (!m_audioCapture)
    {
        LOG_ERROR("Falha ao criar AudioCapture para esta plataforma");
        return false;
    }

    // Open default audio device (will create virtual sink)
    if (!m_audioCapture->open())
    {
        LOG_ERROR("Falha ao abrir dispositivo de áudio");
        m_audioCapture.reset();
        return false;
    }

    // Start capturing
    if (!m_audioCapture->startCapture())
    {
        LOG_ERROR("Falha ao iniciar captura de áudio");
        m_audioCapture->close();
        m_audioCapture.reset();
        return false;
    }

    LOG_INFO("Captura de áudio iniciada: " + std::to_string(m_audioCapture->getSampleRate()) +
             "Hz, " + std::to_string(m_audioCapture->getChannels()) + " canais");

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

        // Processar mudança de fullscreen pendente (fora do callback para evitar deadlock)
        if (m_pendingFullscreenChange && m_window)
        {
            m_pendingFullscreenChange = false;
            m_window->setFullscreen(m_fullscreen, m_monitorIndex);
            // O callback de resize será chamado automaticamente pelo GLFW
            // Não fazer setViewport aqui para evitar bloqueios
        }

        // IMPORTANTE: Captura, processamento e streaming sempre continuam,
        // independente do foco da janela. Isso garante que o streaming funcione
        // mesmo quando a janela não está em foco.

        // OPÇÃO A: Processar áudio continuamente na thread principal (independente de frames de vídeo)
        // Processar TODOS os samples disponíveis em loop até esgotar
        // Isso garante que o áudio seja processado continuamente mesmo se o loop principal não rodar a 60 FPS
        // IMPORTANTE: Processar mainloop do PulseAudio sempre que áudio estiver aberto
        // Isso é crítico para evitar que o PulseAudio trave o áudio do sistema
        // O mainloop precisa ser processado regularmente, mesmo sem streaming ativo
        if (m_audioCapture && m_audioCapture->isOpen())
        {
            // Processar mainloop do PulseAudio para evitar bloqueio do áudio do sistema
            // Isso deve ser feito sempre, não apenas quando streaming está ativo
            if (m_streamManager && m_streamManager->isActive())
            {

                // Calcular tamanho do buffer baseado no tempo para sincronização
                uint32_t audioSampleRate = m_audioCapture->getSampleRate();
                uint32_t videoFps = m_streamingFps > 0 ? m_streamingFps : m_captureFps;

                // Calcular samples correspondentes a 1 frame de vídeo
                // Para 60 FPS e 44100Hz: 44100/60 = 735 samples por frame
                size_t samplesPerVideoFrame = (audioSampleRate > 0 && videoFps > 0)
                                                  ? static_cast<size_t>((audioSampleRate + videoFps / 2) / videoFps)
                                                  : 512;
                samplesPerVideoFrame = std::max(static_cast<size_t>(64), std::min(samplesPerVideoFrame, static_cast<size_t>(audioSampleRate)));

                // Processar áudio em loop até esgotar todos os samples disponíveis
                // OTIMIZAÇÃO: Reutilizar buffer para evitar alocações desnecessárias
                // IMPORTANTE: Adicionar limite de iterações para evitar loop infinito que travaria a thread principal
                std::vector<int16_t> audioBuffer(samplesPerVideoFrame);

                // Limite de iterações para evitar loop infinito (processar no máximo 10 frames de áudio por ciclo)
                const int maxIterations = 10;
                int iteration = 0;

                while (iteration < maxIterations)
                {
                    // Ler áudio em chunks correspondentes ao tempo de 1 frame de vídeo
                    size_t samplesRead = m_audioCapture->getSamples(audioBuffer.data(), samplesPerVideoFrame);

                    if (samplesRead > 0)
                    {
                        m_streamManager->pushAudio(audioBuffer.data(), samplesRead);

                        // Se lemos menos que o esperado, não há mais samples disponíveis
                        if (samplesRead < samplesPerVideoFrame)
                        {
                            break; // Não há mais samples disponíveis
                        }
                    }
                    else
                    {
                        // Não há mais samples disponíveis, parar
                        break;
                    }

                    iteration++;
                }

                // Se atingimos o limite, há muito áudio acumulado - logar apenas ocasionalmente
                if (iteration >= maxIterations)
                {
                    static int logCount = 0;
                    if (logCount < 3)
                    {
                        LOG_WARN("Áudio acumulado: processando em chunks para evitar bloqueio da thread principal");
                        logCount++;
                    }
                }
            }
            else
            {
                // Streaming não está ativo, mas ainda precisamos processar o mainloop
                // para evitar que o PulseAudio trave o áudio do sistema
                // Ler e descartar samples para manter o buffer limpo
                const size_t maxSamples = 4096; // Buffer temporário
                std::vector<int16_t> tempBuffer(maxSamples);
                m_audioCapture->getSamples(tempBuffer.data(), maxSamples);
            }
        }

        // Processar entrada de teclado (F12 para toggle UI)
        handleKeyInput();

        // Iniciar frame do ImGui
        if (m_ui)
        {
            m_ui->beginFrame();
        }

        // Tentar capturar e processar o frame mais recente (descartando frames antigos)
        // IMPORTANTE: A captura sempre continua, mesmo quando a janela não está focada
        // Isso garante que o streaming e processamento continuem funcionando
        // IMPORTANTE: Só tentar capturar se o dispositivo estiver aberto
        bool newFrame = false;
        if (m_capture && m_capture->isOpen())
        {
            // Tentar processar frame várias vezes se não temos textura válida
            // Isso é importante após reconfiguração quando a textura foi deletada
            int maxAttempts = (m_frameProcessor->getTexture() == 0 && !m_frameProcessor->hasValidFrame()) ? 5 : 1;
            for (int attempt = 0; attempt < maxAttempts; ++attempt)
            {
                newFrame = m_frameProcessor->processFrame(m_capture.get());
                if (newFrame && m_frameProcessor->hasValidFrame() && m_frameProcessor->getTexture() != 0)
                {
                    break; // Frame processado com sucesso
                }
                if (attempt < maxAttempts - 1)
                {
#ifdef PLATFORM_LINUX
                    usleep(5000); // 5ms entre tentativas
#else
                    Sleep(5); // 5ms entre tentativas
#endif
                }
            }
        }

        // Sempre renderizar se temos um frame válido
        // Isso garante que estamos sempre mostrando o frame mais recente
        if (m_frameProcessor && m_frameProcessor->hasValidFrame() && m_frameProcessor->getTexture() != 0)
        {
            // Aplicar shader se estiver ativo
            GLuint textureToRender = m_frameProcessor->getTexture();
            bool isShaderTexture = false;

            if (m_shaderEngine && m_shaderEngine->isShaderActive())
            {
                // IMPORTANTE: Atualizar viewport com as dimensões da janela antes de aplicar o shader
                // Isso garante que o último pass renderize para o tamanho correto da janela
                // IMPORTANTE: Sempre usar dimensões atuais, especialmente quando entra em fullscreen
                // IMPORTANTE: Validar dimensões antes de atualizar viewport para evitar problemas durante resize
                uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
                uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

                // Validar dimensões antes de atualizar viewport
                if (currentWidth > 0 && currentHeight > 0 && currentWidth <= 7680 && currentHeight <= 4320)
                {
                    m_shaderEngine->setViewport(currentWidth, currentHeight);
                }

                textureToRender = m_shaderEngine->applyShader(m_frameProcessor->getTexture(),
                                                              m_frameProcessor->getTextureWidth(),
                                                              m_frameProcessor->getTextureHeight());
                isShaderTexture = true;

                // DEBUG: Verificar textura retornada
                if (textureToRender == 0)
                {
                    LOG_WARN("Shader retornou textura inválida (0), usando textura original");
                    textureToRender = m_frameProcessor->getTexture();
                    isShaderTexture = false;
                }
                else
                {
                    // Log removido para reduzir verbosidade
                }
            }

            // Limpar o framebuffer da janela antes de renderizar
            // IMPORTANTE: O framebuffer 0 é a janela (default framebuffer)
            // IMPORTANTE: Lock mutex para proteger durante resize
            std::lock_guard<std::mutex> resizeLock(m_resizeMutex);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // IMPORTANTE: Resetar viewport para o tamanho completo da janela
            // Isso garante que a textura seja renderizada em toda a janela
            // IMPORTANTE: Sempre atualizar viewport com as dimensões atuais da janela
            // Isso é especialmente importante quando entra em fullscreen
            uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
            uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

            // Validar dimensões antes de continuar
            if (currentWidth == 0 || currentHeight == 0 || currentWidth > 7680 || currentHeight > 4320)
            {
                // Dimensões inválidas, pular este frame
                if (m_ui)
                {
                    m_ui->endFrame();
                }
                m_window->swapBuffers();
                continue;
            }

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
                renderWidth = m_frameProcessor->getTextureWidth();
                renderHeight = m_frameProcessor->getTextureHeight();
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
                    renderWidth = m_frameProcessor->getTextureWidth();
                    renderHeight = m_frameProcessor->getTextureHeight();
                }
            }
            else
            {
                // Sem shader, usar dimensões da captura
                renderWidth = m_frameProcessor->getTextureWidth();
                renderHeight = m_frameProcessor->getTextureHeight();
            }

            // IMPORTANTE: A imagem da câmera vem invertida (Y invertido)
            // Shaders também renderizam invertido, então ambos precisam de inversão Y
            // flipY: true para ambos (câmera e shader precisam inverter)
            bool shouldFlipY = true;

            // Calcular viewport onde a captura será renderizada (pode ser menor que a janela se maintainAspect estiver ativo)
            uint32_t windowWidth = m_window->getWidth();
            uint32_t windowHeight = m_window->getHeight();
            GLint viewportX = 0;
            GLint viewportY = 0;
            GLsizei viewportWidth = windowWidth;
            GLsizei viewportHeight = windowHeight;

            if (m_maintainAspect && renderWidth > 0 && renderHeight > 0)
            {
                // Calcular aspect ratio da textura e da janela (igual ao renderTexture)
                float textureAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);
                float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);

                if (textureAspect > windowAspect)
                {
                    // Textura é mais larga: ajustar altura (letterboxing)
                    viewportHeight = static_cast<GLsizei>(windowWidth / textureAspect);
                    viewportY = (windowHeight - viewportHeight) / 2;
                }
                else
                {
                    // Textura é mais alta: ajustar largura (pillarboxing)
                    viewportWidth = static_cast<GLsizei>(windowHeight * textureAspect);
                    viewportX = (windowWidth - viewportWidth) / 2;
                }
            }

            m_renderer->renderTexture(textureToRender, m_window->getWidth(), m_window->getHeight(),
                                      shouldFlipY, isShaderTexture, m_brightness, m_contrast,
                                      m_maintainAspect, renderWidth, renderHeight);

            if (m_streamManager && m_streamManager->isActive())
            {
                uint32_t captureWidth = static_cast<uint32_t>(viewportWidth);
                uint32_t captureHeight = static_cast<uint32_t>(viewportHeight);
                size_t captureDataSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 3;

                if (captureDataSize > 0 && captureDataSize <= (7680 * 4320 * 3) &&
                    captureWidth > 0 && captureHeight > 0 && captureWidth <= 7680 && captureHeight <= 4320)
                {
                    size_t readRowSizeUnpadded = static_cast<size_t>(captureWidth) * 3;
                    size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
                    size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(captureHeight);

                    std::vector<uint8_t> frameDataWithPadding;
                    frameDataWithPadding.resize(totalSizeWithPadding);

                    glReadPixels(viewportX, viewportY, static_cast<GLsizei>(captureWidth), static_cast<GLsizei>(captureHeight),
                                 GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

                    std::vector<uint8_t> frameData;
                    frameData.resize(captureDataSize);

                    for (uint32_t row = 0; row < captureHeight; row++)
                    {
                        uint32_t srcRow = captureHeight - 1 - row; // Flip vertical
                        uint32_t dstRow = row;

                        const uint8_t *srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
                        uint8_t *dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
                        memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
                    }
                    if (m_streamManager)
                    {
                        m_streamManager->pushFrame(frameData.data(), captureWidth, captureHeight);
                    }
                }
            }

            auto streamManager = m_streamManager.get();
            if (m_ui && streamManager && streamManager->isActive())
            {
                m_ui->setStreamingActive(true);
                auto urls = streamManager->getStreamUrls();
                if (!urls.empty())
                {
                    m_ui->setStreamUrl(urls[0]);
                }
                uint32_t clientCount = streamManager->getTotalClientCount();
                m_ui->setStreamClientCount(clientCount);

                // Atualizar cooldown (se ativo, pode iniciar e não há cooldown)
                m_ui->setCanStartStreaming(true);
                m_ui->setStreamingCooldownRemainingMs(0);

                // Atualizar informações do certificado SSL se HTTPS estiver ativo
                std::string foundCert = streamManager->getFoundSSLCertificatePath();
                std::string foundKey = streamManager->getFoundSSLKeyPath();

                if (m_webPortalHTTPSEnabled && !foundCert.empty())
                {
                    m_foundSSLCertPath = foundCert;
                    m_foundSSLKeyPath = foundKey;
                    m_ui->setFoundSSLCertificatePath(foundCert);
                    m_ui->setFoundSSLKeyPath(foundKey);
                }
                else
                {
                    // Limpar caminhos encontrados se HTTPS não estiver ativo
                    m_foundSSLCertPath.clear();
                    m_foundSSLKeyPath.clear();
                    m_ui->setFoundSSLCertificatePath("");
                    m_ui->setFoundSSLKeyPath("");
                }
            }
            else if (m_ui)
            {
                m_ui->setStreamingActive(false);
                m_ui->setStreamUrl("");
                m_ui->setStreamClientCount(0);

                // Atualizar cooldown do StreamManager se disponível
                if (streamManager)
                {
                    bool canStart = streamManager->canStartStreaming();
                    int64_t cooldownMs = streamManager->getStreamingCooldownRemainingMs();
                    m_ui->setCanStartStreaming(canStart);
                    m_ui->setStreamingCooldownRemainingMs(cooldownMs);
                }
                else
                {
                    // Se não há StreamManager, pode iniciar
                    m_ui->setCanStartStreaming(true);
                    m_ui->setStreamingCooldownRemainingMs(0);
                }
            }

            // Renderizar UI (após capturar a área da captura)
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
            // Se não há frame válido ainda, ainda precisamos renderizar a UI e atualizar a janela
            // para que a janela seja visível mesmo sem frame de vídeo

            // Limpar framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            uint32_t currentWidth = m_window ? m_window->getWidth() : m_windowWidth;
            uint32_t currentHeight = m_window ? m_window->getHeight() : m_windowHeight;

            if (currentWidth > 0 && currentHeight > 0)
            {
                glViewport(0, 0, currentWidth, currentHeight);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            // IMPORTANTE: Sempre finalizar o frame do ImGui, mesmo se não renderizarmos nada
            // Isso evita o erro "Forgot to call Render() or EndFrame()"
            if (m_ui)
            {
                m_ui->render();
                m_ui->endFrame();
            }

            // IMPORTANTE: Sempre fazer swapBuffers para que a janela seja atualizada e visível
            m_window->swapBuffers();

// Fazer um pequeno sleep para não consumir 100% da CPU
#ifdef PLATFORM_LINUX
            usleep(1000); // 1ms
#else
            Sleep(1); // 1ms
#endif
        }
    }

    LOG_INFO("Loop principal encerrado");
}

void Application::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    LOG_INFO("Encerrando Application...");

    if (m_frameProcessor)
    {
        m_frameProcessor->deleteTexture();
    }

    if (m_capture)
    {
        m_capture->stopCapture();
        m_capture->close();
        m_capture.reset();
    }

    if (m_shaderEngine)
    {
        m_shaderEngine->shutdown();
        m_shaderEngine.reset();
    }

    if (m_frameProcessor)
    {
        m_frameProcessor.reset();
    }

    if (m_renderer)
    {
        m_renderer->shutdown();
        m_renderer.reset();
    }

    if (m_ui)
    {
        m_ui->shutdown();
        m_ui.reset();
    }

    if (m_window)
    {
        m_window->shutdown();
        m_window.reset();
    }

    // SwsContext de resize foi removido - agora é feito no encoding

    // OPÇÃO A: Não há mais thread de streaming para limpar

    if (m_streamManager)
    {
        m_streamManager->cleanup();
        m_streamManager.reset();
    }

    if (m_audioCapture)
    {
        m_audioCapture->stopCapture();
        m_audioCapture->close();
        m_audioCapture.reset();
    }

    m_initialized = false;
}
