#include "core/Application.h"
#include "ui/UIManager.h"
#include "utils/Logger.h"
#include <iostream>
#include <string>
#include <algorithm>

void printUsage(const char *programName)
{
    std::cout << "Uso: " << programName << " [opções]\n";
    std::cout << "\nOpções de Shader:\n";
    std::cout << "  --shader <caminho>     Carregar shader GLSL simples (.glsl)\n";
    std::cout << "  --preset <caminho>     Carregar preset com múltiplos passes (.glslp)\n";
    std::cout << "\nOpções de Captura:\n";
    std::cout << "  --source <tipo>        Tipo de fonte: none, v4l2 (padrão: v4l2)\n";
    std::cout << "  --width <valor>        Largura da captura (padrão: 1920)\n";
    std::cout << "  --height <valor>       Altura da captura (padrão: 1080)\n";
    std::cout << "  --fps <valor>          Framerate da captura (padrão: 60)\n";
    std::cout << "\nOpções de Janela:\n";
    std::cout << "  --window-width <valor>  Largura da janela (padrão: 1920)\n";
    std::cout << "  --window-height <valor> Altura da janela (padrão: 1080)\n";
    std::cout << "  --maintain-aspect       Manter proporção da captura (evita deformação)\n";
    std::cout << "  --fullscreen            Iniciar em modo tela cheia\n";
    std::cout << "  --monitor <número>      Monitor a usar (0=primário, 1=segundo, etc., padrão: primário)\n";
    std::cout << "\nOpções de Ajuste:\n";
    std::cout << "  --brightness <valor>   Brilho geral (0.0-5.0, padrão: 1.0)\n";
    std::cout << "  --contrast <valor>     Contraste geral (0.0-5.0, padrão: 1.0)\n";
    std::cout << "\nControles V4L2 (hardware) - apenas quando --source v4l2:\n";
    std::cout << "  --v4l2-device <caminho>     Dispositivo de captura V4L2 (padrão: /dev/video0)\n";
    std::cout << "  --v4l2-brightness <valor>    Brilho V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-contrast <valor>      Contraste V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-saturation <valor>    Saturação V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-hue <valor>           Matiz V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-gain <valor>          Ganho V4L2 (0 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-exposure <valor>      Exposição V4L2 (-13 a 1, padrão: não configurar)\n";
    std::cout << "  --v4l2-sharpness <valor>     Nitidez V4L2 (0 a 6, padrão: não configurar)\n";
    std::cout << "  --v4l2-gamma <valor>         Gama V4L2 (100 a 300, padrão: não configurar)\n";
    std::cout << "  --v4l2-whitebalance <valor>  Balanço de branco V4L2 (2800 a 6500, padrão: não configurar)\n";
    std::cout << "\nOpções de Streaming:\n";
    std::cout << "  --stream-enable              Habilitar streaming HTTP MPEG-TS (áudio + vídeo)\n";
    std::cout << "  --stream-port <porta>        Porta para streaming (padrão: 8080)\n";
    std::cout << "  --stream-width <largura>    Largura do stream (padrão: 640, 0 = captura)\n";
    std::cout << "  --stream-height <altura>    Altura do stream (padrão: 480, 0 = captura)\n";
    std::cout << "  --stream-fps <fps>          FPS do stream (padrão: 60, 0 = captura)\n";
    std::cout << "  --stream-bitrate <kbps>      Bitrate de vídeo em kbps (padrão: 8000)\n";
    std::cout << "  --stream-audio-bitrate <kbps> Bitrate de áudio em kbps (padrão: 256)\n";
    std::cout << "  --stream-video-codec <codec> Codec de vídeo: h264, h265, vp8, vp9 (padrão: h264)\n";
    std::cout << "  --stream-audio-codec <codec> Codec de áudio: aac, mp3, opus (padrão: aac)\n";
    std::cout << "\nOpções de Web Portal:\n";
    std::cout << "  --web-portal-enable              Habilitar web portal (padrão: habilitado)\n";
    std::cout << "  --web-portal-disable             Desabilitar web portal\n";
    std::cout << "  --web-portal-port <porta>       Porta do web portal (padrão: 8080, mesma do streaming)\n";
    std::cout << "  --web-portal-https               Habilitar HTTPS no web portal\n";
    std::cout << "  --web-portal-ssl-cert <caminho>   Caminho do certificado SSL (padrão: ssl/server.crt)\n";
    std::cout << "  --web-portal-ssl-key <caminho>    Caminho da chave SSL (padrão: ssl/server.key)\n";
    std::cout << "\nOutras:\n";
    std::cout << "  --help, -h             Mostrar esta ajuda\n";
    std::cout << "\nExemplos:\n";
    std::cout << "  " << programName << " --source v4l2 --v4l2-device /dev/video2 --preset shaders/shaders_glsl/crt/zfast-crt.glslp\n";
    std::cout << "  " << programName << " --width 1280 --height 720 --fps 30\n";
    std::cout << "  " << programName << " --source v4l2 --v4l2-device /dev/video1 --width 3840 --height 2160 --fps 60\n";
    std::cout << "  " << programName << " --window-width 1280 --window-height 720 --brightness 1.2\n";
    std::cout << "  " << programName << " --window-width 800 --window-height 600 --maintain-aspect\n";
    std::cout << "  " << programName << " --fullscreen --maintain-aspect\n";
    std::cout << "  " << programName << " --fullscreen --monitor 1\n";
    std::cout << "  " << programName << " --v4l2-brightness 20 --v4l2-contrast 10 --v4l2-saturation 5\n";
}

int main(int argc, char *argv[])
{
    Logger::init();

    LOG_INFO("RetroCapture v0.3.0");

    std::string shaderPath;
    std::string presetPath;
    // Detectar plataforma e definir sourceType padrão
    std::string sourceType;
#ifdef __linux__
    sourceType = "v4l2"; // Linux usa V4L2
#elif defined(_WIN32)
    sourceType = "mf"; // Windows usa Media Foundation
#else
    sourceType = "none"; // Outras plataformas sem suporte
#endif
#ifdef __linux__
    std::string devicePath = "/dev/video0";
#elif defined(_WIN32)
    std::string devicePath = ""; // Windows: dispositivo será selecionado via Media Foundation
#else
    std::string devicePath = "";
#endif
    int captureWidth = 1920;
    int captureHeight = 1080;
    int captureFps = 60;
    int windowWidth = 1920;
    int windowHeight = 1080;
    bool maintainAspect = false;
    bool fullscreen = false;
    int monitorIndex = -1; // -1 = não especificado (usar padrão)
    float brightness = 1.0f;
    float contrast = 1.0f;

    // Controles V4L2 (-1 significa não configurar)
    int v4l2Brightness = -1;
    int v4l2Contrast = -1;
    int v4l2Saturation = -1;
    int v4l2Hue = -1;
    int v4l2Gain = -1;
    int v4l2Exposure = -1;
    int v4l2Sharpness = -1;
    int v4l2Gamma = -1;
    int v4l2WhiteBalance = -1;

    // Streaming options
    bool streamingEnabled = false;
    int streamingPort = 8080;
    int streamWidth = 640;                 // Padrão: 640px (0 = usar resolução de captura)
    int streamHeight = 480;                // Padrão: 480px (0 = usar resolução de captura)
    int streamFps = 60;                    // Padrão: 60fps (0 = usar FPS da captura)
    int streamBitrate = 8000;              // Padrão: 8000 kbps (0 = calcular automaticamente)
    int streamAudioBitrate = 256;          // Padrão: 256 kbps
    std::string streamVideoCodec = "h264"; // Codec de vídeo
    std::string streamAudioCodec = "aac";  // Codec de áudio

    // Web Portal options
    bool webPortalEnabled = true; // Habilitado por padrão
    int webPortalPort = 8080;     // Porta do web portal (mesma do streaming por padrão)
    bool webPortalHTTPSEnabled = false;
    std::string webPortalSSLCertPath = "ssl/server.crt";
    std::string webPortalSSLKeyPath = "ssl/server.key";

    // Parsear argumentos
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--shader" && i + 1 < argc)
        {
            shaderPath = argv[++i];
        }
        else if (arg == "--preset" && i + 1 < argc)
        {
            presetPath = argv[++i];
        }
        else if (arg == "--source" && i + 1 < argc)
        {
            sourceType = argv[++i];
            // Converter para minúsculas para comparação case-insensitive
            std::transform(sourceType.begin(), sourceType.end(), sourceType.begin(), ::tolower);
#ifdef __linux__
            if (sourceType != "none" && sourceType != "v4l2")
#elif defined(_WIN32)
            if (sourceType != "none" && sourceType != "mf")
#else
            if (sourceType != "none")
#endif
            {
                LOG_ERROR("Tipo de fonte inválido. Use 'none' ou 'v4l2'");
                return 1;
            }
        }
        else if (arg == "--v4l2-device" && i + 1 < argc)
        {
            devicePath = argv[++i];
        }
        else if (arg == "--width" && i + 1 < argc)
        {
            captureWidth = std::stoi(argv[++i]);
            if (captureWidth <= 0 || captureWidth > 7680)
            {
                LOG_ERROR("Largura inválida. Use um valor entre 1 e 7680");
                return 1;
            }
        }
        else if (arg == "--height" && i + 1 < argc)
        {
            captureHeight = std::stoi(argv[++i]);
            if (captureHeight <= 0 || captureHeight > 4320)
            {
                LOG_ERROR("Altura inválida. Use um valor entre 1 e 4320");
                return 1;
            }
        }
        else if (arg == "--fps" && i + 1 < argc)
        {
            captureFps = std::stoi(argv[++i]);
            if (captureFps <= 0 || captureFps > 240)
            {
                LOG_ERROR("FPS inválido. Use um valor entre 1 e 240");
                return 1;
            }
        }
        else if (arg == "--window-width" && i + 1 < argc)
        {
            windowWidth = std::stoi(argv[++i]);
            if (windowWidth <= 0 || windowWidth > 7680)
            {
                LOG_ERROR("Largura da janela inválida. Use um valor entre 1 e 7680");
                return 1;
            }
        }
        else if (arg == "--window-height" && i + 1 < argc)
        {
            windowHeight = std::stoi(argv[++i]);
            if (windowHeight <= 0 || windowHeight > 4320)
            {
                LOG_ERROR("Altura da janela inválida. Use um valor entre 1 e 4320");
                return 1;
            }
        }
        else if (arg == "--maintain-aspect")
        {
            maintainAspect = true;
        }
        else if (arg == "--fullscreen")
        {
            fullscreen = true;
        }
        else if (arg == "--monitor" && i + 1 < argc)
        {
            monitorIndex = std::stoi(argv[++i]);
            if (monitorIndex < 0)
            {
                LOG_ERROR("Índice do monitor deve ser >= 0");
                return 1;
            }
        }
        else if (arg == "--brightness" && i + 1 < argc)
        {
            brightness = std::stof(argv[++i]);
            if (brightness < 0.0f || brightness > 5.0f)
            {
                LOG_ERROR("Brilho inválido. Use um valor entre 0.0 e 5.0");
                return 1;
            }
        }
        else if (arg == "--contrast" && i + 1 < argc)
        {
            contrast = std::stof(argv[++i]);
            if (contrast < 0.0f || contrast > 5.0f)
            {
                LOG_ERROR("Contraste inválido. Use um valor entre 0.0 e 5.0");
                return 1;
            }
        }
        else if (arg == "--v4l2-brightness" && i + 1 < argc)
        {
            v4l2Brightness = std::stoi(argv[++i]);
            if (v4l2Brightness < -100 || v4l2Brightness > 100)
            {
                LOG_ERROR("Brilho V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        }
        else if (arg == "--v4l2-contrast" && i + 1 < argc)
        {
            v4l2Contrast = std::stoi(argv[++i]);
            if (v4l2Contrast < -100 || v4l2Contrast > 100)
            {
                LOG_ERROR("Contraste V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        }
        else if (arg == "--v4l2-saturation" && i + 1 < argc)
        {
            v4l2Saturation = std::stoi(argv[++i]);
            if (v4l2Saturation < -100 || v4l2Saturation > 100)
            {
                LOG_ERROR("Saturação V4L2 inválida. Use um valor entre -100 e 100");
                return 1;
            }
        }
        else if (arg == "--v4l2-hue" && i + 1 < argc)
        {
            v4l2Hue = std::stoi(argv[++i]);
            if (v4l2Hue < -100 || v4l2Hue > 100)
            {
                LOG_ERROR("Matiz V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        }
        else if (arg == "--v4l2-gain" && i + 1 < argc)
        {
            v4l2Gain = std::stoi(argv[++i]);
            if (v4l2Gain < 0 || v4l2Gain > 100)
            {
                LOG_ERROR("Ganho V4L2 inválido. Use um valor entre 0 e 100");
                return 1;
            }
        }
        else if (arg == "--v4l2-exposure" && i + 1 < argc)
        {
            v4l2Exposure = std::stoi(argv[++i]);
            if (v4l2Exposure < -13 || v4l2Exposure > 1)
            {
                LOG_ERROR("Exposição V4L2 inválida. Use um valor entre -13 e 1");
                return 1;
            }
        }
        else if (arg == "--v4l2-sharpness" && i + 1 < argc)
        {
            v4l2Sharpness = std::stoi(argv[++i]);
            if (v4l2Sharpness < 0 || v4l2Sharpness > 6)
            {
                LOG_ERROR("Nitidez V4L2 inválida. Use um valor entre 0 e 6");
                return 1;
            }
        }
        else if (arg == "--v4l2-gamma" && i + 1 < argc)
        {
            v4l2Gamma = std::stoi(argv[++i]);
            if (v4l2Gamma < 100 || v4l2Gamma > 300)
            {
                LOG_ERROR("Gama V4L2 inválido. Use um valor entre 100 e 300");
                return 1;
            }
        }
        else if (arg == "--v4l2-whitebalance" && i + 1 < argc)
        {
            v4l2WhiteBalance = std::stoi(argv[++i]);
            if (v4l2WhiteBalance < 2800 || v4l2WhiteBalance > 6500)
            {
                LOG_ERROR("Balanço de branco V4L2 inválido. Use um valor entre 2800 e 6500");
                return 1;
            }
        }
        else if (arg == "--stream-enable")
        {
            streamingEnabled = true;
        }
        else if (arg == "--stream-port" && i + 1 < argc)
        {
            streamingPort = std::stoi(argv[++i]);
            if (streamingPort < 1024 || streamingPort > 65535)
            {
                LOG_ERROR("Porta de streaming inválida. Use um valor entre 1024 e 65535");
                return 1;
            }
        }
        else if (arg == "--stream-width" && i + 1 < argc)
        {
            streamWidth = std::stoi(argv[++i]);
            if (streamWidth < 1 || streamWidth > 7680)
            {
                LOG_ERROR("Largura do stream inválida. Use um valor entre 1 e 7680");
                return 1;
            }
        }
        else if (arg == "--stream-height" && i + 1 < argc)
        {
            streamHeight = std::stoi(argv[++i]);
            if (streamHeight < 1 || streamHeight > 4320)
            {
                LOG_ERROR("Altura do stream inválida. Use um valor entre 1 e 4320");
                return 1;
            }
        }
        else if (arg == "--stream-fps" && i + 1 < argc)
        {
            streamFps = std::stoi(argv[++i]);
            if (streamFps < 1 || streamFps > 120)
            {
                LOG_ERROR("FPS do stream inválido. Use um valor entre 1 e 120");
                return 1;
            }
        }
        else if (arg == "--stream-bitrate" && i + 1 < argc)
        {
            streamBitrate = std::stoi(argv[++i]);
            if (streamBitrate < 100 || streamBitrate > 50000)
            {
                LOG_ERROR("Bitrate do stream inválido. Use um valor entre 100 e 50000 kbps");
                return 1;
            }
        }
        else if (arg == "--stream-audio-bitrate" && i + 1 < argc)
        {
            streamAudioBitrate = std::stoi(argv[++i]);
            if (streamAudioBitrate < 32 || streamAudioBitrate > 320)
            {
                LOG_ERROR("Bitrate de áudio inválido. Use um valor entre 32 e 320 kbps");
                return 1;
            }
        }
        else if (arg == "--stream-video-codec" && i + 1 < argc)
        {
            streamVideoCodec = argv[++i];
        }
        else if (arg == "--stream-audio-codec" && i + 1 < argc)
        {
            streamAudioCodec = argv[++i];
        }
        else if (arg == "--web-portal-enable")
        {
            webPortalEnabled = true;
        }
        else if (arg == "--web-portal-disable")
        {
            webPortalEnabled = false;
        }
        else if (arg == "--web-portal-port" && i + 1 < argc)
        {
            webPortalPort = std::stoi(argv[++i]);
            if (webPortalPort < 1024 || webPortalPort > 65535)
            {
                LOG_ERROR("Porta do web portal inválida. Use um valor entre 1024 e 65535");
                return 1;
            }
        }
        else if (arg == "--web-portal-https")
        {
            webPortalHTTPSEnabled = true;
        }
        else if (arg == "--web-portal-ssl-cert" && i + 1 < argc)
        {
            webPortalSSLCertPath = argv[++i];
        }
        else if (arg == "--web-portal-ssl-key" && i + 1 < argc)
        {
            webPortalSSLKeyPath = argv[++i];
        }
        else
        {
            LOG_WARN("Argumento desconhecido: " + arg);
            printUsage(argv[0]);
            return 1;
        }
    }

// Determinar tipo de fonte
#ifdef __linux__
    bool isV4L2Source = (sourceType == "v4l2");
#elif defined(_WIN32)
    bool isMFSource = (sourceType == "mf");
#endif

    LOG_INFO("Inicializando aplicação...");
    LOG_INFO("Tipo de fonte: " + sourceType);
#ifdef __linux__
    if (isV4L2Source)
    {
        LOG_INFO("Dispositivo V4L2: " + devicePath);
    }
#elif defined(_WIN32)
    if (isMFSource && !devicePath.empty())
    {
        LOG_INFO("Dispositivo Media Foundation: " + devicePath);
    }
#endif
    LOG_INFO("Resolução de captura: " + std::to_string(captureWidth) + "x" + std::to_string(captureHeight));
    LOG_INFO("Framerate: " + std::to_string(captureFps) + " fps");
    LOG_INFO("Tamanho da janela: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight));
    LOG_INFO("Modo tela cheia: " + std::string(fullscreen ? "sim" : "não"));
    if (monitorIndex >= 0)
    {
        LOG_INFO("Monitor: " + std::to_string(monitorIndex));
    }
    else
    {
        LOG_INFO("Monitor: primário (padrão)");
    }
    LOG_INFO("Manter proporção: " + std::string(maintainAspect ? "sim" : "não"));
    LOG_INFO("Brilho: " + std::to_string(brightness));
    LOG_INFO("Contraste: " + std::to_string(contrast));
    if (streamingEnabled)
    {
        LOG_INFO("Streaming: habilitado na porta " + std::to_string(streamingPort));
    }
    LOG_INFO("Web Portal: " + std::string(webPortalEnabled ? "habilitado" : "desabilitado"));
    if (webPortalHTTPSEnabled)
    {
        LOG_INFO("HTTPS: habilitado (cert: " + webPortalSSLCertPath + ", key: " + webPortalSSLKeyPath + ")");
    }

    Application app;

    // Configurar shader/preset se especificado
    if (!presetPath.empty())
    {
        app.setPresetPath(presetPath);
        LOG_INFO("Preset especificado: " + presetPath);
    }
    else if (!shaderPath.empty())
    {
        app.setShaderPath(shaderPath);
        LOG_INFO("Shader especificado: " + shaderPath);
    }

    // Configurar parâmetros de captura
    app.setResolution(captureWidth, captureHeight);
    app.setFramerate(captureFps);
    app.setWindowSize(windowWidth, windowHeight);
    app.setFullscreen(fullscreen);
    if (monitorIndex >= 0)
    {
        app.setMonitorIndex(monitorIndex);
    }
    app.setMaintainAspect(maintainAspect);
    app.setBrightness(brightness);
    app.setContrast(contrast);

    // Configurar dispositivo e controles apenas se a fonte for apropriada
#ifdef __linux__
    if (isV4L2Source)
    {
        // Configurar dispositivo V4L2
        app.setDevicePath(devicePath);

        // Configurar controles V4L2 se especificados
        if (v4l2Brightness >= 0)
            app.setV4L2Brightness(v4l2Brightness);
        if (v4l2Contrast >= 0)
            app.setV4L2Contrast(v4l2Contrast);
        if (v4l2Saturation >= 0)
            app.setV4L2Saturation(v4l2Saturation);
        if (v4l2Hue >= 0)
            app.setV4L2Hue(v4l2Hue);
        if (v4l2Gain >= 0)
            app.setV4L2Gain(v4l2Gain);
        if (v4l2Exposure >= 0)
            app.setV4L2Exposure(v4l2Exposure);
        if (v4l2Sharpness >= 0)
            app.setV4L2Sharpness(v4l2Sharpness);
        if (v4l2Gamma >= 0)
            app.setV4L2Gamma(v4l2Gamma);
        if (v4l2WhiteBalance >= 0)
            app.setV4L2WhiteBalance(v4l2WhiteBalance);
    }
#endif
#ifdef __linux__
    if (!isV4L2Source)
    {
        // Se não for V4L2, avisar sobre parâmetros V4L2 ignorados
        bool hasV4L2Params = (v4l2Brightness >= 0 || v4l2Contrast >= 0 || v4l2Saturation >= 0 ||
                              v4l2Hue >= 0 || v4l2Gain >= 0 || v4l2Exposure >= 0 ||
                              v4l2Sharpness >= 0 || v4l2Gamma >= 0 || v4l2WhiteBalance >= 0);
        if (hasV4L2Params || devicePath != "/dev/video0")
        {
            LOG_WARN("Parâmetros V4L2 ou --v4l2-device especificados mas fonte não é V4L2. Parâmetros serão ignorados.");
        }
    }
#endif

    // Configure streaming
    app.setStreamingEnabled(streamingEnabled);
    app.setStreamingPort(streamingPort);
    // Sempre definir width/height (0 significa usar resolução de captura)
    app.setStreamingWidth(streamWidth);
    app.setStreamingHeight(streamHeight);
    // Sempre definir fps (0 significa usar FPS da captura)
    app.setStreamingFps(streamFps);
    // Sempre definir bitrate (0 significa calcular automaticamente)
    app.setStreamingBitrate(streamBitrate);
    app.setStreamingAudioBitrate(streamAudioBitrate);
    app.setStreamingVideoCodec(streamVideoCodec);
    app.setStreamingAudioCodec(streamAudioCodec);

    // Configure Web Portal
    app.setWebPortalEnabled(webPortalEnabled);
    // A porta do web portal é a mesma do streaming (ambos usam o mesmo servidor HTTP)
    // Se especificada uma porta diferente para o portal, usar essa porta para o servidor
    if (webPortalPort != streamingPort)
    {
        app.setStreamingPort(webPortalPort);
        LOG_INFO("Porta do web portal: " + std::to_string(webPortalPort));
    }
    app.setWebPortalHTTPSEnabled(webPortalHTTPSEnabled);
    app.setWebPortalSSLCertPath(webPortalSSLCertPath);
    app.setWebPortalSSLKeyPath(webPortalSSLKeyPath);

    if (!app.init())
    {
        LOG_ERROR("Falha ao inicializar aplicação");
        return 1;
    }

    // Configurar source type após inicialização (para acessar UIManager)
    UIManager::SourceType sourceTypeEnum = UIManager::SourceType::None;
#ifdef __linux__
    if (sourceType == "v4l2")
        sourceTypeEnum = UIManager::SourceType::V4L2;
#elif defined(_WIN32)
    if (sourceType == "mf")
        sourceTypeEnum = UIManager::SourceType::MF;
#endif
    app.getUIManager()->setSourceType(sourceTypeEnum);
    LOG_INFO("Tipo de fonte: " + sourceType);

    app.run();
    app.shutdown();

    return 0;
}
