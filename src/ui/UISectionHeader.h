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
 * Coloured status bullet drawn via ImDrawList primitives.
 *
 * Why not a text glyph: Dear ImGui's built-in Proggy Clean font
 * ships only ASCII + Latin-1 glyph data, so `●` (U+25CF) renders
 * as the missing-glyph fallback ('?') even if we extend the font's
 * glyph range. Drawing a filled circle directly through the
 * window draw list sidesteps the font question entirely and gives
 * us colour control besides.
 *
 * Reserves layout space equal to one text line tall, so callers
 * can `SameLine()` after it just like they would after a real
 * `ImGui::Text("●")`.
 */
inline void ui_status_bullet(const ImVec4 &color)
{
    const float lineH  = ImGui::GetTextLineHeight();
    const float radius = lineH * 0.32f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 center(cursor.x + radius, cursor.y + lineH * 0.5f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, radius,
                        ImGui::ColorConvertFloat4ToU32(color), 16);

    // Reserve layout space — diameter wide, one line tall. The
    // tiny extra padding on the right (4 px) prevents the next
    // SameLine'd widget from touching the circle.
    ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, lineH));
}

/**
 * Inline status indicator used inside renderXxxStatus() blocks.
 * Renders as `Status: <label> <bullet>` where the bullet's colour
 * follows the active flag. Green = active / healthy, red = stopped
 * / inactive — same convention as the connection-status overlay.
 *
 * Usage:
 *   ui_section_header("Video Recording");
 *   ui_status_indicator(active, "Recording", "Stopped");
 */
inline void ui_status_indicator(bool active, const char *activeLabel, const char *inactiveLabel)
{
    ImGui::Text("Status: %s", active ? activeLabel : inactiveLabel);
    ImGui::SameLine();
    ui_status_bullet(active ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                            : ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
}
