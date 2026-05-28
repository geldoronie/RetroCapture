#pragma once

#include "../chat/ChatClient.h"
#include "../identity/ChatIdentity.h"
#include "../identity/OwnedRooms.h"

#include <string>
#include <vector>

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

private:
    // Each of the satellite windows (Profile, Rooms, Create, Join
    // Custom) is rendered BEFORE the chat panel's m_visible gate so
    // they can be opened / kept open independently of the chat
    // panel itself. Result: ticking "Chat" off in the View menu
    // only hides the chat panel, not the whole chat surface.
    void renderProfileWindow();
    void renderRoomsWindow();
    void renderCreateRoomWindow();
    void renderJoinCustomWindow();

public:
    // View-menu hooks — let UIManager surface separate menu items
    // per chat window so the user can show/hide each independently.
    bool isRoomsWindowVisible() const          { return m_showRoomsWindow; }
    void setRoomsWindowVisible(bool v)
    {
        m_showRoomsWindow = v;
        if (v)
        {
            // Match the QuickActions / header-button flow: lazy
            // refresh of the cached lists when the window opens.
            m_standaloneError.clear();
            m_roomsListRequested = false;
            m_ownedRoomsLoaded   = false;
        }
    }
    bool isProfileWindowVisible() const        { return m_showProfileWindow; }
    void setProfileWindowVisible(bool v);

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

    // Profile window state (#84). Mirrors ImGui::Begin's p_open so
    // the title-bar X closes it cleanly. Buffers live across frames
    // (and across opens within a session) so the user's typing isn't
    // dropped if they close + reopen the window. The cached identity
    // is what we last loaded from disk OR last persisted via Save.
    bool        m_showProfileWindow = false;
    char        m_profileNameBuf[120] = {0};
    char        m_profileNickBuf[64]  = {0};
    int         m_profileAge          = 0;
    ChatIdentity m_identity;
    bool        m_identityLoaded      = false;
    std::string m_profileError;
    std::string m_profileSavedHint;

    // Standalone-room window state (#84). Buffers live across frames
    // so what the user typed doesn't reset on every render; the bool
    // mirrors ImGui::Begin's p_open so the title-bar X closes it
    // cleanly.
    bool        m_showRoomsWindow   = false;
    // Separate windows for the create + join-custom flows (#84). The
    // main Rooms window stays uncluttered — just a tab bar with the
    // public + owned listings — and these sub-windows open on demand
    // via header buttons. Buffers live across frames so what the user
    // typed isn't lost if they switch away and back.
    bool        m_showCreateWindow     = false;
    bool        m_showJoinCustomWindow = false;
    // Participants panel toggle (#84). Defaults to visible — the
    // panel reads better with the sidebar showing who's in the room;
    // users who want a wider message column toggle it off via the
    // count chip in the header.
    bool        m_showParticipants  = true;
    char        m_joinSlugBuf[64]       = {0};
    char        m_joinPasswordBuf[64]   = {0};
    char        m_createTitleBuf[128]   = {0};
    char        m_createSlugBuf[64]     = {0};
    char        m_createPasswordBuf[64] = {0};
    bool        m_createListed          = true;
    std::string m_standaloneError;
    // Set true when a POST /rooms is in flight; disables the Create
    // button so we don't double-fire. Cleared by ChatClient's reply.
    bool        m_createInFlight        = false;
    // Cached room listing + refresh state (#84). Lazy-refreshed when
    // the Rooms window is opened.
    std::vector<ChatClient::ListedRoom> m_roomsList;
    std::string m_roomsListError;
    bool        m_roomsListRequested    = false;
    // Cached owned-rooms registry for the "Owned" tab (#84). Loaded
    // from disk on demand (when the window opens or after a create/
    // delete mutates the file). Sourced from ownedrooms::loadAll().
    std::vector<OwnedRoom> m_ownedRooms;
    bool        m_ownedRoomsLoaded      = false;
    std::string m_ownedRoomsError;
    // Two-step confirm for "delete forever". First click on the row's
    // Delete sets this to the room's slug and recolours the button to
    // a destructive red on the next frame; second click fires the
    // server-side DELETE + local registry erase. Any click elsewhere
    // clears it.
    std::string m_pendingDeleteSlug;
    std::string m_deleteError;

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
