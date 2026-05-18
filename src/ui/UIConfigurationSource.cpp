#include "UIConfigurationSource.h"
#include "UIManager.h"
#include "../capture/IVideoCapture.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>
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

    if (sourceType == UIManager::SourceType::Remote)
    {
        renderRemoteControls();
        return;
    }
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

    // Dropdown for selecting the local capture source type.
    // 'Remote' no longer lives here — it now has its own top-level menu
    // ('Remote → Connect to Remote...') with a dedicated window for URL
    // and interpolation. The Source tab focuses only on physical capture
    // options local to the machine.
#ifdef __linux__
    const char *sourceTypeNames[] = {"None", "V4L2"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::V4L2};
#elif defined(_WIN32)
    const char *sourceTypeNames[] = {"None", "DirectShow"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::DS};
#else
    const char *sourceTypeNames[] = {"None"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None};
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

    // (Previously a hardcoded "All V4L2 Controls" block was rendered
    // here with a fixed list of Brightness/Contrast/Saturation/etc and
    // assumed default ranges. It duplicated whatever the dynamic
    // discovery above already shows for the connected device, and used
    // wrong ranges when the device exposed different bounds. Removed —
    // the dynamic loop is the source of truth.)
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

    // If currentDevice is set but doesn't match any V4L2 path, it's
    // leftover state from another source type (typically a remote
    // URL when the user had Remote mode active before switching to
    // V4L2). Showing it raw in the dropdown made http://localhost:…
    // appear as if it were a video device. Display "None" instead.
    std::string displayText = (currentDevice.empty() || selectedIndex < 0)
                              ? "None (No device)"
                              : currentDevice;

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
    ImGui::Separator();
    renderQuickResolutions();
    ImGui::Separator();
    renderQuickFPS();
    ImGui::Separator();

    // DirectShow Hardware Controls
    ImGui::Text("DirectShow Hardware Controls");
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
        std::string label = std::string(name) + "##dsmanual";
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

    // Se não houver dispositivos, mostrar mensagem mas ainda permitir seleção de "None"
    if (currentDevices.empty())
    {
        ImGui::TextWrapped("Nenhum dispositivo DirectShow encontrado. Clique em Refresh para atualizar.");
        ImGui::Spacing();
    }
    int         selectedIndex = -1;
    std::string matchedLabel;

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
                matchedLabel  = currentDevices[i].name + " (" + currentDevices[i].id + ")";
                break;
            }
        }
    }

    // If currentDevice is set but no DS device matched, the value
    // belongs to a different source type (typically a remote URL
    // like http://localhost:8080 left over from Remote mode). It
    // used to be rendered raw inside the dropdown — show "None"
    // instead so the field doesn't claim a URL is a DirectShow
    // camera.
    std::string displayText;
    if (selectedIndex == 0 || selectedIndex < 0) displayText = "None (No device)";
    else                                          displayText = matchedLabel;

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

    // Quando o V4L2 ajusta a resolução pra mais próxima suportada, mostramos
    // a real do dispositivo abaixo do campo. O pipeline faz downscale antes
    // do shader chain pra preservar o look retrô da resolução pedida.
    const uint32_t actualW = m_uiManager->getActualCaptureWidth();
    const uint32_t actualH = m_uiManager->getActualCaptureHeight();
    const uint32_t requestedW = m_uiManager->getCaptureWidth();
    const uint32_t requestedH = m_uiManager->getCaptureHeight();
    if (actualW > 0 && actualH > 0 &&
        (actualW != requestedW || actualH != requestedH))
    {
        ImGui::TextDisabled("Device delivers %ux%u (downscaled to %ux%u for shader)",
                            actualW, actualH, requestedW, requestedH);
    }

    // Overscan: corta uma % das bordas do source antes do downscale. Útil pra
    // remover letterbox do dispositivo de captura ou aproximar o crop de TV CRT.
    // Eixos X/Y independentes; lock espelha um no outro.
    ImGui::Text("Source Overscan:");
    bool overscanLocked = m_uiManager->getSourceOverscanLocked();
    if (ImGui::Checkbox("Lock X/Y##overscan_lock", &overscanLocked))
    {
        m_uiManager->setSourceOverscanLocked(overscanLocked);
        m_uiManager->saveConfig();
    }
    float overscanX = m_uiManager->getSourceOverscanPercentX();
    float overscanY = m_uiManager->getSourceOverscanPercentY();
    ImGui::PushItemWidth(180);
    if (ImGui::SliderFloat("Horizontal##source_overscan_x", &overscanX, 0.0f, 30.0f, "%.1f%%"))
    {
        m_uiManager->setSourceOverscanPercentX(overscanX);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        m_uiManager->saveConfig();
    }
    if (ImGui::SliderFloat("Vertical##source_overscan_y", &overscanY, 0.0f, 30.0f, "%.1f%%"))
    {
        m_uiManager->setSourceOverscanPercentY(overscanY);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        m_uiManager->saveConfig();
    }
    ImGui::PopItemWidth();

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
    // Resoluções nativas de consoles retrô. Quando o dispositivo de captura
    // não suporta a resolução escolhida, o V4L2 ajusta pra mais próxima e o
    // pipeline faz downscale antes do shader chain — o efeito CRT/scanline
    // fica autêntico (cada scanline vira N pixels altos no viewport final).
    ImGui::Text("Retro Consoles:");
    if (ImGui::Button("160x144 GB"))
    {
        m_uiManager->triggerResolutionChange(160, 144);
    }
    ImGui::SameLine();
    if (ImGui::Button("240x160 GBA"))
    {
        m_uiManager->triggerResolutionChange(240, 160);
    }
    ImGui::SameLine();
    if (ImGui::Button("256x192 SMS"))
    {
        m_uiManager->triggerResolutionChange(256, 192);
    }
    ImGui::SameLine();
    if (ImGui::Button("256x224 NES/SNES"))
    {
        m_uiManager->triggerResolutionChange(256, 224);
    }
    if (ImGui::Button("256x240 NES alt"))
    {
        m_uiManager->triggerResolutionChange(256, 240);
    }
    ImGui::SameLine();
    if (ImGui::Button("224x288 Vertical Arcade"))
    {
        m_uiManager->triggerResolutionChange(224, 288);
    }
    ImGui::SameLine();
    if (ImGui::Button("320x200 DOS/CGA"))
    {
        m_uiManager->triggerResolutionChange(320, 200);
    }
    if (ImGui::Button("320x224 Mega Drive"))
    {
        m_uiManager->triggerResolutionChange(320, 224);
    }
    ImGui::SameLine();
    if (ImGui::Button("320x240 Saturn/PSX"))
    {
        m_uiManager->triggerResolutionChange(320, 240);
    }
    ImGui::SameLine();
    if (ImGui::Button("304x224 Mega CD"))
    {
        m_uiManager->triggerResolutionChange(304, 224);
    }
    if (ImGui::Button("384x224 CPS-1"))
    {
        m_uiManager->triggerResolutionChange(384, 224);
    }
    ImGui::SameLine();
    if (ImGui::Button("384x288 CPS-2 PAL"))
    {
        m_uiManager->triggerResolutionChange(384, 288);
    }
    ImGui::SameLine();
    if (ImGui::Button("640x448 PSX Hi-Res"))
    {
        m_uiManager->triggerResolutionChange(640, 448);
    }

    ImGui::Separator();

    // Resoluções 4:3
    ImGui::Text("4:3 Resolutions:");
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

void UIConfigurationSource::renderRemoteControls()
{
    // Remote-mode controls moved out of the Source tab into a dedicated
    // top-level menu entry ('Remote → Connect to Remote...'). This stub
    // just points the user at the new location when they land here with
    // a Remote source already loaded from config.
    ImGui::TextWrapped(
        "Remote viewer mode — connection controls moved to the 'Remote' menu. "
        "Use 'Remote → Connect to Remote...' to manage URL, interpolation and "
        "the connect/disconnect actions.");
    ImGui::Spacing();
    const std::string currentDevice = m_uiManager->getCurrentDevice();
    if (!currentDevice.empty())
    {
        ImGui::TextDisabled("connected to %s", currentDevice.c_str());
    }
    else
    {
        ImGui::TextDisabled("not connected");
    }
}
