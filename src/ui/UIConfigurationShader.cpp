#include "UIConfigurationShader.h"
#include "UIManager.h"
#include "../shader/ShaderEngine.h"
#include <imgui.h>
#include <filesystem>
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
    if (!m_uiManager)
    {
        return;
    }

    // Atualizar referência ao shader engine se necessário
    m_shaderEngine = m_uiManager->getShaderEngine();

    renderShaderSelection();
    ImGui::Separator();
    renderSavePreset();
    ImGui::Separator();
    renderShaderParameters();
}

void UIConfigurationShader::renderShaderSelection()
{
    ImGui::Text("Shader Preset:");

    // Combo box for shader selection
    std::string currentShader = m_uiManager->getCurrentShader();
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
        std::filesystem::path presetPath(currentPreset);
        std::string fileName = presetPath.filename().string();

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
                std::filesystem::path basePath("shaders/shaders_glsl");
                std::filesystem::path newPath = basePath / m_savePresetPath;
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
    if (!m_shaderEngine || !m_shaderEngine->isShaderActive())
    {
        return;
    }

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
