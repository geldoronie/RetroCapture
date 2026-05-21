#pragma once

#include <string>
#include <vector>

// Forward declarations
class UIManager;
class ShaderEngine;

/**
 * Aba Shaders da janela de configuração
 */
class UIConfigurationShader
{
public:
    UIConfigurationShader(UIManager *uiManager);
    ~UIConfigurationShader();

    void render();

private:
    UIManager *m_uiManager = nullptr;
    ShaderEngine *m_shaderEngine = nullptr;

    // Save preset dialog state
    char m_savePresetPath[512] = "";
    bool m_showSaveDialog = false;

    void renderShaderSelection();
    void renderShaderParameters();
    void renderSavePreset();
};
