#include "UIConfigurationShader.h"
#include "UIManager.h"
#include "../shader/ShaderEngine.h"
#include "../utils/FilesystemCompat.h"
#include "../utils/Paths.h"
#include <imgui.h>
#include <cstring>

UIConfigurationShader::UIConfigurationShader(UIManager *uiManager)
    : m_uiManager(uiManager)
{
    if (uiManager)
    {
        m_shaderEngine = uiManager->getShaderEngine();
    }
}

UIConfigurationShader::~UIConfigurationShader()
{
}

void UIConfigurationShader::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Shaders", &m_visible))
    {
        ImGui::End();
        return;
    }

    m_shaderEngine = m_uiManager->getShaderEngine();

    {
        bool enabled = m_uiManager->getShaderPipelineEnabled();
        if (ImGui::Checkbox("Apply shader pipeline", &enabled))
        {
            m_uiManager->setShaderPipelineEnabled(enabled);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Bypass the shader chain without losing the selected preset / parameters.\n"
                              "Lets you A/B compare the effect on/off in real time.");
        }
        ImGui::Separator();
    }

    renderShaderSelection();
    ImGui::Separator();
    renderSavePreset();
    ImGui::Separator();
    renderShaderParameters();

    ImGui::End();
}

void UIConfigurationShader::renderShaderSelection()
{
    ImGui::Text("Shader Preset:");

    // Combo box for shader selection + Rescan button on the same row.
    // The Rescan action used to live in the File menu, but it's only
    // meaningful in the context of this list — moved here so the user
    // doesn't have to leave the Shaders tab to refresh after dropping
    // a new .glslp into the shaders folder.
    std::string currentShader = m_uiManager->getCurrentShader();
    float buttonWidth = ImGui::CalcTextSize("Rescan").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##shader", currentShader.empty() ? "None" : currentShader.c_str()))
    {
        if (ImGui::Selectable("None", currentShader.empty()))
        {
            m_uiManager->setCurrentShader("");
            m_uiManager->saveConfig();
        }

        const auto &shaders = m_uiManager->getScannedShaders();
        for (size_t i = 0; i < shaders.size(); ++i)
        {
            bool isSelected = (currentShader == shaders[i]);
            if (ImGui::Selectable(shaders[i].c_str(), isSelected))
            {
                m_uiManager->setCurrentShader(shaders[i]);
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
    if (ImGui::Button("Rescan"))
    {
        m_uiManager->scanShaders("shaders/shaders_glsl");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Re-scan the shaders folder for newly added .glslp presets.");
    }

    ImGui::Separator();
    ImGui::Text("Shaders found: %zu", m_uiManager->getScannedShaders().size());
}

void UIConfigurationShader::renderSavePreset()
{
    if (!m_shaderEngine || !m_shaderEngine->isShaderActive())
    {
        return;
    }

    ImGui::Text("Save Preset:");

    std::string currentPreset = m_shaderEngine->getPresetPath();
    if (!currentPreset.empty())
    {
        // Extrair apenas o nome do arquivo
        fs::path presetPath(currentPreset);
        std::string fileName = presetPath.filename();

        if (ImGui::Button("Save"))
        {
            // Salvar por cima do arquivo atual
            const auto &onSavePreset = m_uiManager->getOnSavePreset();
            if (onSavePreset)
            {
                onSavePreset(currentPreset, true);
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
            const auto &onSavePreset = m_uiManager->getOnSavePreset();
            if (onSavePreset && strlen(m_savePresetPath) > 0)
            {
                // Construir caminho completo
                fs::path basePath = fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "shaders_glsl";
                fs::path newPath = basePath / m_savePresetPath;
                // Garantir extensão .glslp
                if (newPath.extension() != ".glslp")
                {
                    newPath.replace_extension(".glslp");
                }
                onSavePreset(newPath.string(), false);
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

void UIConfigurationShader::renderShaderParameters()
{
    // Sempre atualizar referência ao shader engine antes de usar
    if (m_uiManager)
    {
        m_shaderEngine = m_uiManager->getShaderEngine();
    }

    if (!m_shaderEngine)
    {
        ImGui::TextDisabled("Shader engine unavailable");
        return;
    }

    if (!m_shaderEngine->isShaderActive())
    {
        ImGui::TextDisabled("No active shader");
        return;
    }

    ImGui::Text("Shader Parameters:");

    auto params = m_shaderEngine->getShaderParameters();

    if (params.empty())
    {
        ImGui::TextDisabled("No parameters available");
        if (m_shaderEngine->isShaderActive())
        {
            ImGui::TextDisabled("(Shader active but exposes no parameters)");
        }
    }
    else
    {
        // Log quando encontramos parâmetros (apenas uma vez)
        static std::string lastShaderPath;
        std::string currentShaderPath = m_shaderEngine->getPresetPath();
        if (currentShaderPath != lastShaderPath)
        {
            lastShaderPath = currentShaderPath;
            // Não temos acesso direto ao Logger aqui, mas podemos mostrar na UI
            ImGui::Text("Found %zu parameter(s)", params.size());
        }

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
