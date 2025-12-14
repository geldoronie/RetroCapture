#include "UIConfigurationSource.h"
#include "UIManager.h"
#include "../capture/IVideoCapture.h"
#include <imgui.h>
#include <algorithm>
#ifdef __linux__
#include <linux/videodev2.h>
#endif

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
    
    // Não atualizar a lista aqui - será atualizada apenas quando necessário em renderDSDeviceSelection
    // Isso evita chamar refreshDSDevices() a cada frame

    renderSourceTypeSelection();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Renderizar controles específicos da fonte selecionada
    UIManager::SourceType sourceType = m_uiManager->getSourceType();
#ifdef __linux__
    if (sourceType == UIManager::SourceType::V4L2)
    {
        renderV4L2Controls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("Nenhuma fonte selecionada. Selecione um tipo de fonte acima.");
    }
#elif defined(_WIN32)
    if (sourceType == UIManager::SourceType::DS)
    {
        renderDSControls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("Nenhuma fonte selecionada. Selecione um tipo de fonte acima.");
    }
#else
    if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("Nenhuma fonte selecionada.");
    }
#endif
}

void UIConfigurationSource::renderSourceTypeSelection()
{
    ImGui::Text("Source Type:");
    ImGui::Separator();
    ImGui::Spacing();

// Dropdown para seleção do tipo de fonte
#ifdef __linux__
    const char *sourceTypeNames[] = {"None", "V4L2"};
    // Mapeamento: índice 0 = None (0), índice 1 = V4L2 (1)
    UIManager::SourceType sourceTypeMap[] = {UIManager::SourceType::None, UIManager::SourceType::V4L2};
#elif defined(_WIN32)
    const char *sourceTypeNames[] = {"None", "DirectShow"};
    // Mapeamento: índice 0 = None (0), índice 1 = MF (2)
    UIManager::SourceType sourceTypeMap[] = {UIManager::SourceType::None, UIManager::SourceType::DS};
#else
    const char *sourceTypeNames[] = {"None"};
    UIManager::SourceType sourceTypeMap[] = {UIManager::SourceType::None};
#endif

    // Encontrar índice atual baseado no SourceType
    int currentIndex = 0;
    UIManager::SourceType currentSourceType = m_uiManager->getSourceType();
    for (int i = 0; i < IM_ARRAYSIZE(sourceTypeNames); ++i)
    {
        if (sourceTypeMap[i] == currentSourceType)
        {
            currentIndex = i;
            break;
        }
    }

    if (ImGui::Combo("##sourceType", &currentIndex, sourceTypeNames, IM_ARRAYSIZE(sourceTypeNames)))
    {
        if (currentIndex >= 0 && currentIndex < IM_ARRAYSIZE(sourceTypeMap))
        {
            m_uiManager->setSourceType(sourceTypeMap[currentIndex]);
        }
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
            m_uiManager->triggerV4L2ControlChange(name, value);
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

#ifdef _WIN32
void UIConfigurationSource::renderDSControls()
{
    // Atualizar referência ao capture ANTES de verificar
    m_capture = m_uiManager->getCapture();
    
    // Sempre mostrar controles, mesmo sem dispositivo
    // Se não houver dispositivo, mostrar mensagem informativa
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("Nenhum dispositivo DirectShow conectado. Selecione um dispositivo abaixo para iniciar a captura.");
        ImGui::Separator();
    }

    renderDSDeviceSelection();
    ImGui::Separator();
    renderCaptureSettings();
}

void UIConfigurationSource::renderDSDeviceSelection()
{
    // Device selection
    ImGui::Text("DirectShow Device:");
    ImGui::Separator();

    // Obter o ponteiro do capture do UIManager (sempre atualizar)
    m_capture = m_uiManager->getCapture();

    // Usar cache de dispositivos do UIManager (similar ao V4L2)
    // Obter lista atual (cópia para evitar problemas de referência)
    auto currentDevices = m_uiManager->getDSDevices();
    
    // Apenas atualizar a lista se estiver vazia E m_capture estiver disponível
    // Isso evita chamar refreshDSDevices() a cada frame
    if (currentDevices.empty() && m_capture)
    {
        m_uiManager->refreshDSDevices();
        // Obter lista novamente após atualização
        currentDevices = m_uiManager->getDSDevices();
    }
    
    // NÃO retornar se m_capture for nullptr - ainda podemos mostrar a lista se ela já foi enumerada
    // A lista pode ter sido populada anteriormente mesmo que m_capture não esteja disponível agora

    // Combo box for device selection
    // Adicionar "None" como primeira opção
    std::string currentDevice = m_uiManager->getCurrentDevice();
    std::string displayText = currentDevice.empty() ? "None (No device)" : currentDevice;
    
    // Se não houver dispositivos, mostrar mensagem mas ainda permitir seleção de "None"
    if (currentDevices.empty())
    {
        ImGui::TextWrapped("Nenhum dispositivo DirectShow encontrado. Clique em Refresh para atualizar.");
        ImGui::Spacing();
    }
    int selectedIndex = -1;

    // Verificar se "None" está selecionado
    if (currentDevice.empty())
    {
        selectedIndex = 0; // "None" é o índice 0
    }
    else
    {
        // Procurar dispositivo na lista (índice +1 porque "None" é 0)
        for (size_t i = 0; i < currentDevices.size(); ++i)
        {
            if (currentDevices[i].id == currentDevice || currentDevices[i].name == currentDevice)
            {
                selectedIndex = static_cast<int>(i) + 1; // +1 porque "None" é 0
                displayText = currentDevices[i].name + " (" + currentDevices[i].id + ")";
                break;
            }
        }
    }

    if (ImGui::BeginCombo("##dsdevice", displayText.c_str()))
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
        for (size_t i = 0; i < currentDevices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i) + 1);
            std::string deviceLabel = currentDevices[i].name;
            if (!currentDevices[i].id.empty() && currentDevices[i].id != currentDevices[i].name)
            {
                deviceLabel += " (" + currentDevices[i].id + ")";
            }
            if (ImGui::Selectable(deviceLabel.c_str(), isSelected))
            {
                // Usar o ID do dispositivo (índice numérico para MF)
                m_uiManager->triggerDeviceChange(currentDevices[i].id);
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
    if (ImGui::Button("Refresh##dsdevices"))
    {
        // Recarregar lista de dispositivos
        m_uiManager->refreshDSDevices();
    }
}
#endif

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
