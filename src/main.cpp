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
    std::cout << "\nOpções de Ajuste:\n";
    std::cout << "  --brightness <valor>   Brilho geral (0.0-5.0, padrão: 1.0)\n";
    std::cout << "\nOutras:\n";
    std::cout << "  --help, -h             Mostrar esta ajuda\n";
    std::cout << "\nExemplos:\n";
    std::cout << "  " << programName << " --device /dev/video2 --preset shaders/shaders_glsl/crt/zfast-crt.glslp\n";
    std::cout << "  " << programName << " --width 1280 --height 720 --fps 30\n";
    std::cout << "  " << programName << " --device /dev/video1 --width 3840 --height 2160 --fps 60\n";
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
    float brightness = 1.0f;

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
        } else if (arg == "--brightness" && i + 1 < argc) {
            brightness = std::stof(argv[++i]);
            if (brightness < 0.0f || brightness > 5.0f) {
                LOG_ERROR("Brilho inválido. Use um valor entre 0.0 e 5.0");
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
    LOG_INFO("Resolução: " + std::to_string(captureWidth) + "x" + std::to_string(captureHeight));
    LOG_INFO("Framerate: " + std::to_string(captureFps) + " fps");
    LOG_INFO("Brilho: " + std::to_string(brightness));

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
    app.setBrightness(brightness);
    
    if (!app.init()) {
        LOG_ERROR("Falha ao inicializar aplicação");
        return 1;
    }
    
    app.run();
    app.shutdown();
    
    return 0;
}

