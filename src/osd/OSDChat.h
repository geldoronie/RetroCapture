#pragma once

#include <string>

class ChatClient;
class UIManager;

/**
 * Chat panel — OSD layer (#84).
 *
 * Pinned to the top-right of the main viewport, alpha-blended over the
 * capture surface. Like the other widgets in `src/osd/`, this overlay
 * renders BEFORE UIManager's m_uiVisible gate, so the panel stays on
 * screen when the user pressed F12 to clear the configuration windows
 * — streamers want chat visible while their viewport is otherwise
 * clean.
 *
 * Backed by `ChatClient` for transport. The overlay reads a snapshot
 * once per frame and pushes user input straight back into the client;
 * no per-message state lives here.
 *
 * Visibility:
 *   - Toggle via View → "Chat" or the F8 hotkey.
 *   - Persisted on UIManager so the choice survives across launches.
 *   - Auto-hides itself when chat is Idle (no streamId published /
 *     subscribed) — there's nothing to show, and the empty panel
 *     would just be visual noise.
 */
class OSDChat
{
public:
    OSDChat(UIManager *uiManager, ChatClient *chat);
    ~OSDChat();

    void render();

    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }
    void toggleVisibility()       { m_visible = !m_visible; }

    // True while the input text box has keyboard focus. The hotkey
    // handler reads this to suppress global shortcut routing while the
    // user is typing (otherwise pressing 'r' to type "rgb" would
    // trigger any rebinding watcher that fires on 'r').
    bool isInputFocused() const   { return m_inputFocused; }

private:
    UIManager  *m_uiManager   = nullptr;
    ChatClient *m_chat        = nullptr;
    bool        m_visible     = true;
    bool        m_inputFocused = false;

    // Sticky-bottom autoscroll. Cleared if the user scrolls up so new
    // messages don't yank them back; re-armed when they scroll all
    // the way back down.
    bool        m_autoScroll  = true;

    // Cached input buffer — outlives a single ImGui frame so the
    // text persists across renders. 800 chars + null matches the
    // server-side cap.
    char        m_inputBuf[801] = {0};

    // Edit buffer for the nickname row. Same lifetime semantics.
    char        m_nickBuf[64]  = {0};
    bool        m_nickInitialized = false;

    // Standalone-room window state (#84). Buffers live across frames
    // so what the user typed doesn't reset on every render; the bool
    // mirrors ImGui::Begin's p_open so the title-bar X closes it
    // cleanly.
    bool        m_showRoomsWindow   = false;
    // Participants panel toggle (#84). When true, a small list is
    // rendered between the header and the message log showing
    // everyone in the room with their host marker.
    bool        m_showParticipants  = false;
    char        m_joinSlugBuf[64]   = {0};
    char        m_createTitleBuf[128] = {0};
    char        m_createSlugBuf[64] = {0};
    std::string m_standaloneError;
    // Set true when a POST /rooms is in flight; disables the Create
    // button so we don't double-fire. Cleared by ChatClient's reply.
    bool        m_createInFlight = false;

    // #84 — Transient inline validation feedback for the nickname
    // Apply button. Cleared next frame when the user resumes typing
    // or successfully reconnects.
    std::string m_nickError;

    // InputTextMultiline callback shared state. Must live across
    // frames AND be in scope of the message loop (which writes
    // pendingInsert on a nick double-click). Hence a member, not
    // a function-local static.
    //   - pendingPost: body captured by CallbackEdit when it found
    //     a '\n' in the buffer; posted right after InputText
    //     returns, then cleared.
    //   - pendingInsert: text the double-click handler wants
    //     injected at the cursor (`@<nick> `). Consumed by the
    //     CallbackAlways path, then cleared.
    struct InputEditCb {
        std::string pendingPost;
        std::string pendingInsert;
    };
    InputEditCb m_inputCb;
};
