#include "core/Application.h"
#include "utils/Logger.h"
#include <iostream>
#include <string>

void printUsage(const char* programName) {
    std::cout << "Uso: " << programName << " [opções]\n";
    std::cout << "\nOpções de Shader:\n";
    std::cout << "  --shader <caminho>     Carregar shader GLSL simples (.glsl)\n";
    std::cout << "  --preset <caminho>     Carregar preset com múltiplos passes (.glslp)\n";
    std::cout << "\nOpções de Captura:\n";
    std::cout << "  --device <caminho>     Dispositivo de captura (padrão: /dev/video0)\n";
    std::cout << "  --width <valor>        Largura da captura (padrão: 1920)\n";
    std::cout << "  --height <valor>       Altura da captura (padrão: 1080)\n";
    std::cout << "  --fps <valor>          Framerate da captura (padrão: 60)\n";
    std::cout << "\nOpções de Janela:\n";
    std::cout << "  --window-width <valor>  Largura da janela (padrão: 1920)\n";
    std::cout << "  --window-height <valor> Altura da janela (padrão: 1080)\n";
    std::cout << "  --maintain-aspect       Manter proporção da captura (evita deformação)\n";
    std::cout << "\nOpções de Ajuste:\n";
    std::cout << "  --brightness <valor>   Brilho geral (0.0-5.0, padrão: 1.0)\n";
    std::cout << "  --contrast <valor>     Contraste geral (0.0-5.0, padrão: 1.0)\n";
    std::cout << "\nControles V4L2 (hardware):\n";
    std::cout << "  --v4l2-brightness <valor>    Brilho V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-contrast <valor>      Contraste V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-saturation <valor>    Saturação V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-hue <valor>           Matiz V4L2 (-100 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-gain <valor>          Ganho V4L2 (0 a 100, padrão: não configurar)\n";
    std::cout << "  --v4l2-exposure <valor>      Exposição V4L2 (-13 a 1, padrão: não configurar)\n";
    std::cout << "  --v4l2-sharpness <valor>     Nitidez V4L2 (0 a 6, padrão: não configurar)\n";
    std::cout << "  --v4l2-gamma <valor>         Gama V4L2 (100 a 300, padrão: não configurar)\n";
    std::cout << "  --v4l2-whitebalance <valor>  Balanço de branco V4L2 (2800 a 6500, padrão: não configurar)\n";
    std::cout << "\nOutras:\n";
    std::cout << "  --help, -h             Mostrar esta ajuda\n";
    std::cout << "\nExemplos:\n";
    std::cout << "  " << programName << " --device /dev/video2 --preset shaders/shaders_glsl/crt/zfast-crt.glslp\n";
    std::cout << "  " << programName << " --width 1280 --height 720 --fps 30\n";
    std::cout << "  " << programName << " --device /dev/video1 --width 3840 --height 2160 --fps 60\n";
    std::cout << "  " << programName << " --window-width 1280 --window-height 720 --brightness 1.2\n";
    std::cout << "  " << programName << " --window-width 800 --window-height 600 --maintain-aspect\n";
    std::cout << "  " << programName << " --v4l2-brightness 20 --v4l2-contrast 10 --v4l2-saturation 5\n";
}

int main(int argc, char* argv[]) {
    Logger::init();
    
    LOG_INFO("RetroCapture v0.3.0");
    
    std::string shaderPath;
    std::string presetPath;
    std::string devicePath = "/dev/video0";
    int captureWidth = 1920;
    int captureHeight = 1080;
    int captureFps = 60;
    int windowWidth = 1920;
    int windowHeight = 1080;
    bool maintainAspect = false;
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

    // Parsear argumentos
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--shader" && i + 1 < argc) {
            shaderPath = argv[++i];
        } else if (arg == "--preset" && i + 1 < argc) {
            presetPath = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            devicePath = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            captureWidth = std::stoi(argv[++i]);
            if (captureWidth <= 0 || captureWidth > 7680) {
                LOG_ERROR("Largura inválida. Use um valor entre 1 e 7680");
                return 1;
            }
        } else if (arg == "--height" && i + 1 < argc) {
            captureHeight = std::stoi(argv[++i]);
            if (captureHeight <= 0 || captureHeight > 4320) {
                LOG_ERROR("Altura inválida. Use um valor entre 1 e 4320");
                return 1;
            }
        } else if (arg == "--fps" && i + 1 < argc) {
            captureFps = std::stoi(argv[++i]);
            if (captureFps <= 0 || captureFps > 240) {
                LOG_ERROR("FPS inválido. Use um valor entre 1 e 240");
                return 1;
            }
        } else if (arg == "--window-width" && i + 1 < argc) {
            windowWidth = std::stoi(argv[++i]);
            if (windowWidth <= 0 || windowWidth > 7680) {
                LOG_ERROR("Largura da janela inválida. Use um valor entre 1 e 7680");
                return 1;
            }
        } else if (arg == "--window-height" && i + 1 < argc) {
            windowHeight = std::stoi(argv[++i]);
            if (windowHeight <= 0 || windowHeight > 4320) {
                LOG_ERROR("Altura da janela inválida. Use um valor entre 1 e 4320");
                return 1;
            }
        } else if (arg == "--maintain-aspect") {
            maintainAspect = true;
        } else if (arg == "--brightness" && i + 1 < argc) {
            brightness = std::stof(argv[++i]);
            if (brightness < 0.0f || brightness > 5.0f) {
                LOG_ERROR("Brilho inválido. Use um valor entre 0.0 e 5.0");
                return 1;
            }
        } else if (arg == "--contrast" && i + 1 < argc) {
            contrast = std::stof(argv[++i]);
            if (contrast < 0.0f || contrast > 5.0f) {
                LOG_ERROR("Contraste inválido. Use um valor entre 0.0 e 5.0");
                return 1;
            }
        } else if (arg == "--v4l2-brightness" && i + 1 < argc) {
            v4l2Brightness = std::stoi(argv[++i]);
            if (v4l2Brightness < -100 || v4l2Brightness > 100) {
                LOG_ERROR("Brilho V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        } else if (arg == "--v4l2-contrast" && i + 1 < argc) {
            v4l2Contrast = std::stoi(argv[++i]);
            if (v4l2Contrast < -100 || v4l2Contrast > 100) {
                LOG_ERROR("Contraste V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        } else if (arg == "--v4l2-saturation" && i + 1 < argc) {
            v4l2Saturation = std::stoi(argv[++i]);
            if (v4l2Saturation < -100 || v4l2Saturation > 100) {
                LOG_ERROR("Saturação V4L2 inválida. Use um valor entre -100 e 100");
                return 1;
            }
        } else if (arg == "--v4l2-hue" && i + 1 < argc) {
            v4l2Hue = std::stoi(argv[++i]);
            if (v4l2Hue < -100 || v4l2Hue > 100) {
                LOG_ERROR("Matiz V4L2 inválido. Use um valor entre -100 e 100");
                return 1;
            }
        } else if (arg == "--v4l2-gain" && i + 1 < argc) {
            v4l2Gain = std::stoi(argv[++i]);
            if (v4l2Gain < 0 || v4l2Gain > 100) {
                LOG_ERROR("Ganho V4L2 inválido. Use um valor entre 0 e 100");
                return 1;
            }
        } else if (arg == "--v4l2-exposure" && i + 1 < argc) {
            v4l2Exposure = std::stoi(argv[++i]);
            if (v4l2Exposure < -13 || v4l2Exposure > 1) {
                LOG_ERROR("Exposição V4L2 inválida. Use um valor entre -13 e 1");
                return 1;
            }
        } else if (arg == "--v4l2-sharpness" && i + 1 < argc) {
            v4l2Sharpness = std::stoi(argv[++i]);
            if (v4l2Sharpness < 0 || v4l2Sharpness > 6) {
                LOG_ERROR("Nitidez V4L2 inválida. Use um valor entre 0 e 6");
                return 1;
            }
        } else if (arg == "--v4l2-gamma" && i + 1 < argc) {
            v4l2Gamma = std::stoi(argv[++i]);
            if (v4l2Gamma < 100 || v4l2Gamma > 300) {
                LOG_ERROR("Gama V4L2 inválido. Use um valor entre 100 e 300");
                return 1;
            }
        } else if (arg == "--v4l2-whitebalance" && i + 1 < argc) {
            v4l2WhiteBalance = std::stoi(argv[++i]);
            if (v4l2WhiteBalance < 2800 || v4l2WhiteBalance > 6500) {
                LOG_ERROR("Balanço de branco V4L2 inválido. Use um valor entre 2800 e 6500");
                return 1;
            }
        } else {
            LOG_WARN("Argumento desconhecido: " + arg);
            printUsage(argv[0]);
            return 1;
        }
    }
    
    LOG_INFO("Inicializando aplicação...");
    LOG_INFO("Dispositivo: " + devicePath);
    LOG_INFO("Resolução de captura: " + std::to_string(captureWidth) + "x" + std::to_string(captureHeight));
    LOG_INFO("Framerate: " + std::to_string(captureFps) + " fps");
    LOG_INFO("Tamanho da janela: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight));
    LOG_INFO("Manter proporção: " + std::string(maintainAspect ? "sim" : "não"));
    LOG_INFO("Brilho: " + std::to_string(brightness));
    LOG_INFO("Contraste: " + std::to_string(contrast));

    Application app;

    // Configurar shader/preset se especificado
    if (!presetPath.empty()) {
        app.setPresetPath(presetPath);
        LOG_INFO("Preset especificado: " + presetPath);
    } else if (!shaderPath.empty()) {
        app.setShaderPath(shaderPath);
        LOG_INFO("Shader especificado: " + shaderPath);
    }

    // Configurar dispositivo e parâmetros de captura
    app.setDevicePath(devicePath);
    app.setResolution(captureWidth, captureHeight);
    app.setFramerate(captureFps);
    app.setWindowSize(windowWidth, windowHeight);
    app.setMaintainAspect(maintainAspect);
    app.setBrightness(brightness);
    app.setContrast(contrast);
    
    // Configurar controles V4L2 se especificados
    if (v4l2Brightness >= 0) app.setV4L2Brightness(v4l2Brightness);
    if (v4l2Contrast >= 0) app.setV4L2Contrast(v4l2Contrast);
    if (v4l2Saturation >= 0) app.setV4L2Saturation(v4l2Saturation);
    if (v4l2Hue >= 0) app.setV4L2Hue(v4l2Hue);
    if (v4l2Gain >= 0) app.setV4L2Gain(v4l2Gain);
    if (v4l2Exposure >= 0) app.setV4L2Exposure(v4l2Exposure);
    if (v4l2Sharpness >= 0) app.setV4L2Sharpness(v4l2Sharpness);
    if (v4l2Gamma >= 0) app.setV4L2Gamma(v4l2Gamma);
    if (v4l2WhiteBalance >= 0) app.setV4L2WhiteBalance(v4l2WhiteBalance);
    
    if (!app.init()) {
        LOG_ERROR("Falha ao inicializar aplicação");
        return 1;
    }
    
    app.run();
    app.shutdown();
    
    return 0;
}

