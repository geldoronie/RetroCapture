#include "core/Application.h"
#include "ui/UIManager.h"
#include "utils/Logger.h"
#include <iostream>
#include <string>
#include <algorithm>

void printUsage(const char *programName)
{
    std::cout << "Usage: " << programName << " [options]\n";
    std::cout << "\nShader Options:\n";
    std::cout << "  --shader <path>        Load simple GLSL shader (.glsl)\n";
    std::cout << "  --preset <path>        Load preset with multiple passes (.glslp)\n";
    std::cout << "\nCapture Options:\n";
#ifdef __linux__
    std::cout << "  --source <type>        Source type: none, v4l2 (default: v4l2)\n";
#elif defined(_WIN32)
    std::cout << "  --source <type>        Source type: none, ds (default: ds)\n";
#else
    std::cout << "  --source <type>        Source type: none (default: none)\n";
#endif
    std::cout << "  --width <value>        Capture width (default: 1920)\n";
    std::cout << "  --height <value>       Capture height (default: 1080)\n";
    std::cout << "  --fps <value>          Capture framerate (default: 60)\n";
    std::cout << "\nWindow Options:\n";
    std::cout << "  --window-width <value>  Window width (default: 1920)\n";
    std::cout << "  --window-height <value> Window height (default: 1080)\n";
    std::cout << "  --maintain-aspect       Maintain capture aspect ratio (prevents distortion)\n";
    std::cout << "  --fullscreen            Start in fullscreen mode\n";
    std::cout << "  --monitor <number>      Monitor to use (0=primary, 1=secondary, etc., default: primary)\n";
    std::cout << "\nAdjustment Options:\n";
    std::cout << "  --brightness <value>   Overall brightness (0.0-5.0, default: 1.0)\n";
    std::cout << "  --contrast <value>     Overall contrast (0.0-5.0, default: 1.0)\n";
#ifdef __linux__
    std::cout << "\nV4L2 Hardware Controls (only when --source v4l2):\n";
    std::cout << "  --v4l2-device <path>        V4L2 capture device (default: /dev/video0)\n";
    std::cout << "  --v4l2-brightness <value>   V4L2 brightness (-100 to 100, default: don't set)\n";
    std::cout << "  --v4l2-contrast <value>     V4L2 contrast (-100 to 100, default: don't set)\n";
    std::cout << "  --v4l2-saturation <value>   V4L2 saturation (-100 to 100, default: don't set)\n";
    std::cout << "  --v4l2-hue <value>          V4L2 hue (-100 to 100, default: don't set)\n";
    std::cout << "  --v4l2-gain <value>         V4L2 gain (0 to 100, default: don't set)\n";
    std::cout << "  --v4l2-exposure <value>     V4L2 exposure (-13 to 1, default: don't set)\n";
    std::cout << "  --v4l2-sharpness <value>    V4L2 sharpness (0 to 6, default: don't set)\n";
    std::cout << "  --v4l2-gamma <value>        V4L2 gamma (100 to 300, default: don't set)\n";
    std::cout << "  --v4l2-whitebalance <value> V4L2 white balance (2800 to 6500, default: don't set)\n";
#elif defined(_WIN32)
    std::cout << "\nDirectShow Hardware Controls (only when --source ds):\n";
    std::cout << "  --ds-device <index>         DirectShow device index (default: first available)\n";
    std::cout << "  --ds-brightness <value>    DirectShow brightness (-100 to 100, default: don't set)\n";
    std::cout << "  --ds-contrast <value>       DirectShow contrast (-100 to 100, default: don't set)\n";
    std::cout << "  --ds-saturation <value>     DirectShow saturation (-100 to 100, default: don't set)\n";
    std::cout << "  --ds-hue <value>            DirectShow hue (-100 to 100, default: don't set)\n";
    std::cout << "  --ds-gain <value>           DirectShow gain (0 to 100, default: don't set)\n";
    std::cout << "  --ds-exposure <value>       DirectShow exposure (-13 to 1, default: don't set)\n";
    std::cout << "  --ds-sharpness <value>      DirectShow sharpness (0 to 6, default: don't set)\n";
    std::cout << "  --ds-gamma <value>         DirectShow gamma (100 to 300, default: don't set)\n";
    std::cout << "  --ds-whitebalance <value>   DirectShow white balance (2800 to 6500, default: don't set)\n";
#endif
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

    LOG_INFO("RetroCapture v0.4.0");

    std::string shaderPath;
    std::string presetPath;
    // Detectar plataforma e definir sourceType padrão
    std::string sourceType;
#ifdef __linux__
    sourceType = "v4l2"; // Linux usa V4L2
#elif defined(_WIN32)
    sourceType = "ds"; // Windows usa DirectShow
#else
    sourceType = "none"; // Outras plataformas sem suporte
#endif
#ifdef __linux__
    std::string devicePath = "/dev/video0";
#elif defined(_WIN32)
    std::string devicePath = ""; // Windows: dispositivo será selecionado via DirectShow
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

    // Controles DirectShow (-1 significa não configurar)
    int dsBrightness = -1;
    int dsContrast = -1;
    int dsSaturation = -1;
    int dsHue = -1;
    int dsGain = -1;
    int dsExposure = -1;
    int dsSharpness = -1;
    int dsGamma = -1;
    int dsWhiteBalance = -1;

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
            if (sourceType != "none" && sourceType != "ds")
#else
            if (sourceType != "none")
#endif
            {
#ifdef __linux__
                LOG_ERROR("Invalid source type. Use 'none' or 'v4l2'");
#elif defined(_WIN32)
                LOG_ERROR("Invalid source type. Use 'none' or 'ds'");
#else
                LOG_ERROR("Invalid source type. Use 'none'");
#endif
                return 1;
            }
        }
        else if (arg == "--v4l2-device" && i + 1 < argc)
        {
#ifdef __linux__
            devicePath = argv[++i];
#else
            LOG_WARN("--v4l2-device is only available on Linux");
            ++i; // Skip argument
#endif
        }
#ifdef _WIN32
        else if (arg == "--ds-device" && i + 1 < argc)
        {
            devicePath = argv[++i];
        }
#endif
        else if (arg == "--width" && i + 1 < argc)
        {
            captureWidth = std::stoi(argv[++i]);
            if (captureWidth <= 0 || captureWidth > 7680)
            {
                LOG_ERROR("Invalid width. Use a value between 1 and 7680");
                return 1;
            }
        }
        else if (arg == "--height" && i + 1 < argc)
        {
            captureHeight = std::stoi(argv[++i]);
            if (captureHeight <= 0 || captureHeight > 4320)
            {
                LOG_ERROR("Invalid height. Use a value between 1 and 4320");
                return 1;
            }
        }
        else if (arg == "--fps" && i + 1 < argc)
        {
            captureFps = std::stoi(argv[++i]);
            if (captureFps <= 0 || captureFps > 240)
            {
                LOG_ERROR("Invalid FPS. Use a value between 1 and 240");
                return 1;
            }
        }
        else if (arg == "--window-width" && i + 1 < argc)
        {
            windowWidth = std::stoi(argv[++i]);
            if (windowWidth <= 0 || windowWidth > 7680)
            {
                LOG_ERROR("Invalid window width. Use a value between 1 and 7680");
                return 1;
            }
        }
        else if (arg == "--window-height" && i + 1 < argc)
        {
            windowHeight = std::stoi(argv[++i]);
            if (windowHeight <= 0 || windowHeight > 4320)
            {
                LOG_ERROR("Invalid window height. Use a value between 1 and 4320");
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
                LOG_ERROR("Monitor index must be >= 0");
                return 1;
            }
        }
        else if (arg == "--brightness" && i + 1 < argc)
        {
            brightness = std::stof(argv[++i]);
            if (brightness < 0.0f || brightness > 5.0f)
            {
                LOG_ERROR("Invalid brightness. Use a value between 0.0 and 5.0");
                return 1;
            }
        }
        else if (arg == "--contrast" && i + 1 < argc)
        {
            contrast = std::stof(argv[++i]);
            if (contrast < 0.0f || contrast > 5.0f)
            {
                LOG_ERROR("Invalid contrast. Use a value between 0.0 and 5.0");
                return 1;
            }
        }
        else if (arg == "--v4l2-brightness" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Brightness = std::stoi(argv[++i]);
            if (v4l2Brightness < -100 || v4l2Brightness > 100)
            {
                LOG_ERROR("V4L2 brightness invalid. Use a value between -100 and 100");
                return 1;
            }
#else
            LOG_WARN("--v4l2-brightness is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-contrast" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Contrast = std::stoi(argv[++i]);
            if (v4l2Contrast < -100 || v4l2Contrast > 100)
            {
                LOG_ERROR("V4L2 contrast invalid. Use a value between -100 and 100");
                return 1;
            }
#else
            LOG_WARN("--v4l2-contrast is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-saturation" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Saturation = std::stoi(argv[++i]);
            if (v4l2Saturation < -100 || v4l2Saturation > 100)
            {
                LOG_ERROR("V4L2 saturation invalid. Use a value between -100 and 100");
                return 1;
            }
#else
            LOG_WARN("--v4l2-saturation is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-hue" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Hue = std::stoi(argv[++i]);
            if (v4l2Hue < -100 || v4l2Hue > 100)
            {
                LOG_ERROR("V4L2 hue invalid. Use a value between -100 and 100");
                return 1;
            }
#else
            LOG_WARN("--v4l2-hue is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-gain" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Gain = std::stoi(argv[++i]);
            if (v4l2Gain < 0 || v4l2Gain > 100)
            {
                LOG_ERROR("V4L2 gain invalid. Use a value between 0 and 100");
                return 1;
            }
#else
            LOG_WARN("--v4l2-gain is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-exposure" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Exposure = std::stoi(argv[++i]);
            if (v4l2Exposure < -13 || v4l2Exposure > 1)
            {
                LOG_ERROR("V4L2 exposure invalid. Use a value between -13 and 1");
                return 1;
            }
#else
            LOG_WARN("--v4l2-exposure is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-sharpness" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Sharpness = std::stoi(argv[++i]);
            if (v4l2Sharpness < 0 || v4l2Sharpness > 6)
            {
                LOG_ERROR("V4L2 sharpness invalid. Use a value between 0 and 6");
                return 1;
            }
#else
            LOG_WARN("--v4l2-sharpness is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-gamma" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2Gamma = std::stoi(argv[++i]);
            if (v4l2Gamma < 100 || v4l2Gamma > 300)
            {
                LOG_ERROR("V4L2 gamma invalid. Use a value between 100 and 300");
                return 1;
            }
#else
            LOG_WARN("--v4l2-gamma is only available on Linux");
            ++i; // Skip argument
#endif
        }
        else if (arg == "--v4l2-whitebalance" && i + 1 < argc)
        {
#ifdef __linux__
            v4l2WhiteBalance = std::stoi(argv[++i]);
            if (v4l2WhiteBalance < 2800 || v4l2WhiteBalance > 6500)
            {
                LOG_ERROR("V4L2 white balance invalid. Use a value between 2800 and 6500");
                return 1;
            }
#else
            LOG_WARN("--v4l2-whitebalance is only available on Linux");
            ++i; // Skip argument
#endif
        }
#ifdef _WIN32
        else if (arg == "--ds-brightness" && i + 1 < argc)
        {
            dsBrightness = std::stoi(argv[++i]);
            if (dsBrightness < -100 || dsBrightness > 100)
            {
                LOG_ERROR("DirectShow brightness invalid. Use a value between -100 and 100");
                return 1;
            }
        }
        else if (arg == "--ds-contrast" && i + 1 < argc)
        {
            dsContrast = std::stoi(argv[++i]);
            if (dsContrast < -100 || dsContrast > 100)
            {
                LOG_ERROR("DirectShow contrast invalid. Use a value between -100 and 100");
                return 1;
            }
        }
        else if (arg == "--ds-saturation" && i + 1 < argc)
        {
            dsSaturation = std::stoi(argv[++i]);
            if (dsSaturation < -100 || dsSaturation > 100)
            {
                LOG_ERROR("DirectShow saturation invalid. Use a value between -100 and 100");
                return 1;
            }
        }
        else if (arg == "--ds-hue" && i + 1 < argc)
        {
            dsHue = std::stoi(argv[++i]);
            if (dsHue < -100 || dsHue > 100)
            {
                LOG_ERROR("DirectShow hue invalid. Use a value between -100 and 100");
                return 1;
            }
        }
        else if (arg == "--ds-gain" && i + 1 < argc)
        {
            dsGain = std::stoi(argv[++i]);
            if (dsGain < 0 || dsGain > 100)
            {
                LOG_ERROR("DirectShow gain invalid. Use a value between 0 and 100");
                return 1;
            }
        }
        else if (arg == "--ds-exposure" && i + 1 < argc)
        {
            dsExposure = std::stoi(argv[++i]);
            if (dsExposure < -13 || dsExposure > 1)
            {
                LOG_ERROR("DirectShow exposure invalid. Use a value between -13 and 1");
                return 1;
            }
        }
        else if (arg == "--ds-sharpness" && i + 1 < argc)
        {
            dsSharpness = std::stoi(argv[++i]);
            if (dsSharpness < 0 || dsSharpness > 6)
            {
                LOG_ERROR("DirectShow sharpness invalid. Use a value between 0 and 6");
                return 1;
            }
        }
        else if (arg == "--ds-gamma" && i + 1 < argc)
        {
            dsGamma = std::stoi(argv[++i]);
            if (dsGamma < 100 || dsGamma > 300)
            {
                LOG_ERROR("DirectShow gamma invalid. Use a value between 100 and 300");
                return 1;
            }
        }
        else if (arg == "--ds-whitebalance" && i + 1 < argc)
        {
            dsWhiteBalance = std::stoi(argv[++i]);
            if (dsWhiteBalance < 2800 || dsWhiteBalance > 6500)
            {
                LOG_ERROR("DirectShow white balance invalid. Use a value between 2800 and 6500");
                return 1;
            }
        }
#endif
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
                LOG_ERROR("Invalid web portal port. Use a value between 1024 and 65535");
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
    bool isDSSource = (sourceType == "ds");
#endif

    LOG_INFO("Initializing application...");
    LOG_INFO("Source type: " + sourceType);
#ifdef __linux__
    if (isV4L2Source)
    {
        LOG_INFO("V4L2 device: " + devicePath);
    }
#elif defined(_WIN32)
    if (isDSSource && !devicePath.empty())
    {
        LOG_INFO("DirectShow device: " + devicePath);
    }
#endif
    LOG_INFO("Capture resolution: " + std::to_string(captureWidth) + "x" + std::to_string(captureHeight));
    LOG_INFO("Framerate: " + std::to_string(captureFps) + " fps");
    LOG_INFO("Window size: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight));
    LOG_INFO("Fullscreen mode: " + std::string(fullscreen ? "yes" : "no"));
    if (monitorIndex >= 0)
    {
        LOG_INFO("Monitor: " + std::to_string(monitorIndex));
    }
    else
    {
        LOG_INFO("Monitor: primary (default)");
    }
    LOG_INFO("Maintain aspect ratio: " + std::string(maintainAspect ? "yes" : "no"));
    LOG_INFO("Brightness: " + std::to_string(brightness));
    LOG_INFO("Contrast: " + std::to_string(contrast));
    if (streamingEnabled)
    {
        LOG_INFO("Streaming: enabled on port " + std::to_string(streamingPort));
    }
    LOG_INFO("Web Portal: " + std::string(webPortalEnabled ? "enabled" : "disabled"));
    if (webPortalHTTPSEnabled)
    {
        LOG_INFO("HTTPS: enabled (cert: " + webPortalSSLCertPath + ", key: " + webPortalSSLKeyPath + ")");
    }

    Application app;

    // Configure shader/preset if specified
    if (!presetPath.empty())
    {
        app.setPresetPath(presetPath);
        LOG_INFO("Preset specified: " + presetPath);
    }
    else if (!shaderPath.empty())
    {
        app.setShaderPath(shaderPath);
        LOG_INFO("Shader specified: " + shaderPath);
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
    else
    {
        // Se não for V4L2, avisar sobre parâmetros V4L2 ignorados
        bool hasV4L2Params = (v4l2Brightness >= 0 || v4l2Contrast >= 0 || v4l2Saturation >= 0 ||
                              v4l2Hue >= 0 || v4l2Gain >= 0 || v4l2Exposure >= 0 ||
                              v4l2Sharpness >= 0 || v4l2Gamma >= 0 || v4l2WhiteBalance >= 0);
        if (hasV4L2Params || devicePath != "/dev/video0")
        {
            LOG_WARN("V4L2 parameters or --v4l2-device specified but source is not V4L2. Parameters will be ignored.");
        }
    }
#elif defined(_WIN32)
    if (isDSSource)
    {
        // Configurar dispositivo DirectShow (índice ou string vazia)
        if (!devicePath.empty())
        {
            app.setDevicePath(devicePath);
        }

        // Configurar controles DirectShow se especificados (usando interface genérica)
        // Os controles DirectShow usam os mesmos nomes que V4L2, então podemos usar setV4L2* que internamente usa setControl()
        if (dsBrightness >= 0)
            app.setV4L2Brightness(dsBrightness);
        if (dsContrast >= 0)
            app.setV4L2Contrast(dsContrast);
        if (dsSaturation >= 0)
            app.setV4L2Saturation(dsSaturation);
        if (dsHue >= 0)
            app.setV4L2Hue(dsHue);
        if (dsGain >= 0)
            app.setV4L2Gain(dsGain);
        if (dsExposure >= 0)
            app.setV4L2Exposure(dsExposure);
        if (dsSharpness >= 0)
            app.setV4L2Sharpness(dsSharpness);
        if (dsGamma >= 0)
            app.setV4L2Gamma(dsGamma);
        if (dsWhiteBalance >= 0)
            app.setV4L2WhiteBalance(dsWhiteBalance);
    }
    else
    {
        // Se não for DirectShow, avisar sobre parâmetros DirectShow ignorados
        bool hasDSParams = (dsBrightness >= 0 || dsContrast >= 0 || dsSaturation >= 0 ||
                            dsHue >= 0 || dsGain >= 0 || dsExposure >= 0 ||
                            dsSharpness >= 0 || dsGamma >= 0 || dsWhiteBalance >= 0);
        if (hasDSParams || !devicePath.empty())
        {
            LOG_WARN("DirectShow parameters or --ds-device specified but source is not DirectShow. Parameters will be ignored.");
        }
    }
#endif

    // Configure streaming
    app.setStreamingEnabled(streamingEnabled);
    app.setStreamingPort(streamingPort);
    // Always set width/height (0 means use capture resolution)
    app.setStreamingWidth(streamWidth);
    app.setStreamingHeight(streamHeight);
    // Always set fps (0 means use capture FPS)
    app.setStreamingFps(streamFps);
    // Always set bitrate (0 means calculate automatically)
    app.setStreamingBitrate(streamBitrate);
    app.setStreamingAudioBitrate(streamAudioBitrate);
    app.setStreamingVideoCodec(streamVideoCodec);
    app.setStreamingAudioCodec(streamAudioCodec);

    // Configure Web Portal
    app.setWebPortalEnabled(webPortalEnabled);
    // Web portal port is the same as streaming (both use the same HTTP server)
    // If a different port is specified for the portal, use that port for the server
    if (webPortalPort != streamingPort)
    {
        app.setStreamingPort(webPortalPort);
        LOG_INFO("Web portal port: " + std::to_string(webPortalPort));
    }
    app.setWebPortalHTTPSEnabled(webPortalHTTPSEnabled);
    app.setWebPortalSSLCertPath(webPortalSSLCertPath);
    app.setWebPortalSSLKeyPath(webPortalSSLKeyPath);

    if (!app.init())
    {
        LOG_ERROR("Failed to initialize application");
        return 1;
    }

    // Configure source type after initialization (to access UIManager)
    UIManager::SourceType sourceTypeEnum = UIManager::SourceType::None;
#ifdef __linux__
    if (sourceType == "v4l2")
        sourceTypeEnum = UIManager::SourceType::V4L2;
#elif defined(_WIN32)
    if (sourceType == "ds")
        sourceTypeEnum = UIManager::SourceType::DS;
#endif
    app.getUIManager()->setSourceType(sourceTypeEnum);
    LOG_INFO("Source type: " + sourceType);

    app.run();
    app.shutdown();

    return 0;
}
