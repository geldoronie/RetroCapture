#include "UIConfigurationWebPortal.h"
#include "UIManager.h"
#include <imgui.h>
#include <cstring>

UIConfigurationWebPortal::UIConfigurationWebPortal(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationWebPortal::~UIConfigurationWebPortal()
{
}

void UIConfigurationWebPortal::render()
{
    if (!m_uiManager)
    {
        return;
    }

    ImGui::Text("Web Portal");
    ImGui::Separator();
    ImGui::Spacing();

    renderWebPortalEnable();

    bool portalEnabled = m_uiManager->getWebPortalEnabled();
    if (!portalEnabled)
    {
        ImGui::Spacing();
        std::string streamUrl = "http://localhost:" + std::to_string(m_uiManager->getStreamingPort()) + "/stream";
        ImGui::Text("Stream direto: %s", streamUrl.c_str());
        return;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderStartStopButton();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderHTTPSSettings();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderCustomization();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderPortalURL();
}

void UIConfigurationWebPortal::renderWebPortalEnable()
{
    // Web Portal Enable/Disable (configuração)
    bool portalEnabled = m_uiManager->getWebPortalEnabled();
    if (ImGui::Checkbox("Habilitar Web Portal", &portalEnabled))
    {
        m_uiManager->triggerWebPortalEnabledChange(portalEnabled);
    }
}

void UIConfigurationWebPortal::renderStartStopButton()
{
    // Botão Start/Stop do Portal Web (independente do streaming)
    bool active = m_uiManager->getWebPortalActive();
    if (active)
    {
        if (ImGui::Button("Parar Portal Web", ImVec2(-1, 0)))
        {
            m_uiManager->triggerWebPortalStartStop(false);
        }
        ImGui::Spacing();
        std::string portalUrl = (m_uiManager->getWebPortalHTTPSEnabled() ? "https://" : "http://") +
                                std::string("localhost:") + std::to_string(m_uiManager->getStreamingPort());
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Portal Web Ativo");
        ImGui::Text("URL: %s", portalUrl.c_str());
    }
    else
    {
        if (ImGui::Button("Iniciar Portal Web", ImVec2(-1, 0)))
        {
            m_uiManager->triggerWebPortalStartStop(true);
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Portal Web Inativo");
    }
}

void UIConfigurationWebPortal::renderHTTPSSettings()
{
    // HTTPS Enable/Disable
    bool httpsEnabled = m_uiManager->getWebPortalHTTPSEnabled();
    if (ImGui::Checkbox("Habilitar HTTPS", &httpsEnabled))
    {
        m_uiManager->triggerWebPortalHTTPSChange(httpsEnabled);
    }

    if (httpsEnabled)
    {
        ImGui::Spacing();

        std::string certPath = m_uiManager->getFoundSSLCertificatePath();
        if (!certPath.empty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ HTTPS Ativo");
            ImGui::Text("Certificado: %s", certPath.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Certificado não encontrado");
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Configuração de Certificado"))
        {
            char certPathBuffer[512];
            strncpy(certPathBuffer, m_uiManager->getWebPortalSSLCertPath().c_str(), sizeof(certPathBuffer) - 1);
            certPathBuffer[sizeof(certPathBuffer) - 1] = '\0';
            ImGui::Text("Caminho do Certificado:");
            if (ImGui::InputText("##SSLCertPath", certPathBuffer, sizeof(certPathBuffer)))
            {
                m_uiManager->triggerWebPortalSSLCertPathChange(std::string(certPathBuffer));
            }

            char keyPathBuffer[512];
            strncpy(keyPathBuffer, m_uiManager->getWebPortalSSLKeyPath().c_str(), sizeof(keyPathBuffer) - 1);
            keyPathBuffer[sizeof(keyPathBuffer) - 1] = '\0';
            ImGui::Text("Caminho da Chave Privada:");
            if (ImGui::InputText("##SSLKeyPath", keyPathBuffer, sizeof(keyPathBuffer)))
            {
                m_uiManager->triggerWebPortalSSLKeyPathChange(std::string(keyPathBuffer));
            }
        }
    }
}

void UIConfigurationWebPortal::renderCustomization()
{
    // Personalização
    ImGui::Text("Personalização");
    ImGui::Separator();
    ImGui::Spacing();

    // Título
    char titleBuffer[256];
    strncpy(titleBuffer, m_uiManager->getWebPortalTitle().c_str(), sizeof(titleBuffer) - 1);
    titleBuffer[sizeof(titleBuffer) - 1] = '\0';
    ImGui::Text("Título:");
    if (ImGui::InputText("##WebPortalTitle", titleBuffer, sizeof(titleBuffer)))
    {
        m_uiManager->triggerWebPortalTitleChange(std::string(titleBuffer));
    }

    ImGui::Spacing();

    // Subtítulo
    char subtitleBuffer[256];
    strncpy(subtitleBuffer, m_uiManager->getWebPortalSubtitle().c_str(), sizeof(subtitleBuffer) - 1);
    subtitleBuffer[sizeof(subtitleBuffer) - 1] = '\0';
    ImGui::Text("Subtítulo:");
    if (ImGui::InputText("##WebPortalSubtitle", subtitleBuffer, sizeof(subtitleBuffer)))
    {
        m_uiManager->triggerWebPortalSubtitleChange(std::string(subtitleBuffer));
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
        strncpy(bgImagePathBuffer, m_uiManager->getWebPortalBackgroundImagePath().c_str(), sizeof(bgImagePathBuffer) - 1);
        bgImagePathBuffer[sizeof(bgImagePathBuffer) - 1] = '\0';
        ImGui::Text("Imagem de Fundo:");
        if (ImGui::InputText("##WebPortalBackgroundImagePath", bgImagePathBuffer, sizeof(bgImagePathBuffer)))
        {
            m_uiManager->triggerWebPortalBackgroundImagePathChange(std::string(bgImagePathBuffer));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Cores
        ImGui::Text("Cores:");
        ImGui::Spacing();

        bool colorsChanged = false;

        float *bgColor = m_uiManager->getWebPortalColorBackgroundEditable();
        if (ImGui::ColorEdit4("Fundo", bgColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *textColor = m_uiManager->getWebPortalColorTextEditable();
        if (ImGui::ColorEdit4("Texto", textColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *primaryColor = m_uiManager->getWebPortalColorPrimaryEditable();
        if (ImGui::ColorEdit4("Primária", primaryColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *primaryLightColor = m_uiManager->getWebPortalColorPrimaryLightEditable();
        if (ImGui::ColorEdit4("Primária Light", primaryLightColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *primaryDarkColor = m_uiManager->getWebPortalColorPrimaryDarkEditable();
        if (ImGui::ColorEdit4("Primária Dark", primaryDarkColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *secondaryColor = m_uiManager->getWebPortalColorSecondaryEditable();
        if (ImGui::ColorEdit4("Secundária", secondaryColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *secondaryHighlightColor = m_uiManager->getWebPortalColorSecondaryHighlightEditable();
        if (ImGui::ColorEdit4("Secundária Highlight", secondaryHighlightColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *cardHeaderColor = m_uiManager->getWebPortalColorCardHeaderEditable();
        if (ImGui::ColorEdit4("Cabeçalho", cardHeaderColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *borderColor = m_uiManager->getWebPortalColorBorderEditable();
        if (ImGui::ColorEdit4("Bordas", borderColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        ImGui::Spacing();

        float *successColor = m_uiManager->getWebPortalColorSuccessEditable();
        if (ImGui::ColorEdit4("Sucesso", successColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *warningColor = m_uiManager->getWebPortalColorWarningEditable();
        if (ImGui::ColorEdit4("Aviso", warningColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *dangerColor = m_uiManager->getWebPortalColorDangerEditable();
        if (ImGui::ColorEdit4("Erro", dangerColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        float *infoColor = m_uiManager->getWebPortalColorInfoEditable();
        if (ImGui::ColorEdit4("Info", infoColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        {
            colorsChanged = true;
        }

        if (colorsChanged)
        {
            m_uiManager->triggerWebPortalColorsChange();
        }

        ImGui::Spacing();
        if (ImGui::Button("Restaurar Cores Padrão"))
        {
            // Restaurar valores padrão do styleguide RetroCapture
            float *bg = m_uiManager->getWebPortalColorBackgroundEditable();
            bg[0] = 0.114f;
            bg[1] = 0.122f;
            bg[2] = 0.129f;
            bg[3] = 1.0f;

            float *text = m_uiManager->getWebPortalColorTextEditable();
            text[0] = 0.973f;
            text[1] = 0.973f;
            text[2] = 0.949f;
            text[3] = 1.0f;

            float *primary = m_uiManager->getWebPortalColorPrimaryEditable();
            primary[0] = 0.039f;
            primary[1] = 0.478f;
            primary[2] = 0.514f;
            primary[3] = 1.0f;

            float *primaryLight = m_uiManager->getWebPortalColorPrimaryLightEditable();
            primaryLight[0] = 0.435f;
            primaryLight[1] = 0.769f;
            primaryLight[2] = 0.753f;
            primaryLight[3] = 1.0f;

            float *primaryDark = m_uiManager->getWebPortalColorPrimaryDarkEditable();
            primaryDark[0] = 0.059f;
            primaryDark[1] = 0.243f;
            primaryDark[2] = 0.259f;
            primaryDark[3] = 1.0f;

            float *secondary = m_uiManager->getWebPortalColorSecondaryEditable();
            secondary[0] = 0.278f;
            secondary[1] = 0.702f;
            secondary[2] = 0.808f;
            secondary[3] = 1.0f;

            float *secondaryHighlight = m_uiManager->getWebPortalColorSecondaryHighlightEditable();
            secondaryHighlight[0] = 0.788f;
            secondaryHighlight[1] = 0.949f;
            secondaryHighlight[2] = 0.906f;
            secondaryHighlight[3] = 1.0f;

            float *cardHeader = m_uiManager->getWebPortalColorCardHeaderEditable();
            cardHeader[0] = 0.059f;
            cardHeader[1] = 0.243f;
            cardHeader[2] = 0.259f;
            cardHeader[3] = 1.0f;

            float *border = m_uiManager->getWebPortalColorBorderEditable();
            border[0] = 0.039f;
            border[1] = 0.478f;
            border[2] = 0.514f;
            border[3] = 0.5f;

            float *success = m_uiManager->getWebPortalColorSuccessEditable();
            success[0] = 0.271f;
            success[1] = 0.839f;
            success[2] = 0.643f;
            success[3] = 1.0f;

            float *warning = m_uiManager->getWebPortalColorWarningEditable();
            warning[0] = 0.953f;
            warning[1] = 0.788f;
            warning[2] = 0.243f;
            warning[3] = 1.0f;

            float *danger = m_uiManager->getWebPortalColorDangerEditable();
            danger[0] = 0.851f;
            danger[1] = 0.325f;
            danger[2] = 0.310f;
            danger[3] = 1.0f;

            float *info = m_uiManager->getWebPortalColorInfoEditable();
            info[0] = 0.298f;
            info[1] = 0.737f;
            info[2] = 0.902f;
            info[3] = 1.0f;

            m_uiManager->triggerWebPortalColorsChange();
        }
    }
}

void UIConfigurationWebPortal::renderPortalURL()
{
    // Portal URL
    bool httpsEnabled = m_uiManager->getWebPortalHTTPSEnabled();
    std::string protocol = httpsEnabled ? "https" : "http";
    std::string portalUrl = protocol + "://localhost:" + std::to_string(m_uiManager->getStreamingPort());
    ImGui::Text("URL: %s", portalUrl.c_str());
}
