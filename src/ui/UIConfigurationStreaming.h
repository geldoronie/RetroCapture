#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declarations
class UIManager;

/**
 * Aba Streaming da janela de configuração
 */
class UIConfigurationStreaming
{
public:
    UIConfigurationStreaming(UIManager *uiManager);
    ~UIConfigurationStreaming();

    void render();

private:
    UIManager *m_uiManager = nullptr;

    void renderStreamingStatus();
    void renderBasicSettings();
    void renderCodecSettings();
    void renderBitrateSettings();
    void renderAdvancedBufferSettings();
    void renderStartStopButton();

    // Codec-specific settings
    void renderH264Settings();
    void renderH265Settings();
    void renderVP8Settings();
    void renderVP9Settings();
};
