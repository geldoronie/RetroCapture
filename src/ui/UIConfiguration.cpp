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
        if (ImGui::BeginTabItem("Shaders"))
        {
            shaderTab.render();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Image"))
        {
            imageTab.render();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Source"))
        {
            sourceTab.render();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Info"))
        {
            infoTab.render();
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

        ImGui::EndTabBar();
    }

    ImGui::End();
}
