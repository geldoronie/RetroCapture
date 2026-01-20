#include "UIManager.h"
#include "UIConfiguration.h"
#include "UICredits.h"
#include "UICapturePresets.h"
#include "UIRecordings.h"
#include "../utils/Logger.h"
#include "../utils/ShaderScanner.h"
#ifdef PLATFORM_LINUX
#include "../utils/V4L2DeviceScanner.h"
#endif
#include "../capture/IVideoCapture.h"
#include "../shader/ShaderEngine.h"
#ifdef __APPLE__
// Forward declaration - VideoCaptureAVFoundation is Objective-C++ and cannot be included in C++ file
// Use virtual methods from IVideoCapture interface instead
class VideoCaptureAVFoundation;
#endif
#include "../renderer/glad_loader.h"
#include <string>
#ifdef USE_SDL2
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#endif
#ifdef PLATFORM_LINUX
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "../utils/FilesystemCompat.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

UIManager::UIManager()
    : m_configWindow(nullptr), m_creditsWindow(nullptr)
{
}

UIManager::~UIManager()
{
    // Destruir janelas antes de shutdown
    m_configWindow.reset();
    m_creditsWindow.reset();
    m_capturePresetsWindow.reset();
    m_recordingsWindow.reset();
    shutdown();
}

bool UIManager::init(void *window)
{
    if (m_initialized)
    {
        return true;
    }

    if (!window)
    {
        LOG_ERROR("Invalid window pointer for ImGui initialization");
        return false;
    }

    m_window = window;

#ifdef USE_SDL2
    // SDL2: window is already current context
    SDL_Window *sdlWindow = static_cast<SDL_Window *>(window);
    if (!sdlWindow)
    {
        LOG_ERROR("Invalid SDL2 window for ImGui initialization");
        return false;
    }
#else
    // GLFW: ensure context is active
    GLFWwindow *glfwWindow = static_cast<GLFWwindow *>(window);
    if (!glfwWindow)
    {
        LOG_ERROR("Invalid GLFW window for ImGui initialization");
        return false;
    }
    glfwMakeContextCurrent(glfwWindow);
#endif

    // Verify OpenGL functions are loaded before initializing ImGui
    if (!glGenVertexArrays)
    {
        LOG_ERROR("OpenGL functions not loaded. Loading now...");
        if (!loadOpenGLFunctions())
        {
            LOG_ERROR("Failed to load OpenGL functions for ImGui");
            return false;
        }
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.IniFilename = "RetroCapture.ini";
    std::string oldIniPath = "imgui.ini";
    if (fs::exists(oldIniPath))
    {
        fs::remove(oldIniPath);
        LOG_INFO("Old configuration file removed: " + oldIniPath);
    }

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    // Usar versão GLSL dinâmica baseada na versão OpenGL disponível
    std::string glslVersion = getGLSLVersionString();
#ifdef USE_SDL2
    ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window *>(window), nullptr);
    ImGui_ImplOpenGL3_Init(glslVersion.c_str());
#else
    ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow *>(window), true);
    ImGui_ImplOpenGL3_Init(glslVersion.c_str());
#endif

    // Scan for shaders (check environment variable for AppImage support)
    const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
    if (envShaderPath && fs::exists(envShaderPath))
    {
        m_shaderBasePath = envShaderPath;
    }
    scanShaders(m_shaderBasePath);

    loadConfig();

    m_configWindow = std::make_unique<UIConfiguration>(this);
    m_creditsWindow = std::make_unique<UICredits>(this);
    m_capturePresetsWindow = std::make_unique<UICapturePresets>(this);
    m_recordingsWindow = std::make_unique<UIRecordings>(this);
    m_configWindow->setVisible(true);
    m_configWindow->setJustOpened(true);

    m_initialized = true;
    LOG_INFO("UIManager initialized");
    return true;
}

void UIManager::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    std::string oldIniPath = "imgui.ini";
    if (fs::exists(oldIniPath))
    {
        fs::remove(oldIniPath);
        LOG_INFO("Old configuration file removed during shutdown: " + oldIniPath);
    }

    ImGui_ImplOpenGL3_Shutdown();
#ifdef USE_SDL2
    ImGui_ImplSDL2_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
    ImGui::DestroyContext();

    m_initialized = false;
}

void UIManager::beginFrame()
{
    if (!m_initialized)
    {
        return;
    }

    // Disable ImGui mouse input BEFORE processing events when UI is hidden
    // This prevents ImGui from processing mouse events and controlling cursor
    ImGuiIO& io = ImGui::GetIO();
    if (!m_uiVisible)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        // Also disable mouse buttons to prevent any mouse interaction
        io.MouseDown[0] = false;
        io.MouseDown[1] = false;
        io.MouseDown[2] = false;
        io.MouseDown[3] = false;
        io.MouseDown[4] = false;
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
    
    // Always call NewFrame, even when UI is hidden (maintains ImGui state)
    ImGui_ImplOpenGL3_NewFrame();
#ifdef USE_SDL2
    ImGui_ImplSDL2_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
    
    // CRITICAL: Ensure mouse is disabled after NewFrame
    // ImGui backends (ImGui_ImplGlfw_NewFrame/ImGui_ImplSDL2_NewFrame) may re-enable mouse input
    // and restore cursor visibility, so we must disable it again
    if (!m_uiVisible)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        // Also clear mouse position to prevent any mouse interaction
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MousePosPrev = ImVec2(-FLT_MAX, -FLT_MAX);
    }
}

void UIManager::endFrame()
{
    if (!m_initialized)
    {
        return;
    }

    if (m_uiVisible)
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    else
    {
        // Quando oculta, ainda precisamos finalizar o frame para manter o estado correto
        ImGui::EndFrame();
    }
    
    // Keep ImGui mouse disabled when UI is hidden
    // This prevents ImGui from interfering with cursor visibility
    ImGuiIO& io = ImGui::GetIO();
    if (!m_uiVisible)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }
    else
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
}

void UIManager::setVisible(bool visible)
{
    if (m_uiVisible != visible)
    {
        m_uiVisible = visible;
        if (m_onVisibilityChanged)
        {
            m_onVisibilityChanged(visible);
        }
    }
}

void UIManager::render()
{
    if (!m_initialized || !m_uiVisible)
    {
        return;
    }

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Rescan Shaders"))
            {
                scanShaders(m_shaderBasePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Esc"))
            {
                if (m_window)
                {
#ifdef USE_SDL2
                    // SDL2: Window close is handled via SDL_QUIT event in pollEvents
                    // Can't directly close window from here, but can set flag
#else
                    glfwSetWindowShouldClose(static_cast<GLFWwindow *>(m_window), GLFW_TRUE);
#endif
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Toggle UI", "F12"))
            {
                setVisible(!m_uiVisible);
            }
            ImGui::Separator();
            if (m_configWindow)
            {
                bool visible = m_configWindow->isVisible();
                if (ImGui::MenuItem("Configuration", nullptr, visible))
                {
                    m_configWindow->setVisible(!visible);
                }
            }
            if (m_capturePresetsWindow)
            {
                bool visible = m_capturePresetsWindow->isVisible();
                if (ImGui::MenuItem("Capture Presets", nullptr, visible))
                {
                    m_capturePresetsWindow->setVisible(!visible);
                }
            }
            if (m_recordingsWindow)
            {
                bool visible = m_recordingsWindow->isVisible();
                if (ImGui::MenuItem("Recordings", nullptr, visible))
                {
                    m_recordingsWindow->setVisible(!visible);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (m_creditsWindow)
            {
                bool visible = m_creditsWindow->isVisible();
                if (ImGui::MenuItem("Credits", nullptr, visible))
                {
                    m_creditsWindow->setVisible(!visible);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Renderizar janela de configuração
    if (m_configWindow)
    {
        m_configWindow->render();
    }

    // Renderizar janela de créditos
    if (m_creditsWindow)
    {
        m_creditsWindow->render();
    }

    // Renderizar janela de presets
    if (m_capturePresetsWindow)
    {
        m_capturePresetsWindow->render();
    }

    // Renderizar janela de gravações
    if (m_recordingsWindow)
    {
        m_recordingsWindow->render();
    }
}

void UIManager::renderShaderPanel()
{
    ImGui::Text("Shader Preset:");

    // Combo box for shader selection
    if (ImGui::BeginCombo("##shader", m_currentShader.empty() ? "None" : m_currentShader.c_str()))
    {
        if (ImGui::Selectable("None", m_currentShader.empty()))
        {
            m_currentShader = "";
            if (m_onShaderChanged)
            {
                m_onShaderChanged("");
            }
            saveConfig();
        }

        for (size_t i = 0; i < m_scannedShaders.size(); ++i)
        {
            bool isSelected = (m_currentShader == m_scannedShaders[i]);
            if (ImGui::Selectable(m_scannedShaders[i].c_str(), isSelected))
            {
                m_currentShader = m_scannedShaders[i];
                if (m_onShaderChanged)
                {
                    m_onShaderChanged(m_scannedShaders[i]);
                }
                saveConfig();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text("Shaders found: %zu", m_scannedShaders.size());

    // Botões de salvar preset
    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        ImGui::Separator();
        ImGui::Text("Save Preset:");

        std::string currentPreset = m_shaderEngine->getPresetPath();
        if (!currentPreset.empty())
        {
            // Extrair apenas o nome do arquivo
            fs::path presetPath(currentPreset);
            std::string fileName = presetPath.filename();

            if (ImGui::Button("Save"))
            {
                if (m_onSavePreset)
                {
                    m_onSavePreset(currentPreset, true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As..."))
            {
                strncpy(m_savePresetPath, fileName.c_str(), sizeof(m_savePresetPath) - 1);
                m_savePresetPath[sizeof(m_savePresetPath) - 1] = '\0';
                m_showSaveDialog = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No preset loaded");
        }

        // Dialog para "Save As"
        if (m_showSaveDialog)
        {
            ImGui::OpenPopup("Save Preset As");
            m_showSaveDialog = false;
        }

        if (ImGui::BeginPopupModal("Save Preset As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Enter preset filename:");
            ImGui::InputText("##presetname", m_savePresetPath, sizeof(m_savePresetPath));

            if (ImGui::Button("Save"))
            {
                if (m_onSavePreset && strlen(m_savePresetPath) > 0)
                {
                    // Construir caminho completo
                    fs::path basePath("shaders/shaders_glsl");
                    fs::path newPath = basePath / m_savePresetPath;
                    if (newPath.extension() != ".glslp")
                    {
                        newPath.replace_extension(".glslp");
                    }
                    m_onSavePreset(newPath.string(), false);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (m_shaderEngine && m_shaderEngine->isShaderActive())
    {
        ImGui::Separator();
        ImGui::Text("Shader Parameters:");

        auto params = m_shaderEngine->getShaderParameters();
        if (params.empty())
        {
            ImGui::TextDisabled("No parameters available");
        }
        else
        {
            for (auto &param : params)
            {
                ImGui::PushID(param.name.c_str());

                if (!param.description.empty())
                {
                    ImGui::Text("%s", param.description.c_str());
                }
                else
                {
                    ImGui::Text("%s", param.name.c_str());
                }

                // Slider para o parâmetro
                float value = param.value;
                if (ImGui::SliderFloat("##param", &value, param.min, param.max, "%.3f"))
                {
                    m_shaderEngine->setShaderParameter(param.name, value);
                }

                ImGui::SameLine();
                if (ImGui::Button("Reset##param"))
                {
                    m_shaderEngine->setShaderParameter(param.name, param.defaultValue);
                }

                ImGui::PopID();
            }
        }
    }
}

void UIManager::renderImageControls()
{
    ImGui::Text("Image Adjustments");
    ImGui::Separator();

    // Brightness
    float brightness = m_brightness;
    if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 2.0f, "%.2f"))
    {
        m_brightness = brightness;
        if (m_onBrightnessChanged)
        {
            m_onBrightnessChanged(brightness);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##brightness"))
    {
        m_brightness = 1.0f;
        if (m_onBrightnessChanged)
        {
            m_onBrightnessChanged(1.0f);
        }
    }

    // Contrast
    float contrast = m_contrast;
    if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 5.0f, "%.2f"))
    {
        m_contrast = contrast;
        if (m_onContrastChanged)
        {
            m_onContrastChanged(contrast);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##contrast"))
    {
        m_contrast = 1.0f;
        if (m_onContrastChanged)
        {
            m_onContrastChanged(1.0f);
        }
    }

    ImGui::Separator();

    // Maintain aspect ratio
    bool maintainAspect = m_maintainAspect;
    if (ImGui::Checkbox("Maintain Aspect Ratio", &maintainAspect))
    {
        m_maintainAspect = maintainAspect;
        if (m_onMaintainAspectChanged)
        {
            m_onMaintainAspectChanged(maintainAspect);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Fullscreen
    bool fullscreen = m_fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen))
    {
        m_fullscreen = fullscreen;
        if (m_onFullscreenChanged)
        {
            m_onFullscreenChanged(fullscreen);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Monitor Index (usado quando fullscreen está ativo)
    ImGui::Separator();
    ImGui::Text("Monitor Index:");
    if (!fullscreen && !m_fullscreen)
    {
        ImGui::TextDisabled("(only used in fullscreen mode)");
    }
    else
    {
        ImGui::TextDisabled("(-1 = primary monitor, 0+ = specific monitor)");
    }
    int monitorIndex = m_monitorIndex;
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("##monitor", &monitorIndex, 1, 5))
    {
        monitorIndex = std::max(-1, monitorIndex); // Não permitir valores negativos menores que -1
        m_monitorIndex = monitorIndex;
        if (m_onMonitorIndexChanged)
        {
            m_onMonitorIndexChanged(monitorIndex);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Reset##monitor"))
    {
        m_monitorIndex = -1;
        if (m_onMonitorIndexChanged)
        {
            m_onMonitorIndexChanged(-1);
        }
    }
}

void UIManager::renderSourcePanel()
{
    ImGui::Text("Source Type:");
    ImGui::Separator();
    ImGui::Spacing();

    // Dropdown para seleção do tipo de fonte
    const char *sourceTypeNames[] = {"None", "V4L2"};
    int currentSourceType = static_cast<int>(m_sourceType);

    if (ImGui::Combo("##sourceType", &currentSourceType, sourceTypeNames, IM_ARRAYSIZE(sourceTypeNames)))
    {
        m_sourceType = static_cast<SourceType>(currentSourceType);
        if (m_onSourceTypeChanged)
        {
            m_onSourceTypeChanged(m_sourceType);
        }
        saveConfig();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Renderizar controles específicos da fonte selecionada
    if (m_sourceType == SourceType::V4L2)
    {
        renderV4L2Controls();
    }
    else if (m_sourceType == SourceType::None)
    {
        ImGui::TextWrapped("Nenhuma fonte selecionada. Selecione um tipo de fonte acima.");
    }
}

void UIManager::renderV4L2Controls()
{
    // Sempre mostrar controles, mesmo sem dispositivo
    // Se não houver dispositivo, mostrar mensagem informativa
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("Nenhum dispositivo V4L2 conectado. Selecione um dispositivo abaixo para iniciar a captura.");
        ImGui::Separator();
    }

    // Device selection
    ImGui::Text("V4L2 Device:");
    ImGui::Separator();

    // Scan devices if list is empty
    if (m_v4l2Devices.empty())
    {
        refreshV4L2Devices();
    }

    // Combo box for device selection
    // Adicionar "None" como primeira opção
    std::string displayText = m_currentDevice.empty() ? "None (No device)" : m_currentDevice;
    int selectedIndex = -1;

    // Verificar se "None" está selecionado
    if (m_currentDevice.empty())
    {
        selectedIndex = 0; // "None" é o índice 0
    }
    else
    {
        // Procurar dispositivo na lista (índice +1 porque "None" é 0)
        for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
        {
            if (m_v4l2Devices[i] == m_currentDevice)
            {
                selectedIndex = static_cast<int>(i) + 1; // +1 porque "None" é 0
                break;
            }
        }
    }

    if (ImGui::BeginCombo("##device", displayText.c_str()))
    {
        // Opção "None" sempre como primeira opção
        bool isNoneSelected = m_currentDevice.empty();
        if (ImGui::Selectable("None (No device)", isNoneSelected))
        {
            m_currentDevice = ""; // String vazia = None
            if (m_onDeviceChanged)
            {
                m_onDeviceChanged(""); // Passar string vazia para indicar "None"
            }
            saveConfig();
        }
        if (isNoneSelected)
        {
            ImGui::SetItemDefaultFocus();
        }

        // Listar dispositivos disponíveis
        for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i) + 1);
            if (ImGui::Selectable(m_v4l2Devices[i].c_str(), isSelected))
            {
                m_currentDevice = m_v4l2Devices[i];
                if (m_onDeviceChanged)
                {
                    m_onDeviceChanged(m_v4l2Devices[i]);
                }
                saveConfig();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh##devices"))
    {
        refreshV4L2Devices();
    }

    ImGui::Separator();
    ImGui::Text("Capture Resolution & Framerate");
    ImGui::Separator();

    // Controles de resolução
    ImGui::Text("Resolution:");
    int width = static_cast<int>(m_captureWidth);
    int height = static_cast<int>(m_captureHeight);

    ImGui::PushItemWidth(100);
    ImGui::PushID("width");
    bool widthEdited = ImGui::InputInt("Width##capture", &width, 1, 10);
    width = std::max(1, std::min(7680, width)); // Limitar entre 1 e 7680
    bool widthDeactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();

    ImGui::SameLine();

    ImGui::PushID("height");
    bool heightEdited = ImGui::InputInt("Height##capture", &height, 1, 10);
    height = std::max(1, std::min(4320, height)); // Limitar entre 1 e 4320
    bool heightDeactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    ImGui::PopItemWidth();

    // Aplicar mudanças quando qualquer campo perder o foco
    if ((widthDeactivated || heightDeactivated) && (widthEdited || heightEdited))
    {
        if (width != static_cast<int>(m_captureWidth) || height != static_cast<int>(m_captureHeight))
        {
            m_captureWidth = static_cast<uint32_t>(width);
            m_captureHeight = static_cast<uint32_t>(height);
            if (m_onResolutionChanged)
            {
                m_onResolutionChanged(m_captureWidth, m_captureHeight);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }

    // Controle de FPS
    ImGui::Text("Framerate:");
    int fps = static_cast<int>(m_captureFps);
    ImGui::PushItemWidth(100);
    bool fpsEdited = ImGui::InputInt("FPS##capture", &fps, 1, 5);
    fps = std::max(1, std::min(240, fps)); // Limitar entre 1 e 240
    ImGui::PopItemWidth();

    // Aplicar mudanças quando o campo perder o foco
    if (ImGui::IsItemDeactivatedAfterEdit() && fpsEdited)
    {
        if (fps != static_cast<int>(m_captureFps))
        {
            m_captureFps = static_cast<uint32_t>(fps);
            if (m_onFramerateChanged)
            {
                m_onFramerateChanged(m_captureFps);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }

    // FPS comuns (botões rápidos)
    ImGui::Text("Quick FPS:");
    if (ImGui::Button("30"))
    {
        m_captureFps = 30;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(30);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("60"))
    {
        m_captureFps = 60;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(60);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("120"))
    {
        m_captureFps = 120;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(120);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();

    // Resoluções 4:3
    ImGui::Text("4:3 Resolutions:");
    if (ImGui::Button("320x240"))
    {
        m_captureWidth = 320;
        m_captureHeight = 240;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(320, 240);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("640x480"))
    {
        m_captureWidth = 640;
        m_captureHeight = 480;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(640, 480);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("800x600"))
    {
        m_captureWidth = 800;
        m_captureHeight = 600;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(800, 600);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("1024x768"))
    {
        m_captureWidth = 1024;
        m_captureHeight = 768;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1024, 768);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1280x960"))
    {
        m_captureWidth = 1280;
        m_captureHeight = 960;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1280, 960);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1600x1200"))
    {
        m_captureWidth = 1600;
        m_captureHeight = 1200;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1600, 1200);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("2048x1536"))
    {
        m_captureWidth = 2048;
        m_captureHeight = 1536;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2048, 1536);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1920"))
    {
        m_captureWidth = 2560;
        m_captureHeight = 1920;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2560, 1920);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();

    // Resoluções 16:9
    ImGui::Text("16:9 Resolutions:");
    if (ImGui::Button("1280x720"))
    {
        m_captureWidth = 1280;
        m_captureHeight = 720;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1280, 720);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("1920x1080"))
    {
        m_captureWidth = 1920;
        m_captureHeight = 1080;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1920, 1080);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1440"))
    {
        m_captureWidth = 2560;
        m_captureHeight = 1440;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2560, 1440);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    if (ImGui::Button("3840x2160"))
    {
        m_captureWidth = 3840;
        m_captureHeight = 2160;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(3840, 2160);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();
    ImGui::Text("V4L2 Hardware Controls");
    ImGui::Separator();

    // Renderizar controles dinâmicos (discovered from device)
    for (size_t i = 0; i < m_v4l2Controls.size(); ++i)
    {
        auto &control = m_v4l2Controls[i];
        if (!control.available)
        {
            continue;
        }

        // Use PushID to create unique IDs for each control
        ImGui::PushID(static_cast<int>(i));
        std::string label = control.name + "##dynamic";
        int32_t value = control.value;
        if (ImGui::SliderInt(label.c_str(), &value, control.min, control.max))
        {
            control.value = value;
            if (m_onV4L2ControlChanged)
            {
                m_onV4L2ControlChanged(control.name, value);
            }
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Text("All V4L2 Controls:");
    ImGui::Separator();

    // Helper function para renderizar controle com range do dispositivo ou padrão
    auto renderControl = [this](const char *name, int32_t defaultMin, int32_t defaultMax, int32_t defaultValue)
    {
        if (!m_capture)
            return;

        int32_t value, min, max;

        // Tentar obter valores do dispositivo usando interface genérica
        if (m_capture->getControl(name, value) &&
            m_capture->getControlMin(name, min) &&
            m_capture->getControlMax(name, max))
        {
            // Valores obtidos com sucesso
        }
        else
        {
            // Se não disponível, usar valores padrão
            min = defaultMin;
            max = defaultMax;
            value = defaultValue;
        }

        // Clamp valor
        value = std::max(min, std::min(max, value));

        // Use unique ID with suffix to avoid conflicts with dynamic controls
        std::string label = std::string(name) + "##manual";
        if (ImGui::SliderInt(label.c_str(), &value, min, max))
        {
            value = std::max(min, std::min(max, value));

            if (m_onV4L2ControlChanged)
            {
                m_onV4L2ControlChanged(name, value);
            }
        }
    };

    // Brightness
    renderControl("Brightness", -100, 100, 0);

    // Contrast
    renderControl("Contrast", -100, 100, 0);

    // Saturation
    renderControl("Saturation", -100, 100, 0);

    // Hue
    renderControl("Hue", -100, 100, 0);

    // Gain
    renderControl("Gain", 0, 100, 0);

    // Exposure
    renderControl("Exposure", -13, 1, 0);

    // Sharpness
    renderControl("Sharpness", 0, 6, 0);

    // Gamma
    renderControl("Gamma", 100, 300, 100);

    // White Balance
    renderControl("White Balance", 2800, 6500, 4000);
}

void UIManager::setCaptureControls(IVideoCapture *capture)
{
    m_capture = capture;
    m_v4l2Controls.clear();

    if (!capture)
    {
        return;
    }

    // Se o tipo de fonte for DirectShow, atualizar lista de dispositivos
#ifdef _WIN32
    if (m_sourceType == SourceType::DS)
    {
        // Sempre atualizar lista quando m_capture é setado
        refreshDSDevices();
        LOG_INFO("DirectShow device list updated after setCaptureControls: " + std::to_string(m_dsDevices.size()) + " device(s)");
    }
#endif

    // Lista de controles comuns (usando interface genérica)
    const char *controlNames[] = {
        "Brightness",
        "Contrast",
        "Saturation",
        "Hue",
        "Gain",
        "Exposure",
        "Sharpness",
        "Gamma",
        "White Balance",
    };

    for (const char *name : controlNames)
    {
        V4L2Control ctrl;
        ctrl.name = name;

        // Usar interface genérica para obter informações do controle
        // Verificar se o dispositivo está aberto antes de tentar obter controles
        if (!capture->isOpen())
        {
            ctrl.available = false;
            m_v4l2Controls.push_back(ctrl);
            continue;
        }

        int32_t value, minVal, maxVal;
        if (capture->getControl(name, value) &&
            capture->getControlMin(name, minVal) &&
            capture->getControlMax(name, maxVal))
        {
            ctrl.value = value;
            ctrl.min = minVal;
            ctrl.max = maxVal;
            ctrl.step = 1; // Step não disponível na interface genérica
            ctrl.available = true;
            m_v4l2Controls.push_back(ctrl);
        }
        else
        {
            // Controle não disponível - não adicionar à lista
            ctrl.available = false;
        }
    }
}

void UIManager::renderInfoPanel()
{
    ImGui::Text("Capture Information");
    ImGui::Separator();

    ImGui::Text("Device: %s", m_captureDevice.c_str());
    ImGui::Text("Resolution: %ux%u", m_captureWidth, m_captureHeight);
    ImGui::Text("FPS: %u", m_captureFps);

    ImGui::Separator();
    ImGui::Text("Application Info");
    ImGui::Text("RetroCapture v0.1.0");
    ImGui::Text("ImGui: %s", ImGui::GetVersion());
}

void UIManager::setCaptureInfo(uint32_t width, uint32_t height, uint32_t fps, const std::string &device)
{
    m_captureWidth = width;
    m_captureHeight = height;
    m_captureFps = fps;
    m_captureDevice = device;
    if (m_currentDevice.empty())
    {
        m_currentDevice = device;
    }
}

void UIManager::renderStreamingPanel()
{
    ImGui::Text("HTTP MPEG-TS Streaming (Áudio + Vídeo)");
    ImGui::Separator();

    // Status
    ImGui::Text("Status: %s", m_streamingActive ? "Ativo" : "Inativo");
    if (m_streamingActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }

    if (m_streamingActive && !m_streamUrl.empty())
    {
        ImGui::Text("URL: %s", m_streamUrl.c_str());
        ImGui::Text("Clientes conectados: %u", m_streamClientCount);
    }

    ImGui::Separator();
    ImGui::Text("Configurações Básicas");
    ImGui::Separator();

    // Controles básicos
    int port = static_cast<int>(m_streamingPort);
    if (ImGui::InputInt("Porta", &port, 1, 100))
    {
        // Validation is done in setStreamingPort/triggerStreamingPortChange
        if (port >= 1024 && port <= 65535)
        {
            triggerStreamingPortChange(static_cast<uint16_t>(port));
        }
    }

    // Resolução - Dropdown
    const char *resolutions[] = {
        "Captura (0x0)",
        "320x240",
        "640x480",
        "800x600",
        "1024x768",
        "1280x720 (HD)",
        "1280x1024",
        "1920x1080 (Full HD)",
        "2560x1440 (2K)",
        "3840x2160 (4K)"};
    const uint32_t resolutionWidths[] = {0, 320, 640, 800, 1024, 1280, 1280, 1920, 2560, 3840};
    const uint32_t resolutionHeights[] = {0, 240, 480, 600, 768, 720, 1024, 1080, 1440, 2160};

    int currentResIndex = 0;
    for (int i = 0; i < 10; i++)
    {
        if (m_streamingWidth == resolutionWidths[i] && m_streamingHeight == resolutionHeights[i])
        {
            currentResIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Resolução", &currentResIndex, resolutions, 10))
    {
        m_streamingWidth = resolutionWidths[currentResIndex];
        m_streamingHeight = resolutionHeights[currentResIndex];
        if (m_onStreamingWidthChanged)
        {
            m_onStreamingWidthChanged(m_streamingWidth);
        }
        if (m_onStreamingHeightChanged)
        {
            m_onStreamingHeightChanged(m_streamingHeight);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // FPS - Dropdown
    const char *fpsOptions[] = {"Captura (0)", "15", "24", "30", "60", "120"};
    const uint32_t fpsValues[] = {0, 15, 24, 30, 60, 120};

    int currentFpsIndex = 0;
    for (int i = 0; i < 6; i++)
    {
        if (m_streamingFps == fpsValues[i])
        {
            currentFpsIndex = i;
            break;
        }
    }

    if (ImGui::Combo("FPS", &currentFpsIndex, fpsOptions, 6))
    {
        m_streamingFps = fpsValues[currentFpsIndex];
        if (m_onStreamingFpsChanged)
        {
            m_onStreamingFpsChanged(m_streamingFps);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    ImGui::Separator();
    ImGui::Text("Codecs");
    ImGui::Separator();

    // Seleção de codec de vídeo
    const char *videoCodecs[] = {"h264", "h265", "vp8", "vp9"};
    int currentVideoCodecIndex = 0;
    for (int i = 0; i < 4; i++)
    {
        if (m_streamingVideoCodec == videoCodecs[i])
        {
            currentVideoCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Codec de Vídeo", &currentVideoCodecIndex, videoCodecs, 4))
    {
        m_streamingVideoCodec = videoCodecs[currentVideoCodecIndex];
        if (m_onStreamingVideoCodecChanged)
        {
            m_onStreamingVideoCodecChanged(m_streamingVideoCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Seleção de codec de áudio
    const char *audioCodecs[] = {"aac", "mp3", "opus"};
    int currentAudioCodecIndex = 0;
    for (int i = 0; i < 3; i++)
    {
        if (m_streamingAudioCodec == audioCodecs[i])
        {
            currentAudioCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Codec de Áudio", &currentAudioCodecIndex, audioCodecs, 3))
    {
        m_streamingAudioCodec = audioCodecs[currentAudioCodecIndex];
        if (m_onStreamingAudioCodecChanged)
        {
            m_onStreamingAudioCodecChanged(m_streamingAudioCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    // Qualidade H.264 (apenas se codec for h264)
    if (m_streamingVideoCodec == "h264")
    {
        const char *h264Presets[] = {
            "ultrafast",
            "superfast",
            "veryfast",
            "faster",
            "fast",
            "medium",
            "slow",
            "slower",
            "veryslow"};
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++)
        {
            if (m_streamingH264Preset == h264Presets[i])
            {
                currentPresetIndex = i;
                break;
            }
        }

        if (ImGui::Combo("Qualidade H.264", &currentPresetIndex, h264Presets, 9))
        {
            m_streamingH264Preset = h264Presets[currentPresetIndex];
            if (m_onStreamingH264PresetChanged)
            {
                m_onStreamingH264PresetChanged(m_streamingH264Preset);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Preset do encoder H.264:\n"
                              "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                              "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                              "slow/slower/veryslow: Máxima qualidade, menor velocidade");
        }
    }

    // Qualidade H.265 (apenas se codec for h265)
    if (m_streamingVideoCodec == "h265" || m_streamingVideoCodec == "hevc")
    {
        const char *h265Presets[] = {
            "ultrafast",
            "superfast",
            "veryfast",
            "faster",
            "fast",
            "medium",
            "slow",
            "slower",
            "veryslow"};
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++)
        {
            if (m_streamingH265Preset == h265Presets[i])
            {
                currentPresetIndex = i;
                break;
            }
        }

        if (ImGui::Combo("Qualidade H.265", &currentPresetIndex, h265Presets, 9))
        {
            m_streamingH265Preset = h265Presets[currentPresetIndex];
            if (m_onStreamingH265PresetChanged)
            {
                m_onStreamingH265PresetChanged(m_streamingH265Preset);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Preset do encoder H.265:\n"
                              "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                              "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                              "slow/slower/veryslow: Máxima qualidade, menor velocidade");
        }

        // Profile H.265
        const char *h265Profiles[] = {"main", "main10"};
        int currentProfileIndex = 0;
        for (int i = 0; i < 2; i++)
        {
            if (m_streamingH265Profile == h265Profiles[i])
            {
                currentProfileIndex = i;
                break;
            }
        }

        if (ImGui::Combo("Profile H.265", &currentProfileIndex, h265Profiles, 2))
        {
            m_streamingH265Profile = h265Profiles[currentProfileIndex];
            if (m_onStreamingH265ProfileChanged)
            {
                m_onStreamingH265ProfileChanged(m_streamingH265Profile);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Profile do encoder H.265:\n"
                              "main: 8-bit, máxima compatibilidade\n"
                              "main10: 10-bit, melhor qualidade, suporte HDR");
        }

        // Level H.265
        const char *h265Levels[] = {
            "auto", "1", "2", "2.1", "3", "3.1",
            "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"};
        int currentLevelIndex = 0;
        for (int i = 0; i < 14; i++)
        {
            if (m_streamingH265Level == h265Levels[i])
            {
                currentLevelIndex = i;
                break;
            }
        }

        if (ImGui::Combo("Level H.265", &currentLevelIndex, h265Levels, 14))
        {
            m_streamingH265Level = h265Levels[currentLevelIndex];
            if (m_onStreamingH265LevelChanged)
            {
                m_onStreamingH265LevelChanged(m_streamingH265Level);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Level do encoder H.265:\n"
                              "auto: Detecção automática (recomendado)\n"
                              "1-6.2: Níveis específicos para compatibilidade\n"
                              "Níveis mais altos suportam resoluções/bitrates maiores");
        }
    }

    // Configurações VP8 (apenas se codec for vp8)
    if (m_streamingVideoCodec == "vp8")
    {
        int currentSpeed = m_streamingVP8Speed;
        if (ImGui::SliderInt("Speed VP8 (0-16)", &currentSpeed, 0, 16))
        {
            m_streamingVP8Speed = currentSpeed;
            if (m_onStreamingVP8SpeedChanged)
            {
                m_onStreamingVP8SpeedChanged(m_streamingVP8Speed);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Speed do encoder VP8:\n"
                              "0: Melhor qualidade, mais lento\n"
                              "16: Mais rápido, menor qualidade\n"
                              "12: Bom equilíbrio para streaming");
        }
    }

    // Configurações VP9 (apenas se codec for vp9)
    if (m_streamingVideoCodec == "vp9")
    {
        int currentSpeed = m_streamingVP9Speed;
        if (ImGui::SliderInt("Speed VP9 (0-9)", &currentSpeed, 0, 9))
        {
            m_streamingVP9Speed = currentSpeed;
            if (m_onStreamingVP9SpeedChanged)
            {
                m_onStreamingVP9SpeedChanged(m_streamingVP9Speed);
            }
            saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Speed do encoder VP9:\n"
                              "0: Melhor qualidade, mais lento\n"
                              "9: Mais rápido, menor qualidade\n"
                              "6: Bom equilíbrio para streaming");
        }
    }

    ImGui::Separator();
    ImGui::Text("Bitrates");
    ImGui::Separator();

    // Bitrate de vídeo
    int bitrate = static_cast<int>(m_streamingBitrate);
    if (ImGui::InputInt("Bitrate Vídeo (kbps, 0 = auto)", &bitrate, 100, 1000))
    {
        // Limites: 0 (auto) ou 100-100000 kbps
        if (bitrate == 0 || (bitrate >= 100 && bitrate <= 100000))
        {
            m_streamingBitrate = static_cast<uint32_t>(bitrate);
            if (m_onStreamingBitrateChanged)
            {
                m_onStreamingBitrateChanged(m_streamingBitrate);
            }
            saveConfig();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bitrate de vídeo em kbps.\n"
                          "0 = automático (baseado na resolução/FPS)\n"
                          "100-100000 kbps: valores válidos\n"
                          "Recomendado: 2000-8000 kbps para streaming");
    }

    // Bitrate de áudio
    int audioBitrate = static_cast<int>(m_streamingAudioBitrate);
    if (ImGui::InputInt("Bitrate Áudio (kbps)", &audioBitrate, 8, 32))
    {
        // Limites: 64-320 kbps (32 é muito baixo para qualidade aceitável)
        if (audioBitrate >= 64 && audioBitrate <= 320)
        {
            m_streamingAudioBitrate = static_cast<uint32_t>(audioBitrate);
            if (m_onStreamingAudioBitrateChanged)
            {
                m_onStreamingAudioBitrateChanged(m_streamingAudioBitrate);
            }
            saveConfig();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bitrate de áudio em kbps.\n"
                          "64-320 kbps: valores válidos\n"
                          "Recomendado: 128-256 kbps para boa qualidade");
    }

    ImGui::Separator();
    ImGui::Text("Buffer (Avançado)");
    ImGui::Separator();

    // Max Video Buffer Size
    int maxVideoBuffer = static_cast<int>(m_streamingMaxVideoBufferSize);
    if (ImGui::SliderInt("Max Frames no Buffer", &maxVideoBuffer, 1, 50))
    {
        m_streamingMaxVideoBufferSize = static_cast<size_t>(maxVideoBuffer);
        if (m_onStreamingMaxVideoBufferSizeChanged)
        {
            m_onStreamingMaxVideoBufferSizeChanged(m_streamingMaxVideoBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Máximo de frames de vídeo no buffer.\n"
                          "1-50 frames: valores válidos\n"
                          "Padrão: 10 frames\n"
                          "Valores maiores = mais memória, menos risco de perda de frames");
    }

    // Max Audio Buffer Size
    int maxAudioBuffer = static_cast<int>(m_streamingMaxAudioBufferSize);
    if (ImGui::SliderInt("Max Chunks no Buffer", &maxAudioBuffer, 5, 100))
    {
        m_streamingMaxAudioBufferSize = static_cast<size_t>(maxAudioBuffer);
        if (m_onStreamingMaxAudioBufferSizeChanged)
        {
            m_onStreamingMaxAudioBufferSizeChanged(m_streamingMaxAudioBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Máximo de chunks de áudio no buffer.\n"
                          "5-100 chunks: valores válidos\n"
                          "Padrão: 20 chunks\n"
                          "Valores maiores = mais memória, melhor sincronização");
    }

    // Max Buffer Time
    int maxBufferTime = static_cast<int>(m_streamingMaxBufferTimeSeconds);
    if (ImGui::SliderInt("Max Tempo de Buffer (segundos)", &maxBufferTime, 1, 30))
    {
        m_streamingMaxBufferTimeSeconds = static_cast<int64_t>(maxBufferTime);
        if (m_onStreamingMaxBufferTimeSecondsChanged)
        {
            m_onStreamingMaxBufferTimeSecondsChanged(m_streamingMaxBufferTimeSeconds);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Tempo máximo de buffer em segundos.\n"
                          "1-30 segundos: valores válidos\n"
                          "Padrão: 5 segundos\n"
                          "Controla quanto tempo de vídeo/áudio pode ser armazenado antes de processar");
    }

    // AVIO Buffer Size
    int avioBuffer = static_cast<int>(m_streamingAVIOBufferSize / 1024); // Converter para KB
    if (ImGui::SliderInt("AVIO Buffer (KB)", &avioBuffer, 64, 1024))
    {
        m_streamingAVIOBufferSize = static_cast<size_t>(avioBuffer * 1024);
        if (m_onStreamingAVIOBufferSizeChanged)
        {
            m_onStreamingAVIOBufferSizeChanged(m_streamingAVIOBufferSize);
        }
        saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Tamanho do buffer AVIO do FFmpeg em KB.\n"
                          "64-1024 KB: valores válidos\n"
                          "Padrão: 256 KB\n"
                          "Buffer interno do FFmpeg para I/O de streaming");
    }

    ImGui::Separator();

    // Botão Start/Stop
    // Desabilitar botão se estiver processando (start/stop em andamento)
    if (m_streamingProcessing)
    {
        ImGui::BeginDisabled();
        if (m_streamingActive)
        {
            ImGui::Button("Parando...", ImVec2(-1, 0));
        }
        else
        {
            ImGui::Button("Iniciando...", ImVec2(-1, 0));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Aguarde o processo terminar");
        }
    }
    else if (m_streamingActive)
    {
        if (ImGui::Button("Parar Streaming", ImVec2(-1, 0)))
        {
            if (m_onStreamingStartStop)
            {
                m_streamingProcessing = true; // Marcar como processando
                m_onStreamingStartStop(false);
            }
        }
    }
    else
    {
        // Desabilitar botão se estiver em cooldown
        if (m_streamingCooldownRemainingMs > 0)
        {
            ImGui::BeginDisabled();
            float cooldownSeconds = m_streamingCooldownRemainingMs / 1000.0f;
            std::string label = "Aguardando (" + std::to_string(static_cast<int>(cooldownSeconds)) + "s)";
            ImGui::Button(label.c_str(), ImVec2(-1, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Aguarde o cooldown terminar antes de iniciar o streaming novamente");
            }
        }
        else
        {
            if (ImGui::Button("Iniciar Streaming", ImVec2(-1, 0)))
            {
                if (m_onStreamingStartStop)
                {
                    m_streamingProcessing = true; // Marcar como processando
                    m_onStreamingStartStop(true);
                }
            }
        }
    }
}

void UIManager::scanV4L2Devices()
{
#ifdef PLATFORM_LINUX
    m_v4l2Devices = V4L2DeviceScanner::scan();
#else
    // Windows não usa V4L2
    m_v4l2Devices.clear();
#endif
}

void UIManager::refreshV4L2Devices()
{
    scanV4L2Devices();
}

void UIManager::refreshDSDevices()
{
    if (!m_capture)
    {
        // Não limpar a lista - pode ter sido populada anteriormente
        // Apenas não atualizar se m_capture não estiver disponível
        return;
    }

    m_dsDevices = m_capture->listDevices();
}

void UIManager::refreshAVFoundationDevices()
{
    if (!m_capture)
    {
        // Não limpar a lista - pode ter sido populada anteriormente
        // Apenas não atualizar se m_capture não estiver disponível
        LOG_WARN("refreshAVFoundationDevices: m_capture não disponível");
        return;
    }

    LOG_INFO("Atualizando lista de dispositivos AVFoundation...");
    m_avfoundationDevices = m_capture->listDevices();
    LOG_INFO("Lista atualizada: " + std::to_string(m_avfoundationDevices.size()) + " dispositivo(s) encontrado(s)");
    
    // Refresh formats for current device
    if (!m_currentDevice.empty())
    {
        refreshAVFoundationFormats(m_currentDevice);
    }
}

void UIManager::refreshAVFoundationFormats(const std::string &deviceId)
{
    m_avfoundationFormats.clear();
    
    if (m_capture)
    {
        std::string targetDeviceId = deviceId.empty() ? m_currentDevice : deviceId;
        m_avfoundationFormats = m_capture->listFormats(targetDeviceId);
        LOG_INFO("Formatos atualizados: " + std::to_string(m_avfoundationFormats.size()) + " formato(s) encontrado(s)");
    }
}

void UIManager::refreshAVFoundationAudioDevices()
{
    m_avfoundationAudioDevices.clear();
    
    if (!m_capture)
    {
        return;
    }
    
    // Use virtual method from IVideoCapture interface
    m_avfoundationAudioDevices = m_capture->listAudioDevices();
    LOG_INFO("Dispositivos de áudio atualizados: " + std::to_string(m_avfoundationAudioDevices.size()) + " dispositivo(s) encontrado(s)");
}

void UIManager::setAVFoundationAudioDevice(const std::string &audioDeviceId)
{
    m_avfoundationAudioDeviceId = audioDeviceId;
    
    if (!m_capture)
    {
        return;
    }
    
    // Use virtual method from IVideoCapture interface
    m_capture->setAudioDevice(audioDeviceId);
    
    // If device is open and not in dummy mode, reopen it to apply the new audio device
    // The triggerDeviceChange callback has protection against infinite loops
    if (m_capture->isOpen() && !m_capture->isDummyMode() && !m_currentDevice.empty())
    {
        LOG_INFO("Audio device changed to: " + (audioDeviceId.empty() ? "auto-detect" : audioDeviceId));
        LOG_INFO("Reopening device to apply new audio device selection...");
        // Trigger device change with current device to reopen and apply new audio device
        // The callback has protection against recursion, so this is safe
        triggerDeviceChange(m_currentDevice);
    }
    else
    {
        LOG_INFO("Audio device changed to: " + (audioDeviceId.empty() ? "auto-detect" : audioDeviceId));
        LOG_INFO("Audio device will be applied when device is next opened/reopened.");
    }
}

std::vector<AVFoundationFormatInfo> UIManager::getAVFoundationFormats(const std::string &deviceId)
{
    // Refresh formats if empty or device changed
    std::string targetDeviceId = deviceId.empty() ? m_currentDevice : deviceId;
    if (m_avfoundationFormats.empty() || (!targetDeviceId.empty() && targetDeviceId != m_currentDevice))
    {
        refreshAVFoundationFormats(targetDeviceId);
    }
    return m_avfoundationFormats;
}

void UIManager::setAVFoundationFormat(int formatIndex, const std::string &deviceId)
{
    if (!m_capture)
        return;
        
    std::string targetDeviceId = deviceId.empty() ? m_currentDevice : deviceId;
    auto formats = getAVFoundationFormats(targetDeviceId);
    if (formatIndex >= 0 && formatIndex < static_cast<int>(formats.size()))
    {
        const auto& format = formats[formatIndex];
        if (m_capture->setFormatById(format.id, targetDeviceId))
        {
            m_currentFormatId = format.id;
            LOG_INFO("Format set by index: " + std::to_string(formatIndex));
            
            // Update resolution from format
            if (m_onResolutionChanged)
            {
                m_onResolutionChanged(format.width, format.height);
            }
            
            // Update FPS to maximum supported by format (or 30 if max is too high)
            uint32_t targetFps = static_cast<uint32_t>(format.maxFps);
            if (targetFps > 60)
            {
                targetFps = 60; // Cap at 60 for most use cases
            }
            else if (targetFps < 30)
            {
                targetFps = static_cast<uint32_t>(format.maxFps); // Use max if less than 30
            }
            
            // Update FPS in capture and trigger callback
            m_capture->setFramerate(targetFps);
            if (m_onFramerateChanged)
            {
                m_onFramerateChanged(targetFps);
            }
        }
    }
}

void UIManager::setAVFoundationFormatById(const std::string &formatId, const std::string &deviceId)
{
    if (!m_capture)
        return;
        
    std::string targetDeviceId = deviceId.empty() ? m_currentDevice : deviceId;
    
    // Refresh formats list before applying format to ensure we have the latest list
    refreshAVFoundationFormats(targetDeviceId);
    
    if (m_capture->setFormatById(formatId, targetDeviceId))
    {
        m_currentFormatId = formatId;
        LOG_INFO("Format set by ID: " + formatId);
        LOG_INFO("Current format ID stored: " + m_currentFormatId);
        
        // Find format info and update internal state
        // NOTE: Do NOT trigger callbacks here - setFormatById already handles
        // format and framerate application internally. Triggering callbacks
        // would cause duplicate reconfiguration and potential crashes.
        auto formats = getAVFoundationFormats(targetDeviceId);
        for (const auto& format : formats)
        {
            if (format.id == formatId)
            {
                // Update internal state without triggering callbacks
                // The format has already been applied by setFormatById
                m_captureWidth = format.width;
                m_captureHeight = format.height;
                
                // Calculate target FPS (capped at 60 or use max if less than 30)
                uint32_t targetFps = static_cast<uint32_t>(format.maxFps);
                if (targetFps > 60)
                {
                    targetFps = 60; // Cap at 60 for most use cases
                }
                else if (targetFps < 30)
                {
                    targetFps = static_cast<uint32_t>(format.maxFps); // Use max if less than 30
                }
                
                m_captureFps = targetFps;
                
                // Update UI info directly without triggering reconfiguration callbacks
                if (m_capture && m_capture->isOpen())
                {
                    setCaptureInfo(m_capture->getWidth(), m_capture->getHeight(), 
                                  m_captureFps, targetDeviceId);
                }
                
                LOG_INFO("Format applied: " + std::to_string(format.width) + "x" + 
                         std::to_string(format.height) + " @ " + std::to_string(targetFps) + "fps");
                break;
            }
        }
    }
}

void UIManager::setSourceType(SourceType sourceType)
{
    m_sourceType = sourceType;

    // Atualizar cache de dispositivos quando mudar o tipo de fonte
#ifdef _WIN32
    if (sourceType == SourceType::DS)
    {
        // Garantir que m_capture está disponível antes de atualizar
        if (m_capture)
        {
            refreshDSDevices();
        }
        else
        {
            // Se m_capture não estiver disponível, limpar lista
            m_dsDevices.clear();
        }
    }
#endif

    if (m_onSourceTypeChanged)
    {
        m_onSourceTypeChanged(sourceType);
    }
    saveConfig();
}

void UIManager::setStreamingPort(uint16_t port)
{
    // Validate port range (1024-65535)
    // Note: uint16_t max is 65535, so we only need to check >= 1024
    if (port >= 1024)
    {
        m_streamingPort = port;
    }
    else
    {
        LOG_WARN("Invalid streaming port: " + std::to_string(port) + " (must be between 1024 and 65535)");
    }
}

void UIManager::triggerStreamingPortChange(uint16_t port)
{
    // Validate and set port (validation is done in setStreamingPort)
    setStreamingPort(port);
    if (m_onStreamingPortChanged)
    {
        m_onStreamingPortChanged(m_streamingPort);
    }
    saveConfig();
}

void UIManager::triggerStreamingWidthChange(uint32_t width)
{
    m_streamingWidth = width;
    if (m_onStreamingWidthChanged)
    {
        m_onStreamingWidthChanged(width);
    }
    saveConfig();
}

void UIManager::triggerStreamingHeightChange(uint32_t height)
{
    m_streamingHeight = height;
    if (m_onStreamingHeightChanged)
    {
        m_onStreamingHeightChanged(height);
    }
    saveConfig();
}

void UIManager::triggerStreamingFpsChange(uint32_t fps)
{
    m_streamingFps = fps;
    if (m_onStreamingFpsChanged)
    {
        m_onStreamingFpsChanged(fps);
    }
    saveConfig();
}

void UIManager::triggerStreamingBitrateChange(uint32_t bitrate)
{
    m_streamingBitrate = bitrate;
    if (m_onStreamingBitrateChanged)
    {
        m_onStreamingBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerStreamingAudioBitrateChange(uint32_t bitrate)
{
    m_streamingAudioBitrate = bitrate;
    if (m_onStreamingAudioBitrateChanged)
    {
        m_onStreamingAudioBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerStreamingVideoCodecChange(const std::string &codec)
{
    m_streamingVideoCodec = codec;
    if (m_onStreamingVideoCodecChanged)
    {
        m_onStreamingVideoCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerStreamingAudioCodecChange(const std::string &codec)
{
    m_streamingAudioCodec = codec;
    if (m_onStreamingAudioCodecChanged)
    {
        m_onStreamingAudioCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerStreamingH264PresetChange(const std::string &preset)
{
    m_streamingH264Preset = preset;
    if (m_onStreamingH264PresetChanged)
    {
        m_onStreamingH264PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265PresetChange(const std::string &preset)
{
    m_streamingH265Preset = preset;
    if (m_onStreamingH265PresetChanged)
    {
        m_onStreamingH265PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265ProfileChange(const std::string &profile)
{
    m_streamingH265Profile = profile;
    if (m_onStreamingH265ProfileChanged)
    {
        m_onStreamingH265ProfileChanged(profile);
    }
    saveConfig();
}

void UIManager::triggerStreamingH265LevelChange(const std::string &level)
{
    m_streamingH265Level = level;
    if (m_onStreamingH265LevelChanged)
    {
        m_onStreamingH265LevelChanged(level);
    }
    saveConfig();
}

void UIManager::triggerDeviceChange(const std::string &device)
{
    // If device changed, clear formats list to force refresh
    if (m_currentDevice != device)
    {
        m_avfoundationFormats.clear();
    }
    m_currentDevice = device;
    if (m_onDeviceChanged)
    {
        m_onDeviceChanged(device);
    }
}

void UIManager::triggerStreamingVP8SpeedChange(int speed)
{
    m_streamingVP8Speed = speed;
    if (m_onStreamingVP8SpeedChanged)
    {
        m_onStreamingVP8SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerStreamingVP9SpeedChange(int speed)
{
    m_streamingVP9Speed = speed;
    if (m_onStreamingVP9SpeedChanged)
    {
        m_onStreamingVP9SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerStreamingMaxVideoBufferSizeChange(size_t size)
{
    m_streamingMaxVideoBufferSize = size;
    if (m_onStreamingMaxVideoBufferSizeChanged)
    {
        m_onStreamingMaxVideoBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingMaxAudioBufferSizeChange(size_t size)
{
    m_streamingMaxAudioBufferSize = size;
    if (m_onStreamingMaxAudioBufferSizeChanged)
    {
        m_onStreamingMaxAudioBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingMaxBufferTimeSecondsChange(int64_t seconds)
{
    m_streamingMaxBufferTimeSeconds = seconds;
    if (m_onStreamingMaxBufferTimeSecondsChanged)
    {
        m_onStreamingMaxBufferTimeSecondsChanged(seconds);
    }
    saveConfig();
}

void UIManager::triggerStreamingAVIOBufferSizeChange(size_t size)
{
    m_streamingAVIOBufferSize = size;
    if (m_onStreamingAVIOBufferSizeChanged)
    {
        m_onStreamingAVIOBufferSizeChanged(size);
    }
    saveConfig();
}

void UIManager::triggerStreamingStartStop(bool start)
{
    if (m_onStreamingStartStop)
    {
        m_onStreamingStartStop(start);
    }
}

void UIManager::triggerWebPortalEnabledChange(bool enabled)
{
    m_webPortalEnabled = enabled;
    if (!enabled && m_webPortalHTTPSEnabled)
    {
        m_webPortalHTTPSEnabled = false;
        if (m_onWebPortalHTTPSChanged)
        {
            m_onWebPortalHTTPSChanged(false);
        }
    }
    if (m_onWebPortalEnabledChanged)
    {
        m_onWebPortalEnabledChanged(enabled);
    }
    saveConfig();
}

void UIManager::triggerWebPortalHTTPSChange(bool enabled)
{
    m_webPortalHTTPSEnabled = enabled;
    if (m_onWebPortalHTTPSChanged)
    {
        m_onWebPortalHTTPSChanged(enabled);
    }
    saveConfig();
}

void UIManager::triggerWebPortalStartStop(bool start)
{
    m_webPortalActive = start;
    if (m_onWebPortalStartStop)
    {
        m_onWebPortalStartStop(start);
    }
}

void UIManager::triggerWebPortalTitleChange(const std::string &title)
{
    m_webPortalTitle = title;
    if (m_onWebPortalTitleChanged)
    {
        m_onWebPortalTitleChanged(title);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSubtitleChange(const std::string &subtitle)
{
    m_webPortalSubtitle = subtitle;
    if (m_onWebPortalSubtitleChanged)
    {
        m_onWebPortalSubtitleChanged(subtitle);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSSLCertPathChange(const std::string &path)
{
    m_webPortalSSLCertPath = path;
    if (m_onWebPortalSSLCertPathChanged)
    {
        m_onWebPortalSSLCertPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalSSLKeyPathChange(const std::string &path)
{
    m_webPortalSSLKeyPath = path;
    if (m_onWebPortalSSLKeyPathChanged)
    {
        m_onWebPortalSSLKeyPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalBackgroundImagePathChange(const std::string &path)
{
    m_webPortalBackgroundImagePath = path;
    if (m_onWebPortalBackgroundImagePathChanged)
    {
        m_onWebPortalBackgroundImagePathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerWebPortalColorsChange()
{
    if (m_onWebPortalColorsChanged)
    {
        m_onWebPortalColorsChanged();
    }
    saveConfig();
}

void UIManager::triggerWebPortalTextsChange()
{
    if (m_onWebPortalTextsChanged)
    {
        m_onWebPortalTextsChanged();
    }
    saveConfig();
}

void UIManager::scanShaders(const std::string &basePath)
{
    m_scannedShaders = ShaderScanner::scan(basePath);
    LOG_INFO("Encontrados " + std::to_string(m_scannedShaders.size()) + " shaders em " + basePath);
}

std::string UIManager::getConfigPath() const
{
#ifdef _WIN32
    // Windows: usar APPDATA (ou LOCALAPPDATA como fallback)
    const char *appDataDir = std::getenv("APPDATA");
    if (!appDataDir)
    {
        appDataDir = std::getenv("LOCALAPPDATA");
    }
    if (appDataDir)
    {
        fs::path configDir = fs::path(appDataDir) / "RetroCapture";
        // Criar diretório se não existir
        if (!fs::exists(configDir))
        {
            fs::create_directories(configDir);
        }
        return (configDir / "config.json").string();
    }
#else
    // Linux/Unix: usar diretório home do usuário
    const char *homeDir = std::getenv("HOME");
    if (homeDir)
    {
        fs::path configDir = fs::path(homeDir) / ".config" / "retrocapture";
        // Criar diretório se não existir
        if (!fs::exists(configDir))
        {
            fs::create_directories(configDir);
        }
        return (configDir / "config.json").string();
    }
#endif
    // Fallback: salvar no diretório atual
    return "retrocapture_config.json";
}

void UIManager::loadConfig()
{
    std::string configPath = getConfigPath();

    if (!fs::exists(configPath))
    {
        LOG_INFO("Configuration file not found: " + configPath + " (using defaults)");
        return;
    }

    try
    {
        std::ifstream file(configPath);
        if (!file.is_open())
        {
            LOG_WARN("Não foi possível abrir arquivo de configuração: " + configPath);
            return;
        }

        nlohmann::json config;
        file >> config;
        file.close();

        // Carregar configurações de streaming
        if (config.contains("streaming"))
        {
            auto &streaming = config["streaming"];
            if (streaming.contains("port"))
                m_streamingPort = streaming["port"];
            if (streaming.contains("width"))
                m_streamingWidth = streaming["width"];
            if (streaming.contains("height"))
                m_streamingHeight = streaming["height"];
            if (streaming.contains("fps"))
                m_streamingFps = streaming["fps"];
            if (streaming.contains("bitrate"))
                m_streamingBitrate = streaming["bitrate"];
            if (streaming.contains("audioBitrate"))
                m_streamingAudioBitrate = streaming["audioBitrate"];
            if (streaming.contains("videoCodec"))
                m_streamingVideoCodec = streaming["videoCodec"].get<std::string>();
            if (streaming.contains("audioCodec"))
                m_streamingAudioCodec = streaming["audioCodec"].get<std::string>();
            if (streaming.contains("h264Preset"))
                m_streamingH264Preset = streaming["h264Preset"].get<std::string>();
            if (streaming.contains("h265Preset"))
                m_streamingH265Preset = streaming["h265Preset"].get<std::string>();
            if (streaming.contains("h265Profile"))
                m_streamingH265Profile = streaming["h265Profile"].get<std::string>();
            if (streaming.contains("h265Level"))
                m_streamingH265Level = streaming["h265Level"].get<std::string>();
            if (streaming.contains("vp8Speed"))
                m_streamingVP8Speed = streaming["vp8Speed"].get<int>();
            if (streaming.contains("vp9Speed"))
                m_streamingVP9Speed = streaming["vp9Speed"].get<int>();

            // Carregar configurações de buffer
            if (streaming.contains("buffer"))
            {
                auto &buffer = streaming["buffer"];
                if (buffer.contains("maxVideoBufferSize"))
                    m_streamingMaxVideoBufferSize = buffer["maxVideoBufferSize"].get<size_t>();
                if (buffer.contains("maxAudioBufferSize"))
                    m_streamingMaxAudioBufferSize = buffer["maxAudioBufferSize"].get<size_t>();
                if (buffer.contains("maxBufferTimeSeconds"))
                    m_streamingMaxBufferTimeSeconds = buffer["maxBufferTimeSeconds"].get<int64_t>();
                if (buffer.contains("avioBufferSize"))
                    m_streamingAVIOBufferSize = buffer["avioBufferSize"].get<size_t>();
            }
        }

        // Carregar configurações de captura
        if (config.contains("capture"))
        {
            auto &capture = config["capture"];
            if (capture.contains("width"))
                m_captureWidth = capture["width"].get<uint32_t>();
            if (capture.contains("height"))
                m_captureHeight = capture["height"].get<uint32_t>();
            if (capture.contains("fps"))
                m_captureFps = capture["fps"].get<uint32_t>();
        }

        // Carregar configurações de imagem
        if (config.contains("image"))
        {
            auto &image = config["image"];
            if (image.contains("brightness"))
                m_brightness = image["brightness"];
            if (image.contains("contrast"))
                m_contrast = image["contrast"];
            if (image.contains("maintainAspect"))
                m_maintainAspect = image["maintainAspect"];
            if (image.contains("fullscreen"))
                m_fullscreen = image["fullscreen"];
            if (image.contains("monitorIndex"))
                m_monitorIndex = image["monitorIndex"];
            if (image.contains("outputWidth"))
                m_outputWidth = image["outputWidth"].get<uint32_t>();
            if (image.contains("outputHeight"))
                m_outputHeight = image["outputHeight"].get<uint32_t>();
        }

        // Carregar configurações do Web Portal
        if (config.contains("webPortal"))
        {
            auto &webPortal = config["webPortal"];
            if (webPortal.contains("enabled"))
                m_webPortalEnabled = webPortal["enabled"];
            if (webPortal.contains("httpsEnabled"))
                m_webPortalHTTPSEnabled = webPortal["httpsEnabled"];
            if (webPortal.contains("sslCertPath"))
                m_webPortalSSLCertPath = webPortal["sslCertPath"].get<std::string>();
            if (webPortal.contains("sslKeyPath"))
                m_webPortalSSLKeyPath = webPortal["sslKeyPath"].get<std::string>();
            if (webPortal.contains("title"))
                m_webPortalTitle = webPortal["title"].get<std::string>();
            if (webPortal.contains("subtitle"))
                m_webPortalSubtitle = webPortal["subtitle"].get<std::string>();
            if (webPortal.contains("imagePath"))
                m_webPortalImagePath = webPortal["imagePath"].get<std::string>();
            if (webPortal.contains("backgroundImagePath"))
                m_webPortalBackgroundImagePath = webPortal["backgroundImagePath"].get<std::string>();

            // Carregar textos editáveis
            if (webPortal.contains("texts"))
            {
                auto &texts = webPortal["texts"];
                if (texts.contains("streamInfo"))
                    m_webPortalTextStreamInfo = texts["streamInfo"].get<std::string>();
                if (texts.contains("quickActions"))
                    m_webPortalTextQuickActions = texts["quickActions"].get<std::string>();
                if (texts.contains("compatibility"))
                    m_webPortalTextCompatibility = texts["compatibility"].get<std::string>();
                if (texts.contains("status"))
                    m_webPortalTextStatus = texts["status"].get<std::string>();
                if (texts.contains("codec"))
                    m_webPortalTextCodec = texts["codec"].get<std::string>();
                if (texts.contains("resolution"))
                    m_webPortalTextResolution = texts["resolution"].get<std::string>();
                if (texts.contains("streamUrl"))
                    m_webPortalTextStreamUrl = texts["streamUrl"].get<std::string>();
                if (texts.contains("copyUrl"))
                    m_webPortalTextCopyUrl = texts["copyUrl"].get<std::string>();
                if (texts.contains("openNewTab"))
                    m_webPortalTextOpenNewTab = texts["openNewTab"].get<std::string>();
                if (texts.contains("supported"))
                    m_webPortalTextSupported = texts["supported"].get<std::string>();
                if (texts.contains("format"))
                    m_webPortalTextFormat = texts["format"].get<std::string>();
                if (texts.contains("codecInfo"))
                    m_webPortalTextCodecInfo = texts["codecInfo"].get<std::string>();
                if (texts.contains("supportedBrowsers"))
                    m_webPortalTextSupportedBrowsers = texts["supportedBrowsers"].get<std::string>();
                if (texts.contains("formatInfo"))
                    m_webPortalTextFormatInfo = texts["formatInfo"].get<std::string>();
                if (texts.contains("codecInfoValue"))
                    m_webPortalTextCodecInfoValue = texts["codecInfoValue"].get<std::string>();
                if (texts.contains("connecting"))
                    m_webPortalTextConnecting = texts["connecting"].get<std::string>();
            }

            // Carregar cores
            if (webPortal.contains("colors"))
            {
                auto &colors = webPortal["colors"];
                if (colors.contains("background"))
                {
                    auto &bg = colors["background"];
                    m_webPortalColorBackground[0] = bg[0];
                    m_webPortalColorBackground[1] = bg[1];
                    m_webPortalColorBackground[2] = bg[2];
                    m_webPortalColorBackground[3] = bg[3];
                }
                if (colors.contains("text"))
                {
                    auto &txt = colors["text"];
                    m_webPortalColorText[0] = txt[0];
                    m_webPortalColorText[1] = txt[1];
                    m_webPortalColorText[2] = txt[2];
                    m_webPortalColorText[3] = txt[3];
                }
                if (colors.contains("primary"))
                {
                    auto &prim = colors["primary"];
                    m_webPortalColorPrimary[0] = prim[0];
                    m_webPortalColorPrimary[1] = prim[1];
                    m_webPortalColorPrimary[2] = prim[2];
                    m_webPortalColorPrimary[3] = prim[3];
                }
                if (colors.contains("primaryLight"))
                {
                    auto &primLight = colors["primaryLight"];
                    m_webPortalColorPrimaryLight[0] = primLight[0];
                    m_webPortalColorPrimaryLight[1] = primLight[1];
                    m_webPortalColorPrimaryLight[2] = primLight[2];
                    m_webPortalColorPrimaryLight[3] = primLight[3];
                }
                if (colors.contains("primaryDark"))
                {
                    auto &primDark = colors["primaryDark"];
                    m_webPortalColorPrimaryDark[0] = primDark[0];
                    m_webPortalColorPrimaryDark[1] = primDark[1];
                    m_webPortalColorPrimaryDark[2] = primDark[2];
                    m_webPortalColorPrimaryDark[3] = primDark[3];
                }
                if (colors.contains("secondary"))
                {
                    auto &sec = colors["secondary"];
                    m_webPortalColorSecondary[0] = sec[0];
                    m_webPortalColorSecondary[1] = sec[1];
                    m_webPortalColorSecondary[2] = sec[2];
                    m_webPortalColorSecondary[3] = sec[3];
                }
                if (colors.contains("secondaryHighlight"))
                {
                    auto &secHighlight = colors["secondaryHighlight"];
                    m_webPortalColorSecondaryHighlight[0] = secHighlight[0];
                    m_webPortalColorSecondaryHighlight[1] = secHighlight[1];
                    m_webPortalColorSecondaryHighlight[2] = secHighlight[2];
                    m_webPortalColorSecondaryHighlight[3] = secHighlight[3];
                }
                if (colors.contains("cardHeader"))
                {
                    auto &ch = colors["cardHeader"];
                    m_webPortalColorCardHeader[0] = ch[0];
                    m_webPortalColorCardHeader[1] = ch[1];
                    m_webPortalColorCardHeader[2] = ch[2];
                    m_webPortalColorCardHeader[3] = ch[3];
                }
                if (colors.contains("border"))
                {
                    auto &b = colors["border"];
                    m_webPortalColorBorder[0] = b[0];
                    m_webPortalColorBorder[1] = b[1];
                    m_webPortalColorBorder[2] = b[2];
                    m_webPortalColorBorder[3] = b[3];
                }
                if (colors.contains("success"))
                {
                    auto &s = colors["success"];
                    m_webPortalColorSuccess[0] = s[0];
                    m_webPortalColorSuccess[1] = s[1];
                    m_webPortalColorSuccess[2] = s[2];
                    m_webPortalColorSuccess[3] = s[3];
                }
                if (colors.contains("warning"))
                {
                    auto &w = colors["warning"];
                    m_webPortalColorWarning[0] = w[0];
                    m_webPortalColorWarning[1] = w[1];
                    m_webPortalColorWarning[2] = w[2];
                    m_webPortalColorWarning[3] = w[3];
                }
                if (colors.contains("danger"))
                {
                    auto &d = colors["danger"];
                    m_webPortalColorDanger[0] = d[0];
                    m_webPortalColorDanger[1] = d[1];
                    m_webPortalColorDanger[2] = d[2];
                    m_webPortalColorDanger[3] = d[3];
                }
                if (colors.contains("info"))
                {
                    auto &inf = colors["info"];
                    m_webPortalColorInfo[0] = inf[0];
                    m_webPortalColorInfo[1] = inf[1];
                    m_webPortalColorInfo[2] = inf[2];
                    m_webPortalColorInfo[3] = inf[3];
                }
            }
        }

        // Carregar shader atual
        if (config.contains("shader"))
        {
            auto &shader = config["shader"];
            if (shader.contains("current") && !shader["current"].is_null())
            {
                m_currentShader = shader["current"].get<std::string>();
            }
        }

        // Carregar configurações de fonte
        if (config.contains("source"))
        {
            auto &source = config["source"];
            if (source.contains("type"))
            {
                int sourceTypeInt = source["type"].get<int>();
                m_sourceType = static_cast<SourceType>(sourceTypeInt);
            }
        }

        // Carregar dispositivo V4L2
        if (config.contains("v4l2"))
        {
            auto &v4l2 = config["v4l2"];
            if (v4l2.contains("device") && !v4l2["device"].is_null())
            {
                m_currentDevice = v4l2["device"].get<std::string>();
            }
        }

        // Carregar dispositivo DirectShow
        if (config.contains("directshow"))
        {
            auto &ds = config["directshow"];
            if (ds.contains("device") && !ds["device"].is_null())
            {
                m_currentDevice = ds["device"].get<std::string>();
            }
        }

        // Carregar dispositivo AVFoundation
        if (config.contains("avfoundation"))
        {
            auto &avf = config["avfoundation"];
            if (avf.contains("device") && !avf["device"].is_null())
            {
                m_currentDevice = avf["device"].get<std::string>();
                LOG_INFO("Loaded AVFoundation device from config: " + m_currentDevice);
            }
            if (avf.contains("formatId") && !avf["formatId"].is_null())
            {
                m_currentFormatId = avf["formatId"].get<std::string>();
                LOG_INFO("Loaded AVFoundation format from config: " + m_currentFormatId);
            }
            if (avf.contains("audioDeviceId") && !avf["audioDeviceId"].is_null())
            {
                m_avfoundationAudioDeviceId = avf["audioDeviceId"].get<std::string>();
                LOG_INFO("Loaded AVFoundation audio device from config: " + m_avfoundationAudioDeviceId);
            }
        }

        // Carregar configurações de áudio
        if (config.contains("audio"))
        {
            auto &audio = config["audio"];
            if (audio.contains("inputSourceId") && !audio["inputSourceId"].is_null())
            {
                m_audioInputSourceId = audio["inputSourceId"].get<std::string>();
            }
        }

        // Carregar configurações de gravação
        if (config.contains("recording"))
        {
            auto &recording = config["recording"];
            if (recording.contains("width"))
                m_recordingWidth = recording["width"].get<uint32_t>();
            if (recording.contains("height"))
                m_recordingHeight = recording["height"].get<uint32_t>();
            if (recording.contains("fps"))
                m_recordingFps = recording["fps"].get<uint32_t>();
            if (recording.contains("bitrate"))
                m_recordingBitrate = recording["bitrate"].get<uint32_t>();
            if (recording.contains("audioBitrate"))
                m_recordingAudioBitrate = recording["audioBitrate"].get<uint32_t>();
            if (recording.contains("videoCodec"))
                m_recordingVideoCodec = recording["videoCodec"].get<std::string>();
            if (recording.contains("audioCodec"))
                m_recordingAudioCodec = recording["audioCodec"].get<std::string>();
            if (recording.contains("h264Preset"))
                m_recordingH264Preset = recording["h264Preset"].get<std::string>();
            if (recording.contains("h265Preset"))
                m_recordingH265Preset = recording["h265Preset"].get<std::string>();
            if (recording.contains("h265Profile"))
                m_recordingH265Profile = recording["h265Profile"].get<std::string>();
            if (recording.contains("h265Level"))
                m_recordingH265Level = recording["h265Level"].get<std::string>();
            if (recording.contains("vp8Speed"))
                m_recordingVP8Speed = recording["vp8Speed"].get<int>();
            if (recording.contains("vp9Speed"))
                m_recordingVP9Speed = recording["vp9Speed"].get<int>();
            if (recording.contains("container"))
                m_recordingContainer = recording["container"].get<std::string>();
            if (recording.contains("outputPath"))
                m_recordingOutputPath = recording["outputPath"].get<std::string>();
            if (recording.contains("filenameTemplate"))
                m_recordingFilenameTemplate = recording["filenameTemplate"].get<std::string>();
            if (recording.contains("includeAudio"))
                m_recordingIncludeAudio = recording["includeAudio"];
        }

        LOG_INFO("Configuration loaded from: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error loading configuration: " + std::string(e.what()));
    }
}

void UIManager::saveConfig()
{
    std::string configPath = getConfigPath();

    try
    {
        nlohmann::json config;

        // Salvar configurações de streaming
        config["streaming"] = {
            {"port", m_streamingPort},
            {"width", m_streamingWidth},
            {"height", m_streamingHeight},
            {"fps", m_streamingFps},
            {"bitrate", m_streamingBitrate},
            {"audioBitrate", m_streamingAudioBitrate},
            {"videoCodec", m_streamingVideoCodec},
            {"audioCodec", m_streamingAudioCodec},
            {"h264Preset", m_streamingH264Preset},
            {"h265Preset", m_streamingH265Preset},
            {"h265Profile", m_streamingH265Profile},
            {"h265Level", m_streamingH265Level},
            {"vp8Speed", m_streamingVP8Speed},
            {"vp9Speed", m_streamingVP9Speed},
            {"buffer", {{"maxVideoBufferSize", m_streamingMaxVideoBufferSize}, {"maxAudioBufferSize", m_streamingMaxAudioBufferSize}, {"maxBufferTimeSeconds", m_streamingMaxBufferTimeSeconds}, {"avioBufferSize", m_streamingAVIOBufferSize}}}};

        // Salvar configurações de imagem
        config["image"] = {
            {"brightness", m_brightness},
            {"contrast", m_contrast},
            {"maintainAspect", m_maintainAspect},
            {"fullscreen", m_fullscreen},
            {"monitorIndex", m_monitorIndex},
            {"outputWidth", m_outputWidth},
            {"outputHeight", m_outputHeight}};

        // Salvar configurações do Web Portal
        config["webPortal"] = {
            {"enabled", m_webPortalEnabled},
            {"httpsEnabled", m_webPortalHTTPSEnabled},
            {"sslCertPath", m_webPortalSSLCertPath},
            {"sslKeyPath", m_webPortalSSLKeyPath},
            {"title", m_webPortalTitle},
            {"subtitle", m_webPortalSubtitle},
            {"imagePath", m_webPortalImagePath},
            {"backgroundImagePath", m_webPortalBackgroundImagePath},
            {"texts", {{"streamInfo", m_webPortalTextStreamInfo}, {"quickActions", m_webPortalTextQuickActions}, {"compatibility", m_webPortalTextCompatibility}, {"status", m_webPortalTextStatus}, {"codec", m_webPortalTextCodec}, {"resolution", m_webPortalTextResolution}, {"streamUrl", m_webPortalTextStreamUrl}, {"copyUrl", m_webPortalTextCopyUrl}, {"openNewTab", m_webPortalTextOpenNewTab}, {"supported", m_webPortalTextSupported}, {"format", m_webPortalTextFormat}, {"codecInfo", m_webPortalTextCodecInfo}, {"supportedBrowsers", m_webPortalTextSupportedBrowsers}, {"formatInfo", m_webPortalTextFormatInfo}, {"codecInfoValue", m_webPortalTextCodecInfoValue}, {"connecting", m_webPortalTextConnecting}}},
            {"colors", {{"background", {m_webPortalColorBackground[0], m_webPortalColorBackground[1], m_webPortalColorBackground[2], m_webPortalColorBackground[3]}}, {"text", {m_webPortalColorText[0], m_webPortalColorText[1], m_webPortalColorText[2], m_webPortalColorText[3]}}, {"primary", {m_webPortalColorPrimary[0], m_webPortalColorPrimary[1], m_webPortalColorPrimary[2], m_webPortalColorPrimary[3]}}, {"primaryLight", {m_webPortalColorPrimaryLight[0], m_webPortalColorPrimaryLight[1], m_webPortalColorPrimaryLight[2], m_webPortalColorPrimaryLight[3]}}, {"primaryDark", {m_webPortalColorPrimaryDark[0], m_webPortalColorPrimaryDark[1], m_webPortalColorPrimaryDark[2], m_webPortalColorPrimaryDark[3]}}, {"secondary", {m_webPortalColorSecondary[0], m_webPortalColorSecondary[1], m_webPortalColorSecondary[2], m_webPortalColorSecondary[3]}}, {"secondaryHighlight", {m_webPortalColorSecondaryHighlight[0], m_webPortalColorSecondaryHighlight[1], m_webPortalColorSecondaryHighlight[2], m_webPortalColorSecondaryHighlight[3]}}, {"cardHeader", {m_webPortalColorCardHeader[0], m_webPortalColorCardHeader[1], m_webPortalColorCardHeader[2], m_webPortalColorCardHeader[3]}}, {"border", {m_webPortalColorBorder[0], m_webPortalColorBorder[1], m_webPortalColorBorder[2], m_webPortalColorBorder[3]}}, {"success", {m_webPortalColorSuccess[0], m_webPortalColorSuccess[1], m_webPortalColorSuccess[2], m_webPortalColorSuccess[3]}}, {"warning", {m_webPortalColorWarning[0], m_webPortalColorWarning[1], m_webPortalColorWarning[2], m_webPortalColorWarning[3]}}, {"danger", {m_webPortalColorDanger[0], m_webPortalColorDanger[1], m_webPortalColorDanger[2], m_webPortalColorDanger[3]}}, {"info", {m_webPortalColorInfo[0], m_webPortalColorInfo[1], m_webPortalColorInfo[2], m_webPortalColorInfo[3]}}}}};

        // Salvar configurações de captura
        config["capture"] = {
            {"width", m_captureWidth},
            {"height", m_captureHeight},
            {"fps", m_captureFps}};

        // Salvar shader atual
        config["shader"] = {
            {"current", m_currentShader.empty() ? "" : m_currentShader}};

        // Salvar configurações de fonte
        config["source"] = {
            {"type", static_cast<int>(m_sourceType)}};

        // Salvar dispositivo V4L2
        config["v4l2"] = {
            {"device", m_currentDevice.empty() ? "" : m_currentDevice}};

        // Salvar dispositivo DirectShow
        config["directshow"] = {
            {"device", m_currentDevice.empty() ? "" : m_currentDevice}};

        // Salvar dispositivo AVFoundation, formato e dispositivo de áudio
        config["avfoundation"] = {
            {"device", m_currentDevice.empty() ? "" : m_currentDevice},
            {"formatId", m_currentFormatId.empty() ? "" : m_currentFormatId},
            {"audioDeviceId", m_avfoundationAudioDeviceId.empty() ? "" : m_avfoundationAudioDeviceId}};

        // Salvar configurações de áudio
        config["audio"] = {
            {"inputSourceId", m_audioInputSourceId.empty() ? "" : m_audioInputSourceId}};

        // Salvar configurações de gravação
        config["recording"] = {
            {"width", m_recordingWidth},
            {"height", m_recordingHeight},
            {"fps", m_recordingFps},
            {"bitrate", m_recordingBitrate},
            {"audioBitrate", m_recordingAudioBitrate},
            {"videoCodec", m_recordingVideoCodec},
            {"audioCodec", m_recordingAudioCodec},
            {"h264Preset", m_recordingH264Preset},
            {"h265Preset", m_recordingH265Preset},
            {"h265Profile", m_recordingH265Profile},
            {"h265Level", m_recordingH265Level},
            {"vp8Speed", m_recordingVP8Speed},
            {"vp9Speed", m_recordingVP9Speed},
            {"container", m_recordingContainer},
            {"outputPath", m_recordingOutputPath},
            {"filenameTemplate", m_recordingFilenameTemplate},
            {"includeAudio", m_recordingIncludeAudio}};

        // Escrever arquivo
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            LOG_WARN("Could not create configuration file: " + configPath);
            return;
        }

        file << config.dump(4); // Indentação de 4 espaços para legibilidade
        file.close();

        LOG_INFO("Configuration saved to: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error saving configuration: " + std::string(e.what()));
    }
}

void UIManager::renderWebPortalPanel()
{
    ImGui::Text("Web Portal");
    ImGui::Separator();
    ImGui::Spacing();

    // Web Portal Enable/Disable (configuração)
    bool portalEnabled = m_webPortalEnabled;
    if (ImGui::Checkbox("Habilitar Web Portal", &portalEnabled))
    {
        m_webPortalEnabled = portalEnabled;
        if (!portalEnabled && m_webPortalHTTPSEnabled)
        {
            m_webPortalHTTPSEnabled = false;
            if (m_onWebPortalHTTPSChanged)
            {
                m_onWebPortalHTTPSChanged(false);
            }
        }
        if (m_onWebPortalEnabledChanged)
        {
            m_onWebPortalEnabledChanged(portalEnabled);
        }
        saveConfig();
    }

    if (!portalEnabled)
    {
        ImGui::Spacing();
        std::string streamUrl = "http://localhost:" + std::to_string(m_streamingPort) + "/stream";
        ImGui::Text("Stream direto: %s", streamUrl.c_str());
        return;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Botão Start/Stop do Portal Web (independente do streaming)
    if (m_webPortalActive)
    {
        if (ImGui::Button("Parar Portal Web", ImVec2(-1, 0)))
        {
            m_webPortalActive = false;
            if (m_onWebPortalStartStop)
            {
                m_onWebPortalStartStop(false);
            }
        }
        ImGui::Spacing();
        std::string portalUrl = (m_webPortalHTTPSEnabled ? "https://" : "http://") +
                                std::string("localhost:") + std::to_string(m_streamingPort);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Portal Web Ativo");
        ImGui::Text("URL: %s", portalUrl.c_str());
    }
    else
    {
        if (ImGui::Button("Iniciar Portal Web", ImVec2(-1, 0)))
        {
            m_webPortalActive = true;
            if (m_onWebPortalStartStop)
            {
                m_onWebPortalStartStop(true);
            }
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Portal Web Inativo");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // HTTPS Enable/Disable
    bool httpsEnabled = m_webPortalHTTPSEnabled;
    if (ImGui::Checkbox("Habilitar HTTPS", &httpsEnabled))
    {
        m_webPortalHTTPSEnabled = httpsEnabled;
        if (m_onWebPortalHTTPSChanged)
        {
            m_onWebPortalHTTPSChanged(httpsEnabled);
        }
        saveConfig();
    }

    if (httpsEnabled)
    {
        ImGui::Spacing();

        if (!m_foundSSLCertPath.empty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ HTTPS Ativo");
            ImGui::Text("Certificado: %s", m_foundSSLCertPath.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Certificado não encontrado");
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Configuração de Certificado"))
        {
            char certPathBuffer[512];
            strncpy(certPathBuffer, m_webPortalSSLCertPath.c_str(), sizeof(certPathBuffer) - 1);
            certPathBuffer[sizeof(certPathBuffer) - 1] = '\0';

            ImGui::Text("Caminho do Certificado:");
            if (ImGui::InputText("##SSLCertPath", certPathBuffer, sizeof(certPathBuffer)))
            {
                m_webPortalSSLCertPath = std::string(certPathBuffer);
                if (m_onWebPortalSSLCertPathChanged)
                {
                    m_onWebPortalSSLCertPathChanged(m_webPortalSSLCertPath);
                }
                saveConfig();
            }

            char keyPathBuffer[512];
            strncpy(keyPathBuffer, m_webPortalSSLKeyPath.c_str(), sizeof(keyPathBuffer) - 1);
            keyPathBuffer[sizeof(keyPathBuffer) - 1] = '\0';

            ImGui::Text("Caminho da Chave Privada:");
            if (ImGui::InputText("##SSLKeyPath", keyPathBuffer, sizeof(keyPathBuffer)))
            {
                m_webPortalSSLKeyPath = std::string(keyPathBuffer);
                if (m_onWebPortalSSLKeyPathChanged)
                {
                    m_onWebPortalSSLKeyPathChanged(m_webPortalSSLKeyPath);
                }
                saveConfig();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Personalização
    ImGui::Text("Personalização");
    ImGui::Separator();
    ImGui::Spacing();

    // Título
    char titleBuffer[256];
    strncpy(titleBuffer, m_webPortalTitle.c_str(), sizeof(titleBuffer) - 1);
    titleBuffer[sizeof(titleBuffer) - 1] = '\0';
    ImGui::Text("Título:");
    if (ImGui::InputText("##WebPortalTitle", titleBuffer, sizeof(titleBuffer)))
    {
        m_webPortalTitle = std::string(titleBuffer);
        if (m_onWebPortalTitleChanged)
        {
            m_onWebPortalTitleChanged(m_webPortalTitle);
        }
        saveConfig();
    }

    ImGui::Spacing();

    // Subtítulo
    char subtitleBuffer[256];
    strncpy(subtitleBuffer, m_webPortalSubtitle.c_str(), sizeof(subtitleBuffer) - 1);
    subtitleBuffer[sizeof(subtitleBuffer) - 1] = '\0';
    ImGui::Text("Subtítulo:");
    if (ImGui::InputText("##WebPortalSubtitle", subtitleBuffer, sizeof(subtitleBuffer)))
    {
        m_webPortalSubtitle = std::string(subtitleBuffer);
        if (m_onWebPortalSubtitleChanged)
        {
            m_onWebPortalSubtitleChanged(m_webPortalSubtitle);
        }
        saveConfig();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Configurações avançadas (colapsável)
    if (ImGui::CollapsingHeader("Avançado"))
    {
        ImGui::Spacing();

        // Imagem de fundo
        char bgImagePathBuffer[512];
        strncpy(bgImagePathBuffer, m_webPortalBackgroundImagePath.c_str(), sizeof(bgImagePathBuffer) - 1);
        bgImagePathBuffer[sizeof(bgImagePathBuffer) - 1] = '\0';
        ImGui::Text("Imagem de Fundo:");
        if (ImGui::InputText("##WebPortalBackgroundImagePath", bgImagePathBuffer, sizeof(bgImagePathBuffer)))
        {
            m_webPortalBackgroundImagePath = std::string(bgImagePathBuffer);
            if (m_onWebPortalBackgroundImagePathChanged)
            {
                m_onWebPortalBackgroundImagePathChanged(m_webPortalBackgroundImagePath);
            }
            saveConfig();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Cores
        ImGui::Text("Cores:");
        ImGui::Spacing();

        bool colorsChanged = false;

        if (ImGui::ColorEdit4("Fundo", m_webPortalColorBackground, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Texto", m_webPortalColorText, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primária", m_webPortalColorPrimary, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primária Light", m_webPortalColorPrimaryLight, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Primária Dark", m_webPortalColorPrimaryDark, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Secundária", m_webPortalColorSecondary, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Secundária Highlight", m_webPortalColorSecondaryHighlight, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Cabeçalho", m_webPortalColorCardHeader, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Bordas", m_webPortalColorBorder, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        ImGui::Spacing();

        if (ImGui::ColorEdit4("Sucesso", m_webPortalColorSuccess, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Aviso", m_webPortalColorWarning, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Erro", m_webPortalColorDanger, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (ImGui::ColorEdit4("Info", m_webPortalColorInfo, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (colorsChanged)
        {
            if (m_onWebPortalColorsChanged)
            {
                m_onWebPortalColorsChanged();
            }
            saveConfig();
        }

        ImGui::Spacing();
        if (ImGui::Button("Restaurar Cores Padrão"))
        {
            // Restaurar valores padrão do styleguide RetroCapture
            // Dark Background #1D1F21
            m_webPortalColorBackground[0] = 0.114f;
            m_webPortalColorBackground[1] = 0.122f;
            m_webPortalColorBackground[2] = 0.129f;
            m_webPortalColorBackground[3] = 1.0f;
            // Text Light #F8F8F2
            m_webPortalColorText[0] = 0.973f;
            m_webPortalColorText[1] = 0.973f;
            m_webPortalColorText[2] = 0.949f;
            m_webPortalColorText[3] = 1.0f;
            // Primary - Retro Teal #0A7A83
            m_webPortalColorPrimary[0] = 0.039f;
            m_webPortalColorPrimary[1] = 0.478f;
            m_webPortalColorPrimary[2] = 0.514f;
            m_webPortalColorPrimary[3] = 1.0f;
            // Primary Light - Mint Screen Glow #6FC4C0
            m_webPortalColorPrimaryLight[0] = 0.435f;
            m_webPortalColorPrimaryLight[1] = 0.769f;
            m_webPortalColorPrimaryLight[2] = 0.753f;
            m_webPortalColorPrimaryLight[3] = 1.0f;
            // Primary Dark - Deep Retro #0F3E42
            m_webPortalColorPrimaryDark[0] = 0.059f;
            m_webPortalColorPrimaryDark[1] = 0.243f;
            m_webPortalColorPrimaryDark[2] = 0.259f;
            m_webPortalColorPrimaryDark[3] = 1.0f;
            // Secondary - Cyan Oscilloscope #47B3CE
            m_webPortalColorSecondary[0] = 0.278f;
            m_webPortalColorSecondary[1] = 0.702f;
            m_webPortalColorSecondary[2] = 0.808f;
            m_webPortalColorSecondary[3] = 1.0f;
            // Secondary Highlight - Phosphor Glow #C9F2E7
            m_webPortalColorSecondaryHighlight[0] = 0.788f;
            m_webPortalColorSecondaryHighlight[1] = 0.949f;
            m_webPortalColorSecondaryHighlight[2] = 0.906f;
            m_webPortalColorSecondaryHighlight[3] = 1.0f;
            // Card Header (usa Primary Dark)
            m_webPortalColorCardHeader[0] = 0.059f;
            m_webPortalColorCardHeader[1] = 0.243f;
            m_webPortalColorCardHeader[2] = 0.259f;
            m_webPortalColorCardHeader[3] = 1.0f;
            // Border (usa Primary com transparência)
            m_webPortalColorBorder[0] = 0.039f;
            m_webPortalColorBorder[1] = 0.478f;
            m_webPortalColorBorder[2] = 0.514f;
            m_webPortalColorBorder[3] = 0.5f;
            // Success #45D6A4
            m_webPortalColorSuccess[0] = 0.271f;
            m_webPortalColorSuccess[1] = 0.839f;
            m_webPortalColorSuccess[2] = 0.643f;
            m_webPortalColorSuccess[3] = 1.0f;
            // Warning #F3C93E
            m_webPortalColorWarning[0] = 0.953f;
            m_webPortalColorWarning[1] = 0.788f;
            m_webPortalColorWarning[2] = 0.243f;
            m_webPortalColorWarning[3] = 1.0f;
            // Error #D9534F
            m_webPortalColorDanger[0] = 0.851f;
            m_webPortalColorDanger[1] = 0.325f;
            m_webPortalColorDanger[2] = 0.310f;
            m_webPortalColorDanger[3] = 1.0f;
            // Info #4CBCE6
            m_webPortalColorInfo[0] = 0.298f;
            m_webPortalColorInfo[1] = 0.737f;
            m_webPortalColorInfo[2] = 0.902f;
            m_webPortalColorInfo[3] = 1.0f;

            if (m_onWebPortalColorsChanged)
            {
                m_onWebPortalColorsChanged();
            }
            saveConfig();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Portal URL
    std::string protocol = httpsEnabled ? "https" : "http";
    std::string portalUrl = protocol + "://localhost:" + std::to_string(m_streamingPort);
    ImGui::Text("URL: %s", portalUrl.c_str());
}

// Recording trigger methods
void UIManager::triggerRecordingWidthChange(uint32_t width)
{
    m_recordingWidth = width;
    if (m_onRecordingWidthChanged)
    {
        m_onRecordingWidthChanged(width);
    }
    saveConfig();
}

void UIManager::triggerRecordingHeightChange(uint32_t height)
{
    m_recordingHeight = height;
    if (m_onRecordingHeightChanged)
    {
        m_onRecordingHeightChanged(height);
    }
    saveConfig();
}

void UIManager::triggerRecordingFpsChange(uint32_t fps)
{
    m_recordingFps = fps;
    if (m_onRecordingFpsChanged)
    {
        m_onRecordingFpsChanged(fps);
    }
    saveConfig();
}

void UIManager::triggerRecordingBitrateChange(uint32_t bitrate)
{
    m_recordingBitrate = bitrate;
    if (m_onRecordingBitrateChanged)
    {
        m_onRecordingBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerRecordingAudioBitrateChange(uint32_t bitrate)
{
    m_recordingAudioBitrate = bitrate;
    if (m_onRecordingAudioBitrateChanged)
    {
        m_onRecordingAudioBitrateChanged(bitrate);
    }
    saveConfig();
}

void UIManager::triggerRecordingVideoCodecChange(const std::string& codec)
{
    m_recordingVideoCodec = codec;
    if (m_onRecordingVideoCodecChanged)
    {
        m_onRecordingVideoCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerRecordingAudioCodecChange(const std::string& codec)
{
    m_recordingAudioCodec = codec;
    if (m_onRecordingAudioCodecChanged)
    {
        m_onRecordingAudioCodecChanged(codec);
    }
    saveConfig();
}

void UIManager::triggerRecordingH264PresetChange(const std::string& preset)
{
    m_recordingH264Preset = preset;
    if (m_onRecordingH264PresetChanged)
    {
        m_onRecordingH264PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265PresetChange(const std::string& preset)
{
    m_recordingH265Preset = preset;
    if (m_onRecordingH265PresetChanged)
    {
        m_onRecordingH265PresetChanged(preset);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265ProfileChange(const std::string& profile)
{
    m_recordingH265Profile = profile;
    if (m_onRecordingH265ProfileChanged)
    {
        m_onRecordingH265ProfileChanged(profile);
    }
    saveConfig();
}

void UIManager::triggerRecordingH265LevelChange(const std::string& level)
{
    m_recordingH265Level = level;
    if (m_onRecordingH265LevelChanged)
    {
        m_onRecordingH265LevelChanged(level);
    }
    saveConfig();
}

void UIManager::triggerRecordingVP8SpeedChange(int speed)
{
    m_recordingVP8Speed = speed;
    if (m_onRecordingVP8SpeedChanged)
    {
        m_onRecordingVP8SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerRecordingVP9SpeedChange(int speed)
{
    m_recordingVP9Speed = speed;
    if (m_onRecordingVP9SpeedChanged)
    {
        m_onRecordingVP9SpeedChanged(speed);
    }
    saveConfig();
}

void UIManager::triggerRecordingContainerChange(const std::string& container)
{
    m_recordingContainer = container;
    if (m_onRecordingContainerChanged)
    {
        m_onRecordingContainerChanged(container);
    }
    saveConfig();
}

void UIManager::triggerRecordingOutputPathChange(const std::string& path)
{
    m_recordingOutputPath = path;
    if (m_onRecordingOutputPathChanged)
    {
        m_onRecordingOutputPathChanged(path);
    }
    saveConfig();
}

void UIManager::triggerRecordingFilenameTemplateChange(const std::string& template_)
{
    m_recordingFilenameTemplate = template_;
    if (m_onRecordingFilenameTemplateChanged)
    {
        m_onRecordingFilenameTemplateChanged(template_);
    }
    saveConfig();
}

void UIManager::triggerRecordingIncludeAudioChange(bool include)
{
    m_recordingIncludeAudio = include;
    if (m_onRecordingIncludeAudioChanged)
    {
        m_onRecordingIncludeAudioChanged(include);
    }
    saveConfig();
}

void UIManager::triggerRecordingStartStop(bool start)
{
    if (m_onRecordingStartStop)
    {
        m_onRecordingStartStop(start);
    }
}
