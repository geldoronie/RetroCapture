#include "UIManager.h"
#include "../utils/Logger.h"
#include "../capture/VideoCapture.h"
#include "../shader/ShaderEngine.h"
#include "../renderer/glad_loader.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <linux/videodev2.h>
#include <filesystem>
#include <algorithm>
#include <cstring>

UIManager::UIManager() {
}

UIManager::~UIManager() {
    shutdown();
}

bool UIManager::init(GLFWwindow* window) {
    if (m_initialized) {
        return true;
    }
    
    m_window = window;
    
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    
    // Scan for shaders
    scanShaders(m_shaderBasePath);
    
    m_initialized = true;
    LOG_INFO("UIManager inicializado");
    return true;
}

void UIManager::shutdown() {
    if (!m_initialized) {
        return;
    }
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    m_initialized = false;
}

void UIManager::beginFrame() {
    if (!m_initialized) {
        return;
    }
    
    // IMPORTANTE: Sempre chamar NewFrame, mesmo quando UI está oculta
    // Isso mantém o estado do ImGui correto e permite toggle funcionar
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void UIManager::endFrame() {
    if (!m_initialized) {
        return;
    }
    
    // Renderizar apenas se a UI estiver visível
    if (m_uiVisible) {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    } else {
        // Quando oculta, ainda precisamos finalizar o frame para manter o estado correto
        ImGui::EndFrame();
    }
}

void UIManager::render() {
    if (!m_initialized || !m_uiVisible) {
        return;
    }
    
    // Main window
    ImGui::Begin("RetroCapture Controls", &m_uiVisible, 
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_MenuBar);
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Rescan Shaders")) {
                scanShaders(m_shaderBasePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Esc")) {
                if (m_window) {
                    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Toggle UI", "F12")) {
                m_uiVisible = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // Tabs
    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Shaders")) {
            renderShaderPanel();
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Image")) {
            renderImageControls();
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("V4L2")) {
            renderV4L2Controls();
            ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Info")) {
            renderInfoPanel();
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::End();
}

void UIManager::renderShaderPanel() {
    ImGui::Text("Shader Preset:");
    
    // Combo box for shader selection
    if (ImGui::BeginCombo("##shader", m_currentShader.empty() ? "None" : m_currentShader.c_str())) {
        if (ImGui::Selectable("None", m_currentShader.empty())) {
            m_currentShader = "";
            if (m_onShaderChanged) {
                m_onShaderChanged("");
            }
        }
        
        for (size_t i = 0; i < m_scannedShaders.size(); ++i) {
            bool isSelected = (m_currentShader == m_scannedShaders[i]);
            if (ImGui::Selectable(m_scannedShaders[i].c_str(), isSelected)) {
                m_currentShader = m_scannedShaders[i];
                if (m_onShaderChanged) {
                    m_onShaderChanged(m_scannedShaders[i]);
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::Separator();
    ImGui::Text("Shaders found: %zu", m_scannedShaders.size());
    
    // Botões de salvar preset
    if (m_shaderEngine && m_shaderEngine->isShaderActive()) {
        ImGui::Separator();
        ImGui::Text("Save Preset:");
        
        std::string currentPreset = m_shaderEngine->getPresetPath();
        if (!currentPreset.empty()) {
            // Extrair apenas o nome do arquivo
            std::filesystem::path presetPath(currentPreset);
            std::string fileName = presetPath.filename().string();
            
            if (ImGui::Button("Save")) {
                // Salvar por cima do arquivo atual
                if (m_onSavePreset) {
                    m_onSavePreset(currentPreset, true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As...")) {
                // Abrir dialog para salvar como novo arquivo
                strncpy(m_savePresetPath, fileName.c_str(), sizeof(m_savePresetPath) - 1);
                m_savePresetPath[sizeof(m_savePresetPath) - 1] = '\0';
                m_showSaveDialog = true;
            }
        } else {
            ImGui::TextDisabled("No preset loaded");
        }
        
        // Dialog para "Save As"
        if (m_showSaveDialog) {
            ImGui::OpenPopup("Save Preset As");
            m_showSaveDialog = false;
        }
        
        if (ImGui::BeginPopupModal("Save Preset As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter preset filename:");
            ImGui::InputText("##presetname", m_savePresetPath, sizeof(m_savePresetPath));
            
            if (ImGui::Button("Save")) {
                if (m_onSavePreset && strlen(m_savePresetPath) > 0) {
                    // Construir caminho completo
                    std::filesystem::path basePath("shaders/shaders_glsl");
                    std::filesystem::path newPath = basePath / m_savePresetPath;
                    // Garantir extensão .glslp
                    if (newPath.extension() != ".glslp") {
                        newPath.replace_extension(".glslp");
                    }
                    m_onSavePreset(newPath.string(), false);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
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

void UIManager::renderImageControls() {
    ImGui::Text("Image Adjustments");
    ImGui::Separator();
    
    // Brightness
    float brightness = m_brightness;
    if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 2.0f, "%.2f")) {
        m_brightness = brightness;
        if (m_onBrightnessChanged) {
            m_onBrightnessChanged(brightness);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##brightness")) {
        m_brightness = 1.0f;
        if (m_onBrightnessChanged) {
            m_onBrightnessChanged(1.0f);
        }
    }
    
    // Contrast
    float contrast = m_contrast;
    if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 5.0f, "%.2f")) {
        m_contrast = contrast;
        if (m_onContrastChanged) {
            m_onContrastChanged(contrast);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##contrast")) {
        m_contrast = 1.0f;
        if (m_onContrastChanged) {
            m_onContrastChanged(1.0f);
        }
    }
    
    ImGui::Separator();
    
    // Maintain aspect ratio
    bool maintainAspect = m_maintainAspect;
    if (ImGui::Checkbox("Maintain Aspect Ratio", &maintainAspect)) {
        m_maintainAspect = maintainAspect;
        if (m_onMaintainAspectChanged) {
            m_onMaintainAspectChanged(maintainAspect);
        }
    }
    
    // Fullscreen
    bool fullscreen = m_fullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        m_fullscreen = fullscreen;
        if (m_onFullscreenChanged) {
            m_onFullscreenChanged(fullscreen);
        }
    }
    
    // Monitor Index (usado quando fullscreen está ativo)
    ImGui::Separator();
    ImGui::Text("Monitor Index:");
    if (!fullscreen && !m_fullscreen) {
        ImGui::TextDisabled("(only used in fullscreen mode)");
    } else {
        ImGui::TextDisabled("(-1 = primary monitor, 0+ = specific monitor)");
    }
    int monitorIndex = m_monitorIndex;
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("##monitor", &monitorIndex, 1, 5)) {
        monitorIndex = std::max(-1, monitorIndex); // Não permitir valores negativos menores que -1
        m_monitorIndex = monitorIndex;
        if (m_onMonitorIndexChanged) {
            m_onMonitorIndexChanged(monitorIndex);
        }
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Reset##monitor")) {
        m_monitorIndex = -1;
        if (m_onMonitorIndexChanged) {
            m_onMonitorIndexChanged(-1);
        }
    }
}

void UIManager::renderV4L2Controls() {
    if (!m_capture) {
        ImGui::Text("V4L2 controls not available");
        return;
    }
    
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
    if ((widthDeactivated || heightDeactivated) && (widthEdited || heightEdited)) {
        if (width != static_cast<int>(m_captureWidth) || height != static_cast<int>(m_captureHeight)) {
            m_captureWidth = static_cast<uint32_t>(width);
            m_captureHeight = static_cast<uint32_t>(height);
            if (m_onResolutionChanged) {
                m_onResolutionChanged(m_captureWidth, m_captureHeight);
            }
        }
    }
    
    // Resoluções comuns (botões rápidos)
    ImGui::Text("Quick resolutions:");
    if (ImGui::Button("640x480")) {
        m_captureWidth = 640;
        m_captureHeight = 480;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(640, 480);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("800x600")) {
        m_captureWidth = 800;
        m_captureHeight = 600;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(800, 600);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("1280x720")) {
        m_captureWidth = 1280;
        m_captureHeight = 720;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(1280, 720);
        }
    }
    if (ImGui::Button("1920x1080")) {
        m_captureWidth = 1920;
        m_captureHeight = 1080;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(1920, 1080);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1440")) {
        m_captureWidth = 2560;
        m_captureHeight = 1440;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(2560, 1440);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("3840x2160")) {
        m_captureWidth = 3840;
        m_captureHeight = 2160;
        if (m_onResolutionChanged) {
            m_onResolutionChanged(3840, 2160);
        }
    }
    
    ImGui::Separator();
    
    // Controle de FPS
    ImGui::Text("Framerate:");
    int fps = static_cast<int>(m_captureFps);
    ImGui::PushItemWidth(100);
    bool fpsEdited = ImGui::InputInt("FPS##capture", &fps, 1, 5);
    fps = std::max(1, std::min(240, fps)); // Limitar entre 1 e 240
    ImGui::PopItemWidth();
    
    // Aplicar mudanças quando o campo perder o foco
    if (ImGui::IsItemDeactivatedAfterEdit() && fpsEdited) {
        if (fps != static_cast<int>(m_captureFps)) {
            m_captureFps = static_cast<uint32_t>(fps);
            if (m_onFramerateChanged) {
                m_onFramerateChanged(m_captureFps);
            }
        }
    }
    
    // FPS comuns (botões rápidos)
    ImGui::Text("Quick FPS:");
    if (ImGui::Button("30")) {
        m_captureFps = 30;
        if (m_onFramerateChanged) {
            m_onFramerateChanged(30);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("60")) {
        m_captureFps = 60;
        if (m_onFramerateChanged) {
            m_onFramerateChanged(60);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("120")) {
        m_captureFps = 120;
        if (m_onFramerateChanged) {
            m_onFramerateChanged(120);
        }
    }
    
    ImGui::Separator();
    ImGui::Text("V4L2 Hardware Controls");
    ImGui::Separator();
    
    for (auto& control : m_v4l2Controls) {
        if (!control.available) {
            continue;
        }
        
        int32_t value = control.value;
        if (ImGui::SliderInt(control.name.c_str(), &value, control.min, control.max)) {
            control.value = value;
            if (m_onV4L2ControlChanged) {
                m_onV4L2ControlChanged(control.name, value);
            }
        }
    }
}

void UIManager::setV4L2Controls(VideoCapture* capture) {
    m_capture = capture;
    m_v4l2Controls.clear();
    
    if (!capture) {
        return;
    }
    
    // Lista de controles V4L2 comuns
    struct ControlInfo {
        const char* name;
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
    
    for (const auto& info : controls) {
        V4L2Control ctrl;
        ctrl.name = info.name;
        ctrl.available = capture->getControl(info.cid, ctrl.value, ctrl.min, ctrl.max, ctrl.step);
        
        if (ctrl.available) {
            m_v4l2Controls.push_back(ctrl);
        }
    }
}

void UIManager::renderInfoPanel() {
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

void UIManager::setCaptureInfo(uint32_t width, uint32_t height, uint32_t fps, const std::string& device) {
    m_captureWidth = width;
    m_captureHeight = height;
    m_captureFps = fps;
    m_captureDevice = device;
}

void UIManager::scanShaders(const std::string& basePath) {
    m_scannedShaders.clear();
    
    std::filesystem::path path(basePath);
    if (!std::filesystem::exists(path)) {
        // Tentar caminho relativo ao diretório de trabalho
        path = std::filesystem::current_path() / basePath;
    }
    
    if (!std::filesystem::exists(path)) {
        LOG_WARN("Diretório de shaders não encontrado: " + basePath);
        return;
    }
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                if (ext == ".glslp") {
                    std::string relativePath = std::filesystem::relative(entry.path(), path).string();
                    m_scannedShaders.push_back(relativePath);
                }
            }
        }
        
        std::sort(m_scannedShaders.begin(), m_scannedShaders.end());
        LOG_INFO("Encontrados " + std::to_string(m_scannedShaders.size()) + " shaders em " + basePath);
    } catch (const std::exception& e) {
        LOG_ERROR("Erro ao escanear shaders: " + std::string(e.what()));
    }
}

