#pragma once

#include <imgui.h>

/**
 * Shared visual idiom for section headers inside Configuration
 * windows (Source, Shader, Image, Streaming, Recording, Web Portal,
 * Audio).
 *
 * Why a shared helper:
 *   The previous pattern was each window using a plain
 *   `ImGui::Text("Title") + ImGui::Separator()` for every section,
 *   which read as a dense wall of identical-looking labels with no
 *   visual hierarchy. Switching everyone to a coloured title + an
 *   optional one-paragraph blurb gives unfamiliar fields context
 *   without forcing the user to hover for tooltips, and gives
 *   familiar fields a clear scanning anchor.
 *
 *   Keeping the rendering in one inline function means: (a) every
 *   Configuration window looks identical across releases — change
 *   the colour or spacing here and every window updates; (b) a
 *   future "compact mode" or "verbose mode" toggle can swap the
 *   single implementation without touching call sites.
 *
 * Usage:
 *   ui_section_header("Profiles",
 *                     "Saved configurations you can swap between.");
 *   // ... section body ...
 *
 *   ui_section_header("Output");          // title only, no blurb
 *   // ... section body ...
 *
 * The leading Spacing() handles separation from the previous
 * section, so call sites do NOT need to insert an explicit
 * Separator() / Spacing() before invoking this.
 */
inline void ui_section_header(const char *title, const char *blurb = nullptr)
{
    ImGui::Spacing();
    // Same cyan that UIConfigurationStreaming::renderDirectoryPublish
    // has used for its title since #49 Phase 2 landed.
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", title);
    if (blurb && *blurb)
    {
        ImGui::TextWrapped("%s", blurb);
    }
    ImGui::Spacing();
}

/**
 * Inline status indicator used inside renderXxxStatus() blocks. A
 * coloured filled bullet next to a "Status: ..." line at the top of
 * the window. Same colour conventions everywhere: green = active /
 * healthy, red = stopped / inactive, grey = unavailable / disabled.
 *
 * Usage:
 *   ui_section_header("Video Recording");
 *   ui_status_indicator(active, "Recording", "Stopped");
 */
inline void ui_status_indicator(bool active, const char *activeLabel, const char *inactiveLabel)
{
    ImGui::Text("Status: %s", active ? activeLabel : inactiveLabel);
    ImGui::SameLine();
    if (active)
    {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }
}
