#include "UICredits.h"
#include "UIManager.h"
#include "../utils/Logger.h"
#include <imgui.h>

#ifdef PLATFORM_LINUX
#include <cstdlib>
#elif defined(_WIN32)
#include <cstdlib>
#endif

UICredits::UICredits(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UICredits::~UICredits()
{
}

void UICredits::setVisible(bool visible)
{
    m_visible = visible;
}

void UICredits::render()
{
    if (!m_visible)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Credits", &m_visible))
    {
        ImGui::TextWrapped("RetroCapture v0.3.0");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Autor
        ImGui::Text("Author:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Geldo Ronie");
        ImGui::Spacing();

        // Email
        ImGui::Text("Email:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "geldoronie@gmail.com");
        ImGui::Spacing();

        // GitHub
        ImGui::Text("GitHub:");
        ImGui::SameLine();
        if (ImGui::Button("https://github.com/geldoronie/RetroCapture"))
        {
            // Abrir URL no navegador
#ifdef PLATFORM_LINUX
            system("xdg-open https://github.com/geldoronie/RetroCapture &");
#elif defined(_WIN32)
            system("start https://github.com/geldoronie/RetroCapture");
#endif
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Agradecimentos
        ImGui::Text("Special Thanks:");
        ImGui::Spacing();
        ImGui::BulletText("RetroArch");
        ImGui::Indent();
        ImGui::TextWrapped("For the amazing shader system and GLSL shader presets that make this project possible.");
        ImGui::Unindent();
        ImGui::Spacing();

        // Bibliotecas
        ImGui::Text("Libraries Used:");
        ImGui::Spacing();

        ImGui::BulletText("ImGui");
        ImGui::Indent();
        ImGui::TextWrapped("Immediate mode GUI library for the user interface.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("GLFW");
        ImGui::Indent();
        ImGui::TextWrapped("Window and OpenGL context management.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("FFmpeg");
        ImGui::Indent();
        ImGui::TextWrapped("Video and audio encoding (libavcodec, libavformat, libavutil, libswscale, libswresample).");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("OpenGL");
        ImGui::Indent();
        ImGui::TextWrapped("Graphics rendering and shader execution.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("libpng");
        ImGui::Indent();
        ImGui::TextWrapped("PNG image loading for textures and assets.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("nlohmann/json");
        ImGui::Indent();
        ImGui::TextWrapped("JSON parsing for configuration persistence.");
        ImGui::Unindent();
        ImGui::Spacing();

#ifdef PLATFORM_LINUX
        ImGui::BulletText("V4L2");
        ImGui::Indent();
        ImGui::TextWrapped("Video4Linux2 API for video capture on Linux.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("PulseAudio");
        ImGui::Indent();
        ImGui::TextWrapped("Audio capture from system on Linux.");
        ImGui::Unindent();
        ImGui::Spacing();
#elif defined(_WIN32)
        ImGui::BulletText("DirectShow");
        ImGui::Indent();
        ImGui::TextWrapped("Video capture API for Windows.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::BulletText("WASAPI");
        ImGui::Indent();
        ImGui::TextWrapped("Windows Audio Session API for audio capture.");
        ImGui::Unindent();
        ImGui::Spacing();
#endif

        ImGui::BulletText("OpenSSL");
        ImGui::Indent();
        ImGui::TextWrapped("SSL/TLS support for HTTPS in the web portal.");
        ImGui::Unindent();
        ImGui::Spacing();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Licença
        ImGui::Text("License:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "MIT License");
        ImGui::Spacing();

        if (ImGui::Button("Close"))
        {
            m_visible = false;
        }
    }
    ImGui::End();
}
