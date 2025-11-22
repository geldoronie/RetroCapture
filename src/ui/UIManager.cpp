#include "UIManager.h"
#include "../utils/Logger.h"
#include "../utils/ShaderScanner.h"
#include "../utils/V4L2DeviceScanner.h"
#include "../capture/VideoCapture.h"
#include "../shader/ShaderEngine.h"
#include "../renderer/glad_loader.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

UIManager::UIManager()
{
}

UIManager::~UIManager()
{
    shutdown();
}

bool UIManager::init(GLFWwindow *window)
{
    if (m_initialized)
    {
        return true;
    }

    m_window = window;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Configurar nome do arquivo de configuração para usar o nome da aplicação
    io.IniFilename = "RetroCapture.ini";
    
    // Remover apenas o arquivo de configuração antigo (imgui.ini) se existir
    // O RetroCapture.ini pode ser criado normalmente
    std::string oldIniPath = "imgui.ini";
    if (std::filesystem::exists(oldIniPath))
    {
        std::filesystem::remove(oldIniPath);
        LOG_INFO("Arquivo de configuração antigo removido: " + oldIniPath);
    }

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Scan for shaders
    // Verificar se há variável de ambiente para o caminho dos shaders (útil para AppImage)
    const char *envShaderPath = std::getenv("RETROCAPTURE_SHADER_PATH");
    if (envShaderPath && std::filesystem::exists(envShaderPath))
    {
        m_shaderBasePath = envShaderPath;
    }
    scanShaders(m_shaderBasePath);

    // Carregar configurações salvas
    loadConfig();

    m_initialized = true;
    LOG_INFO("UIManager inicializado");
    return true;
}

void UIManager::shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    // Remover apenas o arquivo antigo (imgui.ini) se ainda existir
    // O RetroCapture.ini pode ser mantido
    std::string oldIniPath = "imgui.ini";
    if (std::filesystem::exists(oldIniPath))
    {
        std::filesystem::remove(oldIniPath);
        LOG_INFO("Arquivo de configuração antigo removido no shutdown: " + oldIniPath);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    m_initialized = false;
}

void UIManager::beginFrame()
{
    if (!m_initialized)
    {
        return;
    }

    // IMPORTANTE: Sempre chamar NewFrame, mesmo quando UI está oculta
    // Isso mantém o estado do ImGui correto e permite toggle funcionar
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void UIManager::endFrame()
{
    if (!m_initialized)
    {
        return;
    }

    // Renderizar apenas se a UI estiver visível
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
}

void UIManager::render()
{
    if (!m_initialized || !m_uiVisible)
    {
        return;
    }

    // Main menu bar fixo no topo
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
                    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Toggle UI", "F12"))
            {
                m_uiVisible = !m_uiVisible;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Configuration", nullptr, m_configWindowVisible))
            {
                m_configWindowVisible = !m_configWindowVisible;
                // Quando a janela é aberta, marcar para aplicar posição/tamanho inicial
                if (m_configWindowVisible)
                {
                    m_configWindowJustOpened = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Renderizar janela de configuração apenas se estiver visível
    if (m_configWindowVisible)
    {
        // Aplicar posição e tamanho inicial apenas quando a janela é aberta
        if (m_configWindowJustOpened)
        {
            // Obter altura do menu bar para posicionar a janela abaixo dele
            float menuBarHeight = ImGui::GetFrameHeight();
            
            // Obter dimensões da viewport
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 workPos = viewport->WorkPos;
            
            // Definir posição inicial: um pouco abaixo do menu bar
            ImVec2 initialPos(workPos.x + 10.0f, workPos.y + menuBarHeight + 10.0f);
            
            // Definir tamanho inicial menor que 640x480 (usar 600x400 para caber em resoluções menores)
            ImVec2 initialSize(600.0f, 400.0f);
            
            // Configurar posição e tamanho inicial (ignora o que está salvo no .ini)
            ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
            
            m_configWindowJustOpened = false;
        }
        
        // Janela flutuante redimensionável
        // Usar ImGuiWindowFlags_NoSavedSettings para não salvar posição/tamanho no .ini
        ImGui::Begin("RetroCapture Controls", &m_configWindowVisible,
                     ImGuiWindowFlags_NoSavedSettings);

        // Tabs
        if (ImGui::BeginTabBar("MainTabs"))
        {
            if (ImGui::BeginTabItem("Shaders"))
            {
                renderShaderPanel();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Image"))
            {
                renderImageControls();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("V4L2"))
            {
                renderV4L2Controls();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Info"))
            {
                renderInfoPanel();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Streaming"))
            {
                renderStreamingPanel();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        
        ImGui::End();
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
            saveConfig(); // Salvar configuração quando mudar
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
                saveConfig(); // Salvar configuração quando mudar
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
            std::filesystem::path presetPath(currentPreset);
            std::string fileName = presetPath.filename().string();

            if (ImGui::Button("Save"))
            {
                // Salvar por cima do arquivo atual
                if (m_onSavePreset)
                {
                    m_onSavePreset(currentPreset, true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As..."))
            {
                // Abrir dialog para salvar como novo arquivo
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
                    std::filesystem::path basePath("shaders/shaders_glsl");
                    std::filesystem::path newPath = basePath / m_savePresetPath;
                    // Garantir extensão .glslp
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

    // Parâmetros do shader
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

                // Mostrar nome e descrição
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

                // Botão para resetar ao valor padrão
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

void UIManager::renderV4L2Controls()
{
    if (!m_capture)
    {
        ImGui::Text("V4L2 controls not available");
        return;
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
    int selectedIndex = -1;
    for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
    {
        if (m_v4l2Devices[i] == m_currentDevice)
        {
            selectedIndex = static_cast<int>(i);
            break;
        }
    }

    if (ImGui::BeginCombo("##device", m_currentDevice.empty() ? "Select device" : m_currentDevice.c_str()))
    {
        for (size_t i = 0; i < m_v4l2Devices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i));
            if (ImGui::Selectable(m_v4l2Devices[i].c_str(), isSelected))
            {
                m_currentDevice = m_v4l2Devices[i];
                if (m_onDeviceChanged)
                {
                    m_onDeviceChanged(m_v4l2Devices[i]);
                }
                saveConfig(); // Salvar configuração quando mudar
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
    }
    ImGui::SameLine();
    if (ImGui::Button("60"))
    {
        m_captureFps = 60;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(60);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("120"))
    {
        m_captureFps = 120;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(120);
        }
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
    }
    if (ImGui::Button("1024x768"))
    {
        m_captureWidth = 1024;
        m_captureHeight = 768;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(1024, 768);
        }
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
    }
    if (ImGui::Button("2048x1536"))
    {
        m_captureWidth = 2048;
        m_captureHeight = 1536;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(2048, 1536);
        }
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
    }
    if (ImGui::Button("3840x2160"))
    {
        m_captureWidth = 3840;
        m_captureHeight = 2160;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(3840, 2160);
        }
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
    auto renderControl = [this](const char *name, uint32_t cid, int32_t defaultMin, int32_t defaultMax, int32_t defaultValue)
    {
        if (!m_capture)
            return;

        int32_t value, min, max, step;
        bool available = m_capture->getControl(cid, value, min, max, step);

        // Se não disponível, usar valores padrão
        if (!available)
        {
            min = defaultMin;
            max = defaultMax;
            value = defaultValue;
            step = 1;
        }

        // Alinhar valor com step
        if (step > 1)
        {
            value = ((value - min) / step) * step + min;
        }

        // Clamp valor
        value = std::max(min, std::min(max, value));

        // Use unique ID with suffix to avoid conflicts with dynamic controls
        std::string label = std::string(name) + "##manual";
        if (ImGui::SliderInt(label.c_str(), &value, min, max))
        {
            // Alinhar valor com step antes de aplicar
            if (step > 1)
            {
                value = ((value - min) / step) * step + min;
            }
            value = std::max(min, std::min(max, value));

            if (m_onV4L2ControlChanged)
            {
                m_onV4L2ControlChanged(name, value);
            }
        }
    };

    // Brightness
    renderControl("Brightness", V4L2_CID_BRIGHTNESS, -100, 100, 0);

    // Contrast
    renderControl("Contrast", V4L2_CID_CONTRAST, -100, 100, 0);

    // Saturation
    renderControl("Saturation", V4L2_CID_SATURATION, -100, 100, 0);

    // Hue
    renderControl("Hue", V4L2_CID_HUE, -100, 100, 0);

    // Gain
    renderControl("Gain", V4L2_CID_GAIN, 0, 100, 0);

    // Exposure
    renderControl("Exposure", V4L2_CID_EXPOSURE_ABSOLUTE, -13, 1, 0);

    // Sharpness
    renderControl("Sharpness", V4L2_CID_SHARPNESS, 0, 6, 0);

    // Gamma
    renderControl("Gamma", V4L2_CID_GAMMA, 100, 300, 100);

    // White Balance
    renderControl("White Balance", V4L2_CID_WHITE_BALANCE_TEMPERATURE, 2800, 6500, 4000);
}

void UIManager::setV4L2Controls(VideoCapture *capture)
{
    m_capture = capture;
    m_v4l2Controls.clear();

    if (!capture)
    {
        return;
    }

    // Lista de controles V4L2 comuns
    struct ControlInfo
    {
        const char *name;
        uint32_t cid;
    };

    ControlInfo controls[] = {
        {"Brightness", V4L2_CID_BRIGHTNESS},
        {"Contrast", V4L2_CID_CONTRAST},
        {"Saturation", V4L2_CID_SATURATION},
        {"Hue", V4L2_CID_HUE},
        {"Gain", V4L2_CID_GAIN},
        {"Exposure", V4L2_CID_EXPOSURE_ABSOLUTE},
        {"Sharpness", V4L2_CID_SHARPNESS},
        {"Gamma", V4L2_CID_GAMMA},
        {"White Balance", V4L2_CID_WHITE_BALANCE_TEMPERATURE},
    };

    for (const auto &info : controls)
    {
        V4L2Control ctrl;
        ctrl.name = info.name;
        ctrl.available = capture->getControl(info.cid, ctrl.value, ctrl.min, ctrl.max, ctrl.step);

        if (ctrl.available)
        {
            m_v4l2Controls.push_back(ctrl);
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
    if (m_streamingActive) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
    } else {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }
    
    if (m_streamingActive && !m_streamUrl.empty()) {
        ImGui::Text("URL: %s", m_streamUrl.c_str());
        ImGui::Text("Clientes conectados: %u", m_streamClientCount);
    }
    
    ImGui::Separator();
    ImGui::Text("Configurações Básicas");
    ImGui::Separator();
    
    // Controles básicos
    int port = static_cast<int>(m_streamingPort);
    if (ImGui::InputInt("Porta", &port, 1, 100)) {
        if (port >= 1024 && port <= 65535) {
            m_streamingPort = static_cast<uint16_t>(port);
            if (m_onStreamingPortChanged) {
                m_onStreamingPortChanged(m_streamingPort);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }
    
    // Resolução - Dropdown
    const char* resolutions[] = { 
        "Captura (0x0)", 
        "320x240", 
        "640x480", 
        "800x600", 
        "1024x768", 
        "1280x720 (HD)", 
        "1280x1024", 
        "1920x1080 (Full HD)", 
        "2560x1440 (2K)", 
        "3840x2160 (4K)" 
    };
    const uint32_t resolutionWidths[] = { 0, 320, 640, 800, 1024, 1280, 1280, 1920, 2560, 3840 };
    const uint32_t resolutionHeights[] = { 0, 240, 480, 600, 768, 720, 1024, 1080, 1440, 2160 };
    
    int currentResIndex = 0;
    for (int i = 0; i < 10; i++) {
        if (m_streamingWidth == resolutionWidths[i] && m_streamingHeight == resolutionHeights[i]) {
            currentResIndex = i;
            break;
        }
    }
    
    if (ImGui::Combo("Resolução", &currentResIndex, resolutions, 10)) {
        m_streamingWidth = resolutionWidths[currentResIndex];
        m_streamingHeight = resolutionHeights[currentResIndex];
        if (m_onStreamingWidthChanged) {
            m_onStreamingWidthChanged(m_streamingWidth);
        }
        if (m_onStreamingHeightChanged) {
            m_onStreamingHeightChanged(m_streamingHeight);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    
    // FPS - Dropdown
    const char* fpsOptions[] = { "Captura (0)", "15", "24", "30", "60", "120" };
    const uint32_t fpsValues[] = { 0, 15, 24, 30, 60, 120 };
    
    int currentFpsIndex = 0;
    for (int i = 0; i < 6; i++) {
        if (m_streamingFps == fpsValues[i]) {
            currentFpsIndex = i;
            break;
        }
    }
    
    if (ImGui::Combo("FPS", &currentFpsIndex, fpsOptions, 6)) {
        m_streamingFps = fpsValues[currentFpsIndex];
        if (m_onStreamingFpsChanged) {
            m_onStreamingFpsChanged(m_streamingFps);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    
    ImGui::Separator();
    ImGui::Text("Codecs");
    ImGui::Separator();
    
    // Seleção de codec de vídeo
    const char* videoCodecs[] = { "h264", "h265", "vp8", "vp9" };
    int currentVideoCodecIndex = 0;
    for (int i = 0; i < 4; i++) {
        if (m_streamingVideoCodec == videoCodecs[i]) {
            currentVideoCodecIndex = i;
            break;
        }
    }
    
    if (ImGui::Combo("Codec de Vídeo", &currentVideoCodecIndex, videoCodecs, 4)) {
        m_streamingVideoCodec = videoCodecs[currentVideoCodecIndex];
        if (m_onStreamingVideoCodecChanged) {
            m_onStreamingVideoCodecChanged(m_streamingVideoCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    
    // Seleção de codec de áudio
    const char* audioCodecs[] = { "aac", "mp3", "opus" };
    int currentAudioCodecIndex = 0;
    for (int i = 0; i < 3; i++) {
        if (m_streamingAudioCodec == audioCodecs[i]) {
            currentAudioCodecIndex = i;
            break;
        }
    }
    
    if (ImGui::Combo("Codec de Áudio", &currentAudioCodecIndex, audioCodecs, 3)) {
        m_streamingAudioCodec = audioCodecs[currentAudioCodecIndex];
        if (m_onStreamingAudioCodecChanged) {
            m_onStreamingAudioCodecChanged(m_streamingAudioCodec);
        }
        saveConfig(); // Salvar configuração quando mudar
    }
    
    // Qualidade H.264 (apenas se codec for h264)
    if (m_streamingVideoCodec == "h264") {
        const char* h264Presets[] = { 
            "ultrafast", 
            "superfast", 
            "veryfast", 
            "faster", 
            "fast", 
            "medium", 
            "slow", 
            "slower", 
            "veryslow" 
        };
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++) {
            if (m_streamingH264Preset == h264Presets[i]) {
                currentPresetIndex = i;
                break;
            }
        }
        
        if (ImGui::Combo("Qualidade H.264", &currentPresetIndex, h264Presets, 9)) {
            m_streamingH264Preset = h264Presets[currentPresetIndex];
            if (m_onStreamingH264PresetChanged) {
                m_onStreamingH264PresetChanged(m_streamingH264Preset);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Preset do encoder H.264:\n"
                              "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                              "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                              "slow/slower/veryslow: Máxima qualidade, menor velocidade");
        }
    }
    
    // Qualidade H.265 (apenas se codec for h265)
    if (m_streamingVideoCodec == "h265" || m_streamingVideoCodec == "hevc") {
        const char* h265Presets[] = { 
            "ultrafast", 
            "superfast", 
            "veryfast", 
            "faster", 
            "fast", 
            "medium", 
            "slow", 
            "slower", 
            "veryslow" 
        };
        int currentPresetIndex = 2; // Padrão: veryfast
        for (int i = 0; i < 9; i++) {
            if (m_streamingH265Preset == h265Presets[i]) {
                currentPresetIndex = i;
                break;
            }
        }
        
        if (ImGui::Combo("Qualidade H.265", &currentPresetIndex, h265Presets, 9)) {
            m_streamingH265Preset = h265Presets[currentPresetIndex];
            if (m_onStreamingH265PresetChanged) {
                m_onStreamingH265PresetChanged(m_streamingH265Preset);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Preset do encoder H.265:\n"
                              "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                              "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                              "slow/slower/veryslow: Máxima qualidade, menor velocidade");
        }
        
        // Profile H.265
        const char* h265Profiles[] = { "main", "main10" };
        int currentProfileIndex = 0;
        for (int i = 0; i < 2; i++) {
            if (m_streamingH265Profile == h265Profiles[i]) {
                currentProfileIndex = i;
                break;
            }
        }
        
        if (ImGui::Combo("Profile H.265", &currentProfileIndex, h265Profiles, 2)) {
            m_streamingH265Profile = h265Profiles[currentProfileIndex];
            if (m_onStreamingH265ProfileChanged) {
                m_onStreamingH265ProfileChanged(m_streamingH265Profile);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Profile do encoder H.265:\n"
                              "main: 8-bit, máxima compatibilidade\n"
                              "main10: 10-bit, melhor qualidade, suporte HDR");
        }
        
        // Level H.265
        const char* h265Levels[] = { 
            "auto", "1", "2", "2.1", "3", "3.1", 
            "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2" 
        };
        int currentLevelIndex = 0;
        for (int i = 0; i < 14; i++) {
            if (m_streamingH265Level == h265Levels[i]) {
                currentLevelIndex = i;
                break;
            }
        }
        
        if (ImGui::Combo("Level H.265", &currentLevelIndex, h265Levels, 14)) {
            m_streamingH265Level = h265Levels[currentLevelIndex];
            if (m_onStreamingH265LevelChanged) {
                m_onStreamingH265LevelChanged(m_streamingH265Level);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Level do encoder H.265:\n"
                              "auto: Detecção automática (recomendado)\n"
                              "1-6.2: Níveis específicos para compatibilidade\n"
                              "Níveis mais altos suportam resoluções/bitrates maiores");
        }
    }
    
    // Configurações VP8 (apenas se codec for vp8)
    if (m_streamingVideoCodec == "vp8") {
        int currentSpeed = m_streamingVP8Speed;
        if (ImGui::SliderInt("Speed VP8 (0-16)", &currentSpeed, 0, 16)) {
            m_streamingVP8Speed = currentSpeed;
            if (m_onStreamingVP8SpeedChanged) {
                m_onStreamingVP8SpeedChanged(m_streamingVP8Speed);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Speed do encoder VP8:\n"
                              "0: Melhor qualidade, mais lento\n"
                              "16: Mais rápido, menor qualidade\n"
                              "12: Bom equilíbrio para streaming");
        }
    }
    
    // Configurações VP9 (apenas se codec for vp9)
    if (m_streamingVideoCodec == "vp9") {
        int currentSpeed = m_streamingVP9Speed;
        if (ImGui::SliderInt("Speed VP9 (0-9)", &currentSpeed, 0, 9)) {
            m_streamingVP9Speed = currentSpeed;
            if (m_onStreamingVP9SpeedChanged) {
                m_onStreamingVP9SpeedChanged(m_streamingVP9Speed);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
        if (ImGui::IsItemHovered()) {
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
    if (ImGui::InputInt("Bitrate Vídeo (kbps, 0 = auto)", &bitrate, 100, 1000)) {
        if (bitrate >= 0 && bitrate <= 50000) {
            m_streamingBitrate = static_cast<uint32_t>(bitrate);
            if (m_onStreamingBitrateChanged) {
                m_onStreamingBitrateChanged(m_streamingBitrate);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }
    
    // Bitrate de áudio
    int audioBitrate = static_cast<int>(m_streamingAudioBitrate);
    if (ImGui::InputInt("Bitrate Áudio (kbps)", &audioBitrate, 8, 32)) {
        if (audioBitrate >= 32 && audioBitrate <= 320) {
            m_streamingAudioBitrate = static_cast<uint32_t>(audioBitrate);
            if (m_onStreamingAudioBitrateChanged) {
                m_onStreamingAudioBitrateChanged(m_streamingAudioBitrate);
            }
            saveConfig(); // Salvar configuração quando mudar
        }
    }
    
    ImGui::Separator();
    
    // Botão Start/Stop
    if (m_streamingActive) {
        if (ImGui::Button("Parar Streaming", ImVec2(-1, 0))) {
            if (m_onStreamingStartStop) {
                m_onStreamingStartStop(false);
            }
        }
    } else {
        if (ImGui::Button("Iniciar Streaming", ImVec2(-1, 0))) {
            if (m_onStreamingStartStop) {
                m_onStreamingStartStop(true);
            }
        }
    }
}

void UIManager::scanV4L2Devices()
{
    m_v4l2Devices = V4L2DeviceScanner::scan();
}

void UIManager::refreshV4L2Devices()
{
    scanV4L2Devices();
}

void UIManager::scanShaders(const std::string &basePath)
{
    m_scannedShaders = ShaderScanner::scan(basePath);
    LOG_INFO("Encontrados " + std::to_string(m_scannedShaders.size()) + " shaders em " + basePath);
}

std::string UIManager::getConfigPath() const
{
    // Usar diretório home do usuário para salvar configurações
    const char *homeDir = std::getenv("HOME");
    if (homeDir)
    {
        std::filesystem::path configDir = std::filesystem::path(homeDir) / ".config" / "retrocapture";
        // Criar diretório se não existir
        if (!std::filesystem::exists(configDir))
        {
            std::filesystem::create_directories(configDir);
        }
        return (configDir / "config.json").string();
    }
    // Fallback: salvar no diretório atual
    return "retrocapture_config.json";
}

void UIManager::loadConfig()
{
    std::string configPath = getConfigPath();
    
    if (!std::filesystem::exists(configPath))
    {
        LOG_INFO("Arquivo de configuração não encontrado: " + configPath + " (usando padrões)");
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
            if (streaming.contains("port")) m_streamingPort = streaming["port"];
            if (streaming.contains("width")) m_streamingWidth = streaming["width"];
            if (streaming.contains("height")) m_streamingHeight = streaming["height"];
            if (streaming.contains("fps")) m_streamingFps = streaming["fps"];
            if (streaming.contains("bitrate")) m_streamingBitrate = streaming["bitrate"];
            if (streaming.contains("audioBitrate")) m_streamingAudioBitrate = streaming["audioBitrate"];
            if (streaming.contains("videoCodec")) m_streamingVideoCodec = streaming["videoCodec"].get<std::string>();
            if (streaming.contains("audioCodec")) m_streamingAudioCodec = streaming["audioCodec"].get<std::string>();
            if (streaming.contains("h264Preset")) m_streamingH264Preset = streaming["h264Preset"].get<std::string>();
            if (streaming.contains("h265Preset")) m_streamingH265Preset = streaming["h265Preset"].get<std::string>();
            if (streaming.contains("h265Profile")) m_streamingH265Profile = streaming["h265Profile"].get<std::string>();
            if (streaming.contains("h265Level")) m_streamingH265Level = streaming["h265Level"].get<std::string>();
            if (streaming.contains("vp8Speed")) m_streamingVP8Speed = streaming["vp8Speed"].get<int>();
            if (streaming.contains("vp9Speed")) m_streamingVP9Speed = streaming["vp9Speed"].get<int>();
        }

        // Carregar configurações de imagem
        if (config.contains("image"))
        {
            auto &image = config["image"];
            if (image.contains("brightness")) m_brightness = image["brightness"];
            if (image.contains("contrast")) m_contrast = image["contrast"];
            if (image.contains("maintainAspect")) m_maintainAspect = image["maintainAspect"];
            if (image.contains("fullscreen")) m_fullscreen = image["fullscreen"];
            if (image.contains("monitorIndex")) m_monitorIndex = image["monitorIndex"];
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

        // Carregar dispositivo V4L2
        if (config.contains("v4l2"))
        {
            auto &v4l2 = config["v4l2"];
            if (v4l2.contains("device") && !v4l2["device"].is_null())
            {
                m_currentDevice = v4l2["device"].get<std::string>();
            }
        }

        LOG_INFO("Configurações carregadas de: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Erro ao carregar configurações: " + std::string(e.what()));
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
            {"vp9Speed", m_streamingVP9Speed}
        };

        // Salvar configurações de imagem
        config["image"] = {
            {"brightness", m_brightness},
            {"contrast", m_contrast},
            {"maintainAspect", m_maintainAspect},
            {"fullscreen", m_fullscreen},
            {"monitorIndex", m_monitorIndex}
        };

        // Salvar shader atual
        config["shader"] = {
            {"current", m_currentShader.empty() ? nullptr : m_currentShader}
        };

        // Salvar dispositivo V4L2
        config["v4l2"] = {
            {"device", m_currentDevice.empty() ? nullptr : m_currentDevice}
        };

        // Escrever arquivo
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            LOG_WARN("Não foi possível criar arquivo de configuração: " + configPath);
            return;
        }

        file << config.dump(4); // Indentação de 4 espaços para legibilidade
        file.close();

        LOG_INFO("Configurações salvas em: " + configPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Erro ao salvar configurações: " + std::string(e.what()));
    }
}
