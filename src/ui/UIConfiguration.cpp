#include "UIConfiguration.h"
#include "UIManager.h"
#include "UIConfigurationSource.h"
#include "UIConfigurationShader.h"
#include "UIConfigurationImage.h"
#include "UIConfigurationStreaming.h"
#include "UIConfigurationRecording.h"
#include "UIConfigurationWebPortal.h"
#include "UIConfigurationAudio.h"
#include "UIInfoPanel.h"
#include "../utils/Logger.h"
#include <imgui.h>

UIConfiguration::UIConfiguration(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfiguration::~UIConfiguration()
{
}

void UIConfiguration::setVisible(bool visible)
{
    if (visible && !m_visible)
    {
        m_justOpened = true; // Marcar como recém-aberta quando mudar de false para true
    }
    m_visible = visible;
}

void UIConfiguration::render()
{
    if (!m_visible)
    {
        return;
    }

    // Aplicar posição e tamanho inicial apenas quando a janela é aberta
    if (m_justOpened)
    {
        // Obter altura do menu bar para posicionar a janela abaixo dele
        float menuBarHeight = ImGui::GetFrameHeight();

        // Obter dimensões da viewport
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;

        // Definir posição inicial: um pouco abaixo do menu bar
        ImVec2 initialPos(workPos.x + 10.0f, workPos.y + menuBarHeight + 10.0f);

        // Definir tamanho inicial menor que 640x480 (usar 600x400 para caber em resoluções menores)
        ImVec2 initialSize(600.0f, 400.0f);

        // Configurar posição e tamanho inicial (ignora o que está salvo no .ini)
        ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);

        m_justOpened = false;
    }

    // Janela flutuante redimensionável
    // Usar ImGuiWindowFlags_NoSavedSettings para não salvar posição/tamanho no .ini
    ImGui::Begin("RetroCapture Controls", &m_visible,
                 ImGuiWindowFlags_NoSavedSettings);

    // Phase 5 of #47: when this RetroCapture is acting as a remote viewer,
    // the only configuration that makes sense for the user to touch is the
    // Info panel (read-only inspection of the mirrored host state) and
    // Shaders for completeness — but Source/Streaming/Recording/Web Portal/
    // Audio belong to a producer role and don't apply to a viewer. Rather
    // than show them disabled (visually noisy, hints at functionality the
    // user can't have), we hide the entire tabs while connected. Disconnect
    // brings them back. A banner explains the mode so the user isn't
    // confused why the rest disappeared.
    const bool remote = m_uiManager && m_uiManager->isRemoteSource() &&
                        !m_uiManager->getCurrentDevice().empty();
    if (remote)
    {
        const ImVec4 warningBg(0.45f, 0.12f, 0.12f, 0.85f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, warningBg);
        ImGui::BeginChild("RemoteBanner", ImVec2(0, ImGui::GetFrameHeight() * 1.2f),
                          false, ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("  REMOTE VIEWER MODE — viewing a host stream. Disconnect from the 'Remote' menu to manage local capture.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Criar instâncias das abas (podem ser membros da classe se necessário)
    static UIConfigurationSource sourceTab(m_uiManager);
    static UIConfigurationShader shaderTab(m_uiManager);
    static UIConfigurationImage imageTab(m_uiManager);
    static UIConfigurationStreaming streamingTab(m_uiManager);
    static UIConfigurationRecording recordingTab(m_uiManager);
    static UIConfigurationWebPortal webPortalTab(m_uiManager);
    static UIConfigurationAudio audioTab(m_uiManager);
    static UIInfoPanel infoTab(m_uiManager);

    // Tabs
    if (ImGui::BeginTabBar("MainTabs"))
    {
        // Info is always available — it's a read-only inspection panel that
        // applies to both local capture and remote viewing.
        if (ImGui::BeginTabItem("Info"))
        {
            infoTab.render();
            ImGui::EndTabItem();
        }

        // Shaders stays visible in remote mode — the host's preset is
        // mirrored via /meta and the user may want to inspect what's
        // running. Disabled wrapper kept so they can't edit the values
        // (those are owned by the host).
        if (ImGui::BeginTabItem("Shaders"))
        {
            ImGui::BeginDisabled(remote);
            shaderTab.render();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        // Image stays visible in remote mode too — brightness, contrast,
        // aspect ratio, fullscreen and output resolution are display-side
        // choices that make sense for a viewer to override. The server
        // seeds defaults via /meta on the initial connect; after that
        // the user is free to tweak.
        if (ImGui::BeginTabItem("Image"))
        {
            imageTab.render();
            ImGui::EndTabItem();
        }

        // Everything below this point is producer-side configuration and
        // is hidden while the local instance is acting as a remote viewer.
        if (!remote)
        {
            if (ImGui::BeginTabItem("Source"))
            {
                sourceTab.render();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Streaming"))
            {
                streamingTab.render();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Recording"))
            {
                recordingTab.render();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Web Portal"))
            {
                webPortalTab.render();
                ImGui::EndTabItem();
            }

#ifdef __linux__
            if (ImGui::BeginTabItem("Audio"))
            {
                audioTab.render();
                ImGui::EndTabItem();
            }
#endif
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
