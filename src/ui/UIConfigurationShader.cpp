#include "UIConfigurationShader.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../shader/ShaderEngine.h"
#include "../utils/FilesystemCompat.h"
#include "../utils/Paths.h"
#include "../utils/TranslationManager.h"
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
    if (!ImGui::Begin(T("shader.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    m_shaderEngine = m_uiManager->getShaderEngine();

    {
        ui_section_header("Pipeline",
                          "Toggle the shader chain on or off without "
                          "losing the selected preset or its tuning.");
        bool enabled = m_uiManager->getShaderPipelineEnabled();
        if (ImGui::Checkbox(T("shader.apply_pipeline").c_str(), &enabled))
        {
            m_uiManager->setShaderPipelineEnabled(enabled);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", T("shader.apply_pipeline.tip").c_str());
        }
    }

    renderShaderSelection();
    renderSavePreset();
    renderShaderParameters();

    ImGui::End();
}

void UIConfigurationShader::renderShaderSelection()
{
    ui_section_header("Preset",
                      "Pick a .glslp shader preset from the scan path. "
                      "Click Rescan to pick up newly dropped files.");

    std::string currentShader = m_uiManager->getCurrentShader();
    const std::string rescanLabel = T("shader.rescan");
    float buttonWidth = ImGui::CalcTextSize(rescanLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    const std::string noneLabel = T("shader.none");
    if (ImGui::BeginCombo("##shader", currentShader.empty() ? noneLabel.c_str() : currentShader.c_str()))
    {
        if (ImGui::Selectable(noneLabel.c_str(), currentShader.empty()))
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
    if (ImGui::Button(rescanLabel.c_str()))
    {
        m_uiManager->scanShaders("shaders/shaders_glsl");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("shader.rescan.tip").c_str());
    }

    ImGui::TextDisabled("%s: %zu", T("shader.shaders_found").c_str(), m_uiManager->getScannedShaders().size());
}

void UIConfigurationShader::renderSavePreset()
{
    if (!m_shaderEngine || !m_shaderEngine->isShaderActive())
    {
        return;
    }

    ui_section_header("Save preset",
                      "Overwrite the loaded .glslp with current "
                      "parameters, or write a new one alongside it.");

    std::string currentPreset = m_shaderEngine->getPresetPath();
    if (!currentPreset.empty())
    {
        fs::path presetPath(currentPreset);
        std::string fileName = presetPath.filename();

        if (ImGui::Button(T("shader.save").c_str()))
        {
            const auto &onSavePreset = m_uiManager->getOnSavePreset();
            if (onSavePreset)
            {
                onSavePreset(currentPreset, true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("shader.save_as").c_str()))
        {
            strncpy(m_savePresetPath, fileName.c_str(), sizeof(m_savePresetPath) - 1);
            m_savePresetPath[sizeof(m_savePresetPath) - 1] = '\0';
            m_showSaveDialog = true;
        }
    }
    else
    {
        ImGui::TextDisabled("%s", T("shader.no_preset_loaded").c_str());
    }

    if (m_showSaveDialog)
    {
        ImGui::OpenPopup(T("shader.save_as_dialog").c_str());
        m_showSaveDialog = false;
    }

    if (ImGui::BeginPopupModal(T("shader.save_as_dialog").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", T("shader.enter_filename").c_str());
        ImGui::InputText("##presetname", m_savePresetPath, sizeof(m_savePresetPath));

        if (ImGui::Button(T("shader.save").c_str()))
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
        if (ImGui::Button(T("shader.cancel").c_str()))
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
        ImGui::TextDisabled("%s", T("shader.engine_unavailable").c_str());
        return;
    }

    if (!m_shaderEngine->isShaderActive())
    {
        ImGui::TextDisabled("%s", T("shader.no_active").c_str());
        return;
    }

    ui_section_header("Parameters",
                      "Knobs the active preset exposes — values persist "
                      "when you Save the preset.");

    auto params = m_shaderEngine->getShaderParameters();

    if (params.empty())
    {
        ImGui::TextDisabled("%s", T("shader.no_parameters").c_str());
        if (m_shaderEngine->isShaderActive())
        {
            ImGui::TextDisabled("%s", T("shader.active_no_params").c_str());
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
