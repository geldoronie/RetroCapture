#pragma once

class UIManager;

/**
 * Quick-actions OSD widget (#68).
 *
 * Lives in the OSD layer (`src/osd/`) — pinned to the bottom-right
 * of the main viewport every frame, no title bar, no drag, no resize,
 * no imgui.ini persistence. Distinguished from the UI layer
 * (`src/ui/`) which owns interactive windows the user opens / closes /
 * moves at will.
 *
 * What it surfaces:
 *   - Start / Stop Streaming                  (mirrors UIConfigurationStreaming)
 *   - Start / Stop Recording                  (mirrors UIConfigurationRecording)
 *   - Connected viewers count                 (when streaming is active)
 *   - Recording duration HH:MM:SS             (when recording is active)
 *
 * Re-entrancy with the full Configuration windows is guaranteed by
 * routing every click through the same UIManager::triggerXxxStartStop
 * paths those windows call. Reading the same processing / cooldown
 * flags keeps the button label tracking byte-for-byte (Iniciando…,
 * Aguardando (Ns), etc.).
 *
 * In client mode (consuming a remote /raw), the streaming button is
 * hidden — there's nothing local to broadcast. Recording stays
 * available since you can record the consumed feed locally.
 *
 * Visibility:
 *   - Default on so first-time users discover it.
 *   - Toggle via View → "Quick actions"; UIManager persists the
 *     choice to config.json across runs.
 */
class QuickActionsOverlay
{
public:
    QuickActionsOverlay(UIManager *uiManager);
    ~QuickActionsOverlay();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

    // Height of the overlay on its last frame, used by the
    // connection-status overlay to push itself above when both are
    // on screen at once. Returns 0 if the overlay hasn't rendered or
    // isn't currently visible.
    float renderedHeight() const  { return m_lastRenderedHeight; }

private:
    UIManager *m_uiManager = nullptr;
    bool       m_visible = true;
    float      m_lastRenderedHeight = 0.0f;

    // Auto-hide bookkeeping. m_lastActivityTime is an ImGui::GetTime()
    // (seconds since start, monotonic) stamp of the last mouse movement
    // / click / hover; the overlay holds full opacity for a few seconds
    // after it, then fades out. m_activityPrimed defers the very first
    // stamp to the first rendered frame so the overlay doesn't start the
    // idle clock from time 0 and hide before the user touches anything.
    double m_lastActivityTime = 0.0;
    bool   m_activityPrimed   = false;
};
