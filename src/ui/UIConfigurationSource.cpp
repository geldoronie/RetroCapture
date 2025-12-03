#include "UIConfigurationSource.h"
#include "UIManager.h"
#include "../capture/VideoCapture.h"
#include <imgui.h>
#include <algorithm>
#include <linux/videodev2.h>

UIConfigurationSource::UIConfigurationSource(UIManager *uiManager)
    : m_uiManager(uiManager)
{
    if (uiManager)
    {
        m_capture = uiManager->getCapture();
    }
}

UIConfigurationSource::~UIConfigurationSource()
{
}

void UIConfigurationSource::render()
{
    if (!m_uiManager)
    {
        return;
    }

    // Atualizar referência ao capture se necessário
    m_capture = m_uiManager->getCapture();

    renderSourceTypeSelection();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Renderizar controles específicos da fonte selecionada
    UIManager::SourceType sourceType = m_uiManager->getSourceType();
    if (sourceType == UIManager::SourceType::V4L2)
    {
        renderV4L2Controls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("Nenhuma fonte selecionada. Selecione um tipo de fonte acima.");
    }
}

void UIConfigurationSource::renderSourceTypeSelection()
{
    ImGui::Text("Source Type:");
    ImGui::Separator();
    ImGui::Spacing();

    // Dropdown para seleção do tipo de fonte
    const char *sourceTypeNames[] = {"None", "V4L2"};
    int currentSourceType = static_cast<int>(m_uiManager->getSourceType());

    if (ImGui::Combo("##sourceType", &currentSourceType, sourceTypeNames, IM_ARRAYSIZE(sourceTypeNames)))
    {
        m_uiManager->setSourceType(static_cast<UIManager::SourceType>(currentSourceType));
    }
}

void UIConfigurationSource::renderV4L2Controls()
{
    // Sempre mostrar controles, mesmo sem dispositivo
    // Se não houver dispositivo, mostrar mensagem informativa
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("Nenhum dispositivo V4L2 conectado. Selecione um dispositivo abaixo para iniciar a captura.");
        ImGui::Separator();
    }

    renderV4L2DeviceSelection();
    ImGui::Separator();
    renderCaptureSettings();
    ImGui::Separator();
    renderQuickResolutions();
    ImGui::Separator();
    renderQuickFPS();
    ImGui::Separator();

    // V4L2 Hardware Controls
    ImGui::Text("V4L2 Hardware Controls");
    ImGui::Separator();

    // Renderizar controles dinâmicos (discovered from device)
    const auto &controls = m_uiManager->getV4L2Controls();
    for (size_t i = 0; i < controls.size(); ++i)
    {
        const auto &control = controls[i];
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
            m_uiManager->triggerV4L2ControlChange(control.name, value);
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

            m_uiManager->triggerV4L2ControlChange(name, value);
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

void UIConfigurationSource::renderV4L2DeviceSelection()
{
    // Device selection
    ImGui::Text("V4L2 Device:");
    ImGui::Separator();

    // Scan devices if list is empty
    const auto &devices = m_uiManager->getV4L2Devices();
    if (devices.empty())
    {
        m_uiManager->refreshV4L2Devices();
    }

    // Combo box for device selection
    // Adicionar "None" como primeira opção
    std::string currentDevice = m_uiManager->getCurrentDevice();
    std::string displayText = currentDevice.empty() ? "None (No device)" : currentDevice;
    int selectedIndex = -1;

    // Verificar se "None" está selecionado
    if (currentDevice.empty())
    {
        selectedIndex = 0; // "None" é o índice 0
    }
    else
    {
        // Procurar dispositivo na lista (índice +1 porque "None" é 0)
        for (size_t i = 0; i < devices.size(); ++i)
        {
            if (devices[i] == currentDevice)
            {
                selectedIndex = static_cast<int>(i) + 1; // +1 porque "None" é 0
                break;
            }
        }
    }

    if (ImGui::BeginCombo("##device", displayText.c_str()))
    {
        // Opção "None" sempre como primeira opção
        bool isNoneSelected = currentDevice.empty();
        if (ImGui::Selectable("None (No device)", isNoneSelected))
        {
            m_uiManager->triggerDeviceChange("");
            m_uiManager->saveConfig();
        }
        if (isNoneSelected)
        {
            ImGui::SetItemDefaultFocus();
        }

        // Listar dispositivos disponíveis
        for (size_t i = 0; i < devices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i) + 1);
            if (ImGui::Selectable(devices[i].c_str(), isSelected))
            {
                m_uiManager->triggerDeviceChange(devices[i]);
                m_uiManager->saveConfig();
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
        m_uiManager->refreshV4L2Devices();
    }
}

void UIConfigurationSource::renderCaptureSettings()
{
    ImGui::Text("Capture Resolution & Framerate");
    ImGui::Separator();

    // Controles de resolução
    ImGui::Text("Resolution:");
    int width = static_cast<int>(m_uiManager->getCaptureWidth());
    int height = static_cast<int>(m_uiManager->getCaptureHeight());

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
        if (width != static_cast<int>(m_uiManager->getCaptureWidth()) ||
            height != static_cast<int>(m_uiManager->getCaptureHeight()))
        {
            m_uiManager->triggerResolutionChange(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    }

    // Controle de FPS
    ImGui::Text("Framerate:");
    int fps = static_cast<int>(m_uiManager->getCaptureFps());
    ImGui::PushItemWidth(100);
    bool fpsEdited = ImGui::InputInt("FPS##capture", &fps, 1, 5);
    fps = std::max(1, std::min(240, fps)); // Limitar entre 1 e 240
    ImGui::PopItemWidth();

    // Aplicar mudanças quando o campo perder o foco
    if (ImGui::IsItemDeactivatedAfterEdit() && fpsEdited)
    {
        if (fps != static_cast<int>(m_uiManager->getCaptureFps()))
        {
            m_uiManager->triggerFramerateChange(static_cast<uint32_t>(fps));
        }
    }
}

void UIConfigurationSource::renderQuickFPS()
{
    // FPS comuns (botões rápidos)
    ImGui::Text("Quick FPS:");
    if (ImGui::Button("30"))
    {
        m_uiManager->triggerFramerateChange(30);
    }
    ImGui::SameLine();
    if (ImGui::Button("60"))
    {
        m_uiManager->triggerFramerateChange(60);
    }
    ImGui::SameLine();
    if (ImGui::Button("120"))
    {
        m_uiManager->triggerFramerateChange(120);
    }
}

void UIConfigurationSource::renderQuickResolutions()
{
    // Resoluções 4:3
    ImGui::Text("4:3 Resolutions:");
    if (ImGui::Button("320x240"))
    {
        m_uiManager->triggerResolutionChange(320, 240);
    }
    ImGui::SameLine();
    if (ImGui::Button("640x480"))
    {
        m_uiManager->triggerResolutionChange(640, 480);
    }
    ImGui::SameLine();
    if (ImGui::Button("800x600"))
    {
        m_uiManager->triggerResolutionChange(800, 600);
    }
    if (ImGui::Button("1024x768"))
    {
        m_uiManager->triggerResolutionChange(1024, 768);
    }
    ImGui::SameLine();
    if (ImGui::Button("1280x960"))
    {
        m_uiManager->triggerResolutionChange(1280, 960);
    }
    ImGui::SameLine();
    if (ImGui::Button("1600x1200"))
    {
        m_uiManager->triggerResolutionChange(1600, 1200);
    }
    if (ImGui::Button("2048x1536"))
    {
        m_uiManager->triggerResolutionChange(2048, 1536);
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1920"))
    {
        m_uiManager->triggerResolutionChange(2560, 1920);
    }

    ImGui::Separator();

    // Resoluções 16:9
    ImGui::Text("16:9 Resolutions:");
    if (ImGui::Button("1280x720"))
    {
        m_uiManager->triggerResolutionChange(1280, 720);
    }
    ImGui::SameLine();
    if (ImGui::Button("1920x1080"))
    {
        m_uiManager->triggerResolutionChange(1920, 1080);
    }
    ImGui::SameLine();
    if (ImGui::Button("2560x1440"))
    {
        m_uiManager->triggerResolutionChange(2560, 1440);
    }
    if (ImGui::Button("3840x2160"))
    {
        m_uiManager->triggerResolutionChange(3840, 2160);
    }
}
