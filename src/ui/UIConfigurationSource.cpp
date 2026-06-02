#include "UIConfigurationSource.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/TranslationManager.h"
#include "../capture/IVideoCapture.h"
#include "../capture/VideoCaptureScreen.h"
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
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(620, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("source.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    // Atualizar referência ao capture se necessário
    m_capture = m_uiManager->getCapture();

    renderSourceTypeSelection();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    UIManager::SourceType sourceType = m_uiManager->getSourceType();

    if (sourceType == UIManager::SourceType::Remote)
    {
        renderRemoteControls();
        ImGui::End();
        return;
    }
    if (sourceType == UIManager::SourceType::Screen)
    {
        renderScreenControls();
        ImGui::End();
        return;
    }
#ifdef __linux__
    if (sourceType == UIManager::SourceType::V4L2)
    {
        renderV4L2Controls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("No source selected. Pick a source type above.");
    }
#elif defined(_WIN32)
    if (sourceType == UIManager::SourceType::DS)
    {
        renderDSControls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("No source selected. Pick a source type above.");
    }
#elif defined(__APPLE__)
    if (sourceType == UIManager::SourceType::AVFoundation)
    {
        renderAVFoundationControls();
    }
    else if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("No source selected. Pick a source type above.");
    }
#else
    if (sourceType == UIManager::SourceType::None)
    {
        ImGui::TextWrapped("No source selected.");
    }
#endif

    ImGui::End();
}

void UIConfigurationSource::renderSourceTypeSelection()
{
    ui_section_header("Source",
                      "Which physical capture device to read frames "
                      "from. Switching here re-initialises the pipeline.");

    // Dropdown for selecting the local capture source type.
    // 'Remote' no longer lives here — it now has its own top-level menu
    // ('Remote → Connect to Remote...') with a dedicated window for URL
    // and interpolation. The Source tab focuses only on physical capture
    // options local to the machine.
    // 'Screen' (#107) is cross-platform and offered everywhere.
#ifdef __linux__
    const char *sourceTypeNames[] = {"None", "V4L2", "Screen"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::V4L2,
        UIManager::SourceType::Screen};
#elif defined(_WIN32)
    const char *sourceTypeNames[] = {"None", "DirectShow", "Screen"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::DS,
        UIManager::SourceType::Screen};
#elif defined(__APPLE__)
    const char *sourceTypeNames[] = {"None", "AVFoundation", "Screen"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::AVFoundation,
        UIManager::SourceType::Screen};
#else
    const char *sourceTypeNames[] = {"None", "Screen"};
    UIManager::SourceType sourceTypeMap[] = {
        UIManager::SourceType::None,
        UIManager::SourceType::Screen};
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
    // Re-fetch the live capture pointer. render() caches it at the top
    // of the frame, but renderSourceTypeSelection() (called just before
    // the dispatch here) can switch the source type and rebuild the
    // backend mid-frame — leaving the cached pointer dangling at the
    // just-destroyed instance. renderDSControls/AVFoundation already
    // re-fetch for the same reason; V4L2 didn't, and dereferencing the
    // stale pointer below segfaulted right after a Screen→V4L2 switch.
    m_capture = m_uiManager->getCapture();

    // Sempre mostrar controles, mesmo sem dispositivo
    // Se não houver dispositivo, mostrar mensagem informativa
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("No V4L2 device connected. Select a device below to start capture.");
        ImGui::Separator();
    }

    renderV4L2DeviceSelection();
    ImGui::Separator();
    renderCaptureSettings();
    ImGui::Separator();
    renderQuickResolutions();
    ImGui::Separator();
    renderQuickFPS();

    ui_section_header("V4L2 Hardware Controls",
                      "Driver-exposed knobs (brightness, hue, exposure, "
                      "etc.) — set is device-specific.");

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
        ImGui::TextWrapped("No DirectShow device connected. Select a device below to start capture.");
        ImGui::Separator();
    }

    renderDSDeviceSelection();
    ImGui::Separator();
    renderCaptureSettings();
    ImGui::Separator();
    renderQuickResolutions();
    ImGui::Separator();
    renderQuickFPS();

    ui_section_header("DirectShow Hardware Controls",
                      "Filter-exposed knobs from the DirectShow driver. "
                      "Set is device-specific.");

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
        ImGui::TextWrapped("No DirectShow devices found. Click Refresh to update.");
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
    ui_section_header("Capture Resolution & Framerate",
                      "What the source streams at — separate from the "
                      "Streaming / Recording target settings.");

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

void UIConfigurationSource::renderScreenControls()
{
    ui_section_header("Screen Capture",
                      "Capture the desktop — a whole monitor, a window, "
                      "or a cropped region — as the source.");

    // Target enumeration. Cached so we don't re-enumerate every frame;
    // a Refresh re-scans (windows come and go). On Wayland the portal's
    // own picker ultimately decides, so this list may be a single entry.
    if (!m_screenTargetsLoaded)
    {
        VideoCaptureScreen probe;
        m_screenTargets       = probe.listDevices();
        m_screenTargetsLoaded = true;
    }

    if (ImGui::Button("Refresh targets"))
    {
        VideoCaptureScreen probe;
        m_screenTargets = probe.listDevices();
    }

    const std::string current = m_uiManager->getCurrentDevice();
    std::string preview = current.empty() ? "(none)" : current;
    for (const auto &d : m_screenTargets)
        if (d.id == current) { preview = d.name; break; }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##screenTarget", preview.c_str()))
    {
        for (const auto &d : m_screenTargets)
        {
            const bool selected = (d.id == current);
            if (ImGui::Selectable(d.name.c_str(), selected))
            {
                m_uiManager->triggerDeviceChange(d.id);
                m_uiManager->saveConfig();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Target");

    if (!current.empty())
    {
        if (ImGui::Button("Stop capture"))
        {
            m_uiManager->triggerDeviceChange("");
            m_uiManager->saveConfig();
        }
    }

    ImGui::Separator();
    ui_section_header("Region (crop)",
                      "Limit the captured area, in target pixels. "
                      "All zero = capture the full target.");

    // Seed the ImGui scratch from persisted config once.
    if (!m_screenRegionSeeded)
    {
        m_screenRegion[0] = static_cast<int>(m_uiManager->getScreenRegionX());
        m_screenRegion[1] = static_cast<int>(m_uiManager->getScreenRegionY());
        m_screenRegion[2] = static_cast<int>(m_uiManager->getScreenRegionW());
        m_screenRegion[3] = static_cast<int>(m_uiManager->getScreenRegionH());
        m_screenRegionSeeded = true;
    }

    // Spin edits (±1 / ±10 with the step buttons) so fine adjustments
    // are easy without dragging. Pairs on a line to stay compact.
    ImGui::PushItemWidth(130.0f);
    ImGui::InputInt("X", &m_screenRegion[0], 1, 10);
    ImGui::SameLine();
    ImGui::InputInt("Y", &m_screenRegion[1], 1, 10);
    ImGui::InputInt("W", &m_screenRegion[2], 1, 10);
    ImGui::SameLine();
    ImGui::InputInt("H", &m_screenRegion[3], 1, 10);
    ImGui::PopItemWidth();
    for (int i = 0; i < 4; ++i) if (m_screenRegion[i] < 0) m_screenRegion[i] = 0;

    if (ImGui::Button("Apply region"))
    {
        m_uiManager->triggerScreenRegionChange(
            static_cast<uint32_t>(m_screenRegion[0]), static_cast<uint32_t>(m_screenRegion[1]),
            static_cast<uint32_t>(m_screenRegion[2]), static_cast<uint32_t>(m_screenRegion[3]));
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear region"))
    {
        m_screenRegion[0] = m_screenRegion[1] = m_screenRegion[2] = m_screenRegion[3] = 0;
        m_uiManager->triggerScreenRegionChange(0, 0, 0, 0);
    }

    ImGui::Spacing();
    if (ImGui::Button("Select region visually…"))
    {
        // Remember the current crop, then show the full (uncropped) frame
        // live so the user marquees against the whole target.
        for (int i = 0; i < 4; ++i) m_savedRegion[i] = m_screenRegion[i];
        m_uiManager->applyScreenRegionLive(0, 0, 0, 0);
        m_haveSelection   = false;
        m_marqueeActive   = false;
        m_regionSelectorOpen = true;
    }

    renderRegionSelector();
}

void UIConfigurationSource::renderRegionSelector()
{
    if (!m_regionSelectorOpen) return;

    ImGui::SetNextWindowSize(ImVec2(820, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Select capture region", &open, ImGuiWindowFlags_NoSavedSettings))
    {
        const unsigned int tex = m_uiManager->getCaptureTextureId();
        const float fullW = static_cast<float>(m_uiManager->getCaptureTextureWidth());
        const float fullH = static_cast<float>(m_uiManager->getCaptureTextureHeight());

        if (tex == 0 || fullW < 1.0f || fullH < 1.0f)
        {
            ImGui::TextWrapped("Waiting for a frame — make sure a screen target is "
                               "being captured.");
        }
        else
        {
            ImGui::TextDisabled("Drag on the image to mark the region. Full frame: %d x %d",
                                static_cast<int>(fullW), static_cast<int>(fullH));

            float availW = ImGui::GetContentRegionAvail().x;
            if (availW < 64.0f) availW = 720.0f;
            float scale = availW / fullW;
            if (scale > 1.0f) scale = 1.0f;
            const float dispW = fullW * scale;
            const float dispH = fullH * scale;

            const ImVec2 imgPos = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(dispW, dispH));

            const ImVec2 mouse = ImGui::GetIO().MousePos;
            auto clampf = [](float v, float hi) { return v < 0.0f ? 0.0f : (v > hi ? hi : v); };
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
            {
                m_selX0 = clampf(mouse.x - imgPos.x, dispW);
                m_selY0 = clampf(mouse.y - imgPos.y, dispH);
                m_selX1 = m_selX0;
                m_selY1 = m_selY0;
                m_marqueeActive = true;
                m_haveSelection = true;
            }
            if (m_marqueeActive && ImGui::IsMouseDown(0))
            {
                m_selX1 = clampf(mouse.x - imgPos.x, dispW);
                m_selY1 = clampf(mouse.y - imgPos.y, dispH);
            }
            if (m_marqueeActive && ImGui::IsMouseReleased(0)) m_marqueeActive = false;

            if (m_haveSelection)
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const ImVec2 a(imgPos.x + std::min(m_selX0, m_selX1),
                               imgPos.y + std::min(m_selY0, m_selY1));
                const ImVec2 b(imgPos.x + std::max(m_selX0, m_selX1),
                               imgPos.y + std::max(m_selY0, m_selY1));
                dl->AddRectFilled(a, b, IM_COL32(64, 160, 255, 40));
                dl->AddRect(a, b, IM_COL32(64, 160, 255, 255), 0.0f, 0, 2.0f);
            }

            const int rx = static_cast<int>(std::min(m_selX0, m_selX1) / scale);
            const int ry = static_cast<int>(std::min(m_selY0, m_selY1) / scale);
            const int rw = static_cast<int>((std::max(m_selX0, m_selX1) - std::min(m_selX0, m_selX1)) / scale);
            const int rh = static_cast<int>((std::max(m_selY0, m_selY1) - std::min(m_selY0, m_selY1)) / scale);
            ImGui::Text("X %d   Y %d   W %d   H %d", rx, ry, rw, rh);

            const bool valid = m_haveSelection && rw >= 8 && rh >= 8;
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Apply"))
            {
                m_screenRegion[0] = rx; m_screenRegion[1] = ry;
                m_screenRegion[2] = rw; m_screenRegion[3] = rh;
                m_uiManager->triggerScreenRegionChange(
                    static_cast<uint32_t>(rx), static_cast<uint32_t>(ry),
                    static_cast<uint32_t>(rw), static_cast<uint32_t>(rh));
                m_regionSelectorOpen = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Use full frame"))
            {
                for (int i = 0; i < 4; ++i) m_screenRegion[i] = 0;
                m_uiManager->triggerScreenRegionChange(0, 0, 0, 0);
                m_regionSelectorOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_uiManager->triggerScreenRegionChange(
                    static_cast<uint32_t>(m_savedRegion[0]), static_cast<uint32_t>(m_savedRegion[1]),
                    static_cast<uint32_t>(m_savedRegion[2]), static_cast<uint32_t>(m_savedRegion[3]));
                m_regionSelectorOpen = false;
            }
        }
    }
    ImGui::End();

    // Closed via the window's X → treat as Cancel: restore the crop the
    // user had before opening the selector.
    if (!open && m_regionSelectorOpen)
    {
        m_uiManager->triggerScreenRegionChange(
            static_cast<uint32_t>(m_savedRegion[0]), static_cast<uint32_t>(m_savedRegion[1]),
            static_cast<uint32_t>(m_savedRegion[2]), static_cast<uint32_t>(m_savedRegion[3]));
        m_regionSelectorOpen = false;
    }
}

#ifdef __APPLE__

void UIConfigurationSource::renderAVFoundationControls()
{
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("No AVFoundation device connected. Pick a device below "
                           "to start capture.");
        ImGui::Separator();
    }

    renderAVFoundationDeviceSelection();
    ImGui::Separator();
    renderAVFoundationFormatSelection();
    // AVFoundation on macOS does not surface per-device hardware
    // controls (brightness, gain, etc.) the way V4L2 / DirectShow do,
    // so there is no hardware-controls section here. See
    // docs/MACOS_PORT_STRATEGY.md.
}

void UIConfigurationSource::renderAVFoundationDeviceSelection()
{
    ui_section_header("AVFoundation device",
                      "macOS capture device. The list is sourced from "
                      "AVCaptureDevice — UVC HDMI grabbers, webcams, and "
                      "any virtual camera installed on the system show up here.");

    if (m_avfDevicesNeedRefresh && m_capture)
    {
        m_avfDevices = m_capture->listDevices();
        m_avfDevicesNeedRefresh = false;
    }

    if (m_avfDevices.empty())
    {
        ImGui::TextWrapped("No AVFoundation devices found.");
        if (ImGui::Button("Refresh##avfDevices"))
        {
            m_avfDevicesNeedRefresh = true;
        }
        return;
    }

    const std::string currentId = m_uiManager
        ? m_uiManager->getAVFoundationDeviceId()
        : std::string();

    std::vector<const char *> names;
    int currentIndex = -1;
    names.reserve(m_avfDevices.size());
    for (size_t i = 0; i < m_avfDevices.size(); ++i)
    {
        names.push_back(m_avfDevices[i].name.c_str());
        if (m_avfDevices[i].id == currentId)
        {
            currentIndex = static_cast<int>(i);
        }
    }
    if (currentIndex < 0) currentIndex = 0;

    if (ImGui::Combo("##avfDeviceCombo", &currentIndex,
                     names.data(), static_cast<int>(names.size())))
    {
        const auto &picked = m_avfDevices[currentIndex];
        if (m_uiManager)
        {
            m_uiManager->triggerDeviceChange(picked.id);
            m_uiManager->setAVFoundationDeviceId(picked.id);
            m_uiManager->saveConfig();
        }
        // Force a format-list refresh against the new device.
        m_avfFormatsNeedRefresh = true;
        m_avfFormatsForDeviceId.clear();
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh##avfDevices"))
    {
        m_avfDevicesNeedRefresh = true;
    }
}

void UIConfigurationSource::renderAVFoundationFormatSelection()
{
    ui_section_header("Format",
                      "Each AVFoundation device exposes a fixed set of "
                      "(resolution, FPS range, pixel format) tuples. "
                      "Picking a format applies all three atomically — "
                      "no separate resolution / FPS sliders, OBS-style.");

    if (!m_capture)
    {
        ImGui::TextWrapped("No capture instance available.");
        return;
    }

    const std::string deviceId = m_uiManager
        ? m_uiManager->getAVFoundationDeviceId()
        : std::string();

    if (m_avfFormatsNeedRefresh || m_avfFormatsForDeviceId != deviceId)
    {
        m_avfFormats = m_capture->listFormats(deviceId);
        m_avfFormatsForDeviceId = deviceId;
        m_avfFormatsNeedRefresh = false;
    }

    if (m_avfFormats.empty())
    {
        ImGui::TextWrapped("No formats available for the selected device.");
        if (ImGui::Button("Refresh##avfFormats"))
        {
            m_avfFormatsNeedRefresh = true;
        }
        return;
    }

    const std::string currentFormatId = m_uiManager
        ? m_uiManager->getAVFoundationFormatId()
        : std::string();

    std::vector<const char *> displayNames;
    int currentIndex = -1;
    displayNames.reserve(m_avfFormats.size());
    for (size_t i = 0; i < m_avfFormats.size(); ++i)
    {
        displayNames.push_back(m_avfFormats[i].displayName.c_str());
        if (m_avfFormats[i].id == currentFormatId)
        {
            currentIndex = static_cast<int>(i);
        }
    }
    if (currentIndex < 0) currentIndex = 0;

    if (ImGui::Combo("##avfFormatCombo", &currentIndex,
                     displayNames.data(), static_cast<int>(displayNames.size())))
    {
        const auto &picked = m_avfFormats[currentIndex];
        if (m_capture->setFormatById(picked.id, deviceId))
        {
            if (m_uiManager)
            {
                m_uiManager->setAVFoundationFormatId(picked.id);
                m_uiManager->saveConfig();
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh##avfFormats"))
    {
        m_avfFormatsNeedRefresh = true;
    }
}

#endif // __APPLE__
