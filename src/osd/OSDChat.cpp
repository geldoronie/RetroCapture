#include "OSDChat.h"

#include "../chat/ChatClient.h"
#include "../identity/OwnedRooms.h"
#include "../ui/UIManager.h"
#include "../ui/UISectionHeader.h" // ui_status_bullet — emoji-safe dot

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace
{
// HH:MM in local time. Posted-at is in epoch ms; the OSD doesn't
// need second precision and showing only HH:MM keeps the line tight.
std::string formatTime(int64_t epochMs)
{
    if (epochMs <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(epochMs / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

const char *stateLabel(ChatClient::State s)
{
    switch (s)
    {
        case ChatClient::State::Idle:         return "idle";
        case ChatClient::State::Resolving:    return "resolving room";
        case ChatClient::State::Connecting:   return "connecting";
        case ChatClient::State::Connected:    return "connected";
        case ChatClient::State::Reconnecting: return "reconnecting";
        case ChatClient::State::Error:        return "error";
    }
    return "?";
}

// True when `body` contains `@nick` as a standalone token (case-
// insensitive, with word-boundary punctuation OK). Used to draw the
// reader's attention to messages calling them out — same as the web
// portal's mentionsMe in chat.js.
bool mentionsNick(const std::string &body, const std::string &nick)
{
    if (body.empty() || nick.empty()) return false;
    const size_t nlen = nick.size();
    for (size_t i = 0; i + 1 < body.size(); ++i)
    {
        if (body[i] != '@') continue;
        // boundary before '@' — start-of-string or non-word char.
        if (i > 0)
        {
            const unsigned char prev = static_cast<unsigned char>(body[i - 1]);
            if (std::isalnum(prev) || prev == '_') continue;
        }
        if (i + 1 + nlen > body.size()) continue;
        bool match = true;
        for (size_t k = 0; k < nlen; ++k)
        {
            const char a = body[i + 1 + k];
            const char b = nick[k];
            if (std::tolower(static_cast<unsigned char>(a)) !=
                std::tolower(static_cast<unsigned char>(b)))
            {
                match = false;
                break;
            }
        }
        if (!match) continue;
        // boundary after the nick
        const size_t after = i + 1 + nlen;
        if (after < body.size())
        {
            const unsigned char next = static_cast<unsigned char>(body[after]);
            if (std::isalnum(next) || next == '_') continue;
        }
        return true;
    }
    return false;
}

ImVec4 stateColor(ChatClient::State s)
{
    switch (s)
    {
        case ChatClient::State::Connected:    return ImVec4(0.40f, 0.80f, 0.40f, 1.0f);
        case ChatClient::State::Reconnecting: return ImVec4(0.95f, 0.70f, 0.30f, 1.0f);
        case ChatClient::State::Error:        return ImVec4(0.85f, 0.33f, 0.31f, 1.0f);
        default:                              return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    }
}
}

OSDChat::OSDChat(UIManager *uiManager, ChatClient *chat)
    : m_uiManager(uiManager)
    , m_chat(chat)
{
}

OSDChat::~OSDChat() = default;

void OSDChat::render()
{
    m_inputFocused = false;
    if (!m_chat) return;

    // #84 — External "open Profile" requests (e.g. from the Streaming
    // settings' "Configure Profile" button). Consumed once per
    // request; lazy-load identity into the buffers exactly like the
    // header button's path does. Runs BEFORE the m_visible gate so
    // the dialog opens even when the chat panel itself is hidden.
    if (m_uiManager && m_uiManager->consumeOpenChatProfileRequest())
    {
        if (!m_identityLoaded)
        {
            m_identity = identity::load();
            std::snprintf(m_profileNameBuf, sizeof(m_profileNameBuf), "%s",
                          m_identity.name.c_str());
            std::snprintf(m_profileNickBuf, sizeof(m_profileNickBuf), "%s",
                          m_identity.nickname.c_str());
            m_profileAge     = m_identity.age;
            m_identityLoaded = true;
        }
        m_showProfileWindow = true;
        m_profileError.clear();
        m_profileSavedHint.clear();
    }

    // Profile window is rendered unconditionally too — the consume
    // request above can pop it on a hidden chat panel; once shown,
    // it should stay reachable until the user closes it.
    renderProfileWindow();

    if (!m_visible) return;

    const auto snap = m_chat->getSnapshot();
    // No more auto-hide — the user controls visibility via F8 / View
    // menu. Standalone rooms break the old "auto-hide when no
    // streamId" rule because the user needs the Rooms... button to
    // even be reachable. When the panel is idle the empty state
    // tells them to click Rooms....

    // Initial placement only — top-right corner with a reasonable
    // size. After that the user is free to drag and resize; imgui.ini
    // persists position/size by window id across launches. The OSD
    // distinction here isn't "pinned, no chrome" (#84 amendment) but
    // "lives outside the m_uiVisible gate so F12 doesn't hide it".
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float initialW = 340.0f;
    const float initialH = std::clamp(vp->WorkSize.y * 0.45f, 240.0f, 480.0f);
    const ImVec2 initialPos(vp->WorkPos.x + vp->WorkSize.x - initialW - 16.0f,
                            vp->WorkPos.y + 16.0f);
    ImGui::SetNextWindowPos(initialPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(initialW, initialH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 160), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.88f);

    // Title bar gives the user an obvious drag handle. NoCollapse so
    // the chevron doesn't appear (the View → Chat menu / F8 already
    // toggles the panel). NoNav keeps arrow keys out of ImGui nav
    // while the input box has focus.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;

    // Stable id (the part after ##) so imgui.ini round-trips the
    // window's geometry. Visible title ("Chat") is what the user
    // reads on the title bar. p_open is bound to m_visible so the
    // native title-bar X closes the panel cleanly — the user can
    // reopen via View → Chat or the F8 hotkey.
    if (!ImGui::Begin("Chat##osdChat", &m_visible, flags))
    {
        ImGui::End();
        return;
    }

    // ---- status line (title bar already says "Chat") -----------------
    // Bullet drawn through ImDrawList instead of a U+25CF glyph,
    // because Dear ImGui's bundled Proggy Clean font has no data
    // for that codepoint — the glyph would render as the missing
    // box. Same approach UISectionHeader::ui_status_bullet uses
    // for the Configuration-window section indicators.
    ui_status_bullet(stateColor(snap.state));
    ImGui::SameLine();
    ImGui::TextColored(stateColor(snap.state), "%s", stateLabel(snap.state));
    if (!snap.lastError.empty() && snap.state != ChatClient::State::Connected)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("- %s", snap.lastError.c_str());
    }
    // Participant count doubles as the panel toggle: click to expand
    // a small list below the header showing everyone in the room.
    if (!snap.participants.empty())
    {
        ImGui::SameLine();
        char countBuf[32];
        std::snprintf(countBuf, sizeof(countBuf), "%s %zu##chatPart",
                      m_showParticipants ? "v" : ">", snap.participants.size());
        if (ImGui::SmallButton(countBuf))
        {
            m_showParticipants = !m_showParticipants;
        }
    }
    // Header buttons — Profile + Rooms + (when connected) Disconnect,
    // right-aligned with a tiny breathing space from the title bar X.
    // Disconnect lives in the chat window itself rather than inside
    // Rooms because that's where the user's attention is during a
    // session.
    ImGui::SameLine();
    {
        const bool connected = (snap.state != ChatClient::State::Idle) &&
                               (!snap.streamId.empty() || !snap.slug.empty());
        const float profW   = 64.0f;
        const float roomsW  = 64.0f;
        const float discW   = connected ? 80.0f : 0.0f;
        const float gap     = ImGui::GetStyle().ItemSpacing.x;
        const float rightPad = 8.0f;
        const float regionW  = ImGui::GetContentRegionAvail().x;
        const float gapsTotal = gap * (connected ? 2.0f : 1.0f);
        const float spacer   = regionW - profW - roomsW - discW - gapsTotal - rightPad;
        if (spacer > 0.0f) ImGui::Dummy(ImVec2(spacer, 1));
        ImGui::SameLine();
        if (ImGui::SmallButton("Profile"))
        {
            // Lazy-load identity on first open so the buffers reflect
            // disk state. Subsequent opens keep whatever the user
            // typed (m_identityLoaded gate).
            if (!m_identityLoaded)
            {
                m_identity = identity::load();
                std::snprintf(m_profileNameBuf, sizeof(m_profileNameBuf), "%s",
                              m_identity.name.c_str());
                std::snprintf(m_profileNickBuf, sizeof(m_profileNickBuf), "%s",
                              m_identity.nickname.c_str());
                m_profileAge       = m_identity.age;
                m_identityLoaded   = true;
            }
            m_showProfileWindow = true;
            m_profileError.clear();
            m_profileSavedHint.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rooms..."))
        {
            m_showRoomsWindow    = true;
            m_standaloneError.clear();
            m_roomsListRequested = false; // re-fetch on open
            m_ownedRoomsLoaded   = false; // re-load owned list on open
        }
        if (connected)
        {
            ImGui::SameLine();
            // Tinted red so it reads as the session-terminating action
            // it is, but still a SmallButton to stay in the header.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.70f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.45f, 0.18f, 0.18f, 1.0f));
            if (ImGui::SmallButton("Disconnect"))
            {
                m_chat->disconnect();
            }
            ImGui::PopStyleColor(3);
        }
    }
    // Render the Rooms window OUTSIDE the chat panel's Begin/End so
    // it lives as its own draggable top-level window with a native
    // close button (X). The window is now slim: a header strip with
    // the two "open sub-window" buttons + a tab bar with Public and
    // Owned listings. Create + Join Custom forms live in their own
    // top-level windows below.
    if (m_showRoomsWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(440.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Chat rooms##chatRoomsWindow", &m_showRoomsWindow,
                         ImGuiWindowFlags_NoCollapse))
        {
            // Suppress the F8 chat-toggle hotkey while the user is
            // typing into this window's inputs.
            m_inputFocused = true;

            // ---------- Header strip ---------------------------------------
            if (ImGui::Button("Create new..."))
            {
                m_showCreateWindow = true;
                m_standaloneError.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Join custom..."))
            {
                m_showJoinCustomWindow = true;
                m_standaloneError.clear();
            }
            ImGui::SameLine();
            // Refresh button anchored right — refreshes whichever
            // tab is active. Both lists reload cheaply.
            {
                const float btnW    = 70.0f;
                const float pad     = 4.0f;
                const float region  = ImGui::GetContentRegionAvail().x;
                const float spacer  = region - btnW - pad;
                if (spacer > 0.0f) ImGui::Dummy(ImVec2(spacer, 1));
                ImGui::SameLine();
                if (ImGui::Button("Refresh", ImVec2(btnW, 0)))
                {
                    m_roomsListRequested = false;
                    m_ownedRoomsLoaded   = false;
                }
            }
            ImGui::Spacing();

            // ---------- Tabs -----------------------------------------------
            if (ImGui::BeginTabBar("##chatRoomsTabs"))
            {
                if (ImGui::BeginTabItem("Public"))
                {
                    // Lazy load on first open of the window or after a
                    // Refresh click.
                    if (!m_roomsListRequested)
                    {
                        std::string err;
                        m_roomsList.clear();
                        if (!m_chat->listPublicRooms(50, m_roomsList, err))
                        {
                            m_roomsListError = err;
                        }
                        else
                        {
                            m_roomsListError.clear();
                        }
                        m_roomsListRequested = true;
                    }
                    if (!m_roomsListError.empty())
                    {
                        ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                           "%s", m_roomsListError.c_str());
                    }
                    else if (m_roomsList.empty())
                    {
                        ImGui::TextDisabled("(no public rooms yet)");
                    }
                    else
                    {
                        if (ImGui::BeginChild("##chatRoomsBrowse",
                                              ImVec2(0, 0), true))
                        {
                            for (const auto &r : m_roomsList)
                            {
                                const bool isStream =
                                    (r.kind == "stream_linked");
                                // Stream-linked rooms have no slug;
                                // fall back to the streamId for a
                                // human-pointable label. Title is
                                // empty for v0.5 stream rooms (the
                                // chat service has no link to the
                                // directory's title field yet).
                                std::string label;
                                if (!r.title.empty())          label = r.title;
                                else if (!r.slug.empty())      label = "#" + r.slug;
                                else if (!r.streamId.empty())  label = "stream " + r.streamId;
                                else                           label = r.roomId;

                                // Colour-coded kind badge so the
                                // user instantly sees standalone vs
                                // stream-linked entries.
                                const ImVec4 kindCol = isStream
                                    ? ImVec4(0.95f, 0.78f, 0.30f, 1.0f)
                                    : ImVec4(0.65f, 0.75f, 0.95f, 1.0f);
                                ImGui::TextColored(kindCol, "%s",
                                    isStream ? "[STREAM]" : "[ROOM]");
                                ImGui::SameLine();

                                char selBuf[256];
                                std::snprintf(selBuf, sizeof(selBuf),
                                              "%s%s##%s",
                                              r.hasPassword ? "[lock] " : "",
                                              label.c_str(), r.roomId.c_str());
                                if (ImGui::Selectable(selBuf))
                                {
                                    const std::string nick = m_uiManager
                                        ? m_uiManager->getChatNickname()
                                        : snap.nickname;
                                    if (isStream)
                                    {
                                        // Stream-linked rooms are
                                        // password-less by design;
                                        // join straight via streamId.
                                        m_chat->connect(r.streamId, nick,
                                                        /*asHost=*/false);
                                    }
                                    else if (r.hasPassword)
                                    {
                                        // Pre-fill the join form so
                                        // the user can supply a
                                        // password.
                                        std::snprintf(m_joinSlugBuf,
                                                      sizeof(m_joinSlugBuf),
                                                      "%s", r.slug.c_str());
                                        m_showJoinCustomWindow = true;
                                    }
                                    else
                                    {
                                        OwnedRoom owned;
                                        const std::string sec =
                                            ownedrooms::findBySlug(r.slug, owned)
                                                ? owned.ownerSecret
                                                : std::string{};
                                        m_chat->connectBySlug(r.slug, nick, "", sec);
                                        // Leave the window open — the
                                        // user decides when to close
                                        // it. Also lets connect errors
                                        // (password_wrong, etc) stay
                                        // visible inline.
                                    }
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("  %d", r.participantCount);
                            }
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Owned"))
                {
                    if (!m_ownedRoomsLoaded)
                    {
                        m_ownedRooms = ownedrooms::loadAll();
                        m_ownedRoomsLoaded = true;
                    }
                    if (!m_deleteError.empty())
                    {
                        ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                           "%s", m_deleteError.c_str());
                    }
                    if (m_ownedRooms.empty())
                    {
                        ImGui::TextDisabled(
                            "(you haven't created any rooms yet)");
                        ImGui::Spacing();
                        ImGui::TextDisabled(
                            "Click \"Create new...\" above to mint one.");
                    }
                    else
                    {
                        if (ImGui::BeginChild("##chatOwnedList",
                                              ImVec2(0, 0), true))
                        {
                            for (size_t i = 0; i < m_ownedRooms.size(); ++i)
                            {
                                const auto &r = m_ownedRooms[i];
                                const std::string label = r.title.empty()
                                    ? ("#" + r.slug)
                                    : r.title;
                                ImGui::PushID(static_cast<int>(i));
                                ImGui::TextColored(
                                    ImVec4(0.98f, 0.98f, 0.92f, 1.0f),
                                    "%s", label.c_str());
                                ImGui::SameLine();
                                ImGui::TextDisabled("  #%s", r.slug.c_str());

                                // Action row
                                if (ImGui::SmallButton("Join"))
                                {
                                    const std::string nick = m_uiManager
                                        ? m_uiManager->getChatNickname()
                                        : snap.nickname;
                                    // #84 — Revive path: the server
                                    // may have reaped this room via
                                    // the inactivity sweep while we
                                    // kept the local entry. Probe
                                    // first; if gone, recreate with
                                    // the same slug + saved secret
                                    // so we transparently keep
                                    // ownership.
                                    std::string probeErr;
                                    const bool exists =
                                        m_chat->roomExistsBySlug(
                                            r.slug, probeErr);
                                    if (!exists && probeErr.empty())
                                    {
                                        std::string newRoomId, newSlug, err;
                                        if (m_chat->createStandaloneRoom(
                                                r.title, r.slug,
                                                /*password=*/"", /*listed=*/true,
                                                /*ownerClientId=*/m_identity.id,
                                                r.ownerSecret,
                                                newRoomId, newSlug, err))
                                        {
                                            OwnedRoom rec = r;
                                            rec.roomId = newRoomId;
                                            ownedrooms::append(rec);
                                            m_ownedRoomsLoaded = false;
                                        }
                                        else
                                        {
                                            m_deleteError =
                                                "Revive failed: " + err;
                                        }
                                    }
                                    m_chat->connectBySlug(
                                        r.slug, nick, "", r.ownerSecret);
                                    m_pendingDeleteSlug.clear();
                                }
                                ImGui::SameLine();
                                const bool pendingThis =
                                    (m_pendingDeleteSlug == r.slug);
                                if (pendingThis)
                                {
                                    ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                        ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                        ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
                                }
                                const char *delLabel = pendingThis
                                    ? "Confirm delete (forever)"
                                    : "Delete";
                                if (ImGui::SmallButton(delLabel))
                                {
                                    if (!pendingThis)
                                    {
                                        m_pendingDeleteSlug = r.slug;
                                        m_deleteError.clear();
                                    }
                                    else
                                    {
                                        // Fire the server-side delete.
                                        // Order: server first (so we
                                        // don't lose the secret if the
                                        // network fails); then erase
                                        // the local entry; finally
                                        // disconnect if we're still in
                                        // the doomed room.
                                        std::string err;
                                        const bool ok =
                                            m_chat->deleteStandaloneRoom(
                                                r.roomId, r.ownerSecret, err);
                                        if (!ok)
                                        {
                                            m_deleteError = err;
                                        }
                                        else
                                        {
                                            const std::string doomedSlug =
                                                r.slug;
                                            ownedrooms::remove(doomedSlug);
                                            m_ownedRoomsLoaded   = false;
                                            m_roomsListRequested = false;
                                            m_pendingDeleteSlug.clear();
                                            m_deleteError.clear();
                                            // If we're currently in
                                            // that room, drop the
                                            // session — the server
                                            // already evicted us, but
                                            // the client state is
                                            // still pointing at it.
                                            if (snap.slug == doomedSlug)
                                            {
                                                m_chat->disconnect();
                                            }
                                            ImGui::PopID();
                                            if (pendingThis)
                                                ImGui::PopStyleColor(3);
                                            // Re-read the (possibly
                                            // shrunk) vector on the
                                            // next frame — bail out
                                            // of the for-loop now.
                                            break;
                                        }
                                    }
                                }
                                if (pendingThis)
                                {
                                    ImGui::PopStyleColor(3);
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Cancel"))
                                    {
                                        m_pendingDeleteSlug.clear();
                                    }
                                }
                                ImGui::Separator();
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    // -------- Create new room window (#84) ------------------------------
    // Standalone top-level window — opened from the Rooms window's
    // "Create new..." header button. Closes via X (p_open bound).
    if (m_showCreateWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Create new room##chatCreateWindow",
                         &m_showCreateWindow,
                         ImGuiWindowFlags_NoCollapse))
        {
            m_inputFocused = true;
            ImGui::TextDisabled("Title");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##createTitle", "Friday Smash Bros",
                                     m_createTitleBuf,
                                     sizeof(m_createTitleBuf));
            ImGui::TextDisabled("Slug (optional)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##createSlug",
                                     "auto-generated when blank",
                                     m_createSlugBuf, sizeof(m_createSlugBuf));
            ImGui::TextDisabled("Password (optional)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##createPass",
                                     "leave blank for an open room",
                                     m_createPasswordBuf,
                                     sizeof(m_createPasswordBuf),
                                     ImGuiInputTextFlags_Password);
            ImGui::Checkbox("List publicly", &m_createListed);
            ImGui::Spacing();

            ImGui::BeginDisabled(m_createInFlight);
            if (ImGui::Button("Create + Join", ImVec2(-1.0f, 0.0f)))
            {
                m_standaloneError.clear();
                m_createInFlight = true;
                std::string newRoomId;
                std::string newSlug;
                std::string err;
                const std::string ownerId = m_identity.id;

                // #84 — Revive-aware create. If the user typed a slug
                // they already own (matches owned_rooms.json), reuse
                // the saved secret instead of minting a new one. That
                // way:
                //   - if the server still has the room: the POST hits
                //     a slug-collision; we fall back to connecting
                //     directly with the saved secret (still owner);
                //   - if the server reaped it: the POST succeeds with
                //     same slug + same secret, ownership preserved.
                OwnedRoom existingOwned;
                const bool reviving = m_createSlugBuf[0] != '\0' &&
                                      ownedrooms::findBySlug(m_createSlugBuf,
                                                             existingOwned);
                const std::string ownerSecret = reviving
                    ? existingOwned.ownerSecret
                    : ownedrooms::generateSecret();

                bool ok = m_chat->createStandaloneRoom(
                    m_createTitleBuf, m_createSlugBuf,
                    m_createPasswordBuf, m_createListed, ownerId,
                    ownerSecret,
                    newRoomId, newSlug, err);

                // Revive fallback: if the POST failed AND the slug is
                // one we own, the server probably still has the room
                // (slug-collision against our previous registration).
                // Connect directly — our owner_secret in the hello
                // grants is_owner without needing the POST to succeed.
                if (!ok && reviving)
                {
                    newSlug   = existingOwned.slug;
                    newRoomId = existingOwned.roomId;
                    ok        = true;
                    err.clear();
                }

                m_createInFlight = false;
                if (!ok)
                {
                    m_standaloneError = err;
                }
                else
                {
                    OwnedRoom rec;
                    rec.roomId      = newRoomId;
                    rec.slug        = newSlug;
                    rec.title       = m_createTitleBuf[0] != '\0'
                                          ? std::string(m_createTitleBuf)
                                          : existingOwned.title;
                    rec.ownerSecret = ownerSecret;
                    ownedrooms::append(rec);

                    const std::string nick = m_uiManager
                        ? m_uiManager->getChatNickname()
                        : snap.nickname;
                    m_chat->connectBySlug(newSlug, nick,
                                          m_createPasswordBuf, ownerSecret);
                    m_createTitleBuf[0]    = '\0';
                    m_createSlugBuf[0]     = '\0';
                    m_createPasswordBuf[0] = '\0';
                    // Window stays open — the user closes when ready.
                    // Refresh the cached lists so the new room shows
                    // up immediately when they switch to Owned/Public.
                    m_roomsListRequested   = false;
                    m_ownedRoomsLoaded     = false;
                }
            }
            ImGui::EndDisabled();
            if (!m_standaloneError.empty())
            {
                ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                   "%s", m_standaloneError.c_str());
            }
        }
        ImGui::End();
    }

    // -------- Join custom room window (#84) -----------------------------
    // Opened from the Rooms window header OR from clicking a
    // password-protected entry in the Public listing (in which case
    // m_joinSlugBuf is pre-filled).
    if (m_showJoinCustomWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Join custom room##chatJoinCustomWindow",
                         &m_showJoinCustomWindow,
                         ImGuiWindowFlags_NoCollapse))
        {
            m_inputFocused = true;
            ImGui::TextDisabled("Slug");
            ImGui::SetNextItemWidth(-1.0f);
            const bool joinEnter = ImGui::InputTextWithHint(
                "##joinSlug", "smash-sun",
                m_joinSlugBuf, sizeof(m_joinSlugBuf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled("Password (if required)");
            ImGui::SetNextItemWidth(-1.0f);
            const bool passEnter = ImGui::InputTextWithHint(
                "##joinPass", "",
                m_joinPasswordBuf, sizeof(m_joinPasswordBuf),
                ImGuiInputTextFlags_Password |
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            const bool joinClick = ImGui::Button("Join", ImVec2(-1.0f, 0.0f));
            if (joinEnter || passEnter || joinClick)
            {
                std::string slug = m_joinSlugBuf;
                while (!slug.empty() && std::isspace((unsigned char)slug.back()))
                    slug.pop_back();
                for (auto &c : slug)
                    c = static_cast<char>(std::tolower((unsigned char)c));
                if (slug.empty())
                {
                    m_standaloneError = "Slug can't be empty";
                }
                else
                {
                    const std::string nick = m_uiManager
                        ? m_uiManager->getChatNickname()
                        : snap.nickname;
                    OwnedRoom owned;
                    std::string sec;
                    if (ownedrooms::findBySlug(slug, owned))
                    {
                        sec = owned.ownerSecret;
                        // #84 — Revive: if the user is joining a slug
                        // they own and the server doesn't have it
                        // anymore (sweep, manual delete elsewhere),
                        // recreate transparently with the saved
                        // secret so ownership survives.
                        std::string probeErr;
                        const bool exists = m_chat->roomExistsBySlug(
                            slug, probeErr);
                        if (!exists && probeErr.empty())
                        {
                            std::string newRoomId, newSlug, err;
                            if (m_chat->createStandaloneRoom(
                                    owned.title, slug,
                                    /*password=*/m_joinPasswordBuf,
                                    /*listed=*/true,
                                    /*ownerClientId=*/m_identity.id,
                                    sec, newRoomId, newSlug, err))
                            {
                                OwnedRoom rec = owned;
                                rec.roomId = newRoomId;
                                ownedrooms::append(rec);
                            }
                            else
                            {
                                m_standaloneError =
                                    "Revive failed: " + err;
                            }
                        }
                    }
                    m_chat->connectBySlug(slug, nick,
                                          m_joinPasswordBuf, sec);
                    // Window stays open so the user sees password
                    // errors inline; m_joinPasswordBuf is kept too in
                    // case they want to fix a typo and retry.
                    if (m_standaloneError.empty()) m_standaloneError.clear();
                }
            }
            if (!m_standaloneError.empty())
            {
                ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                   "%s", m_standaloneError.c_str());
            }
            // Live status feedback so the user sees why a join didn't
            // take. Server-side hello rejections (password_wrong,
            // identity_in_use, etc) leave the ChatClient in Error with
            // lastError populated.
            if (snap.state == ChatClient::State::Resolving ||
                snap.state == ChatClient::State::Connecting)
            {
                ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.90f, 1.0f),
                                   "Connecting...");
            }
            else if (snap.state == ChatClient::State::Error &&
                     !snap.lastError.empty())
            {
                ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                   "%s", snap.lastError.c_str());
            }
            else if (snap.state == ChatClient::State::Connected &&
                     !snap.slug.empty() && snap.slug == [&] {
                         std::string s = m_joinSlugBuf;
                         while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
                         for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
                         return s;
                     }())
            {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                                   "Connected.");
            }
        }
        ImGui::End();
    }

    // Profile window now lives in renderProfileWindow() and is
    // rendered ABOVE this gate so it can be opened even when the
    // chat panel itself is hidden (e.g. Streaming → Configure
    // Profile button).

    ImGui::Separator();

    // ---- room info banner --------------------------------------------
    // Prominent room label so the user always knows which room they
    // landed in. Format:
    //     [STREAM] Friday Smash Bros
    //     [ROOM]   #smash-sun · hosted by geldo
    // The kind tag colour-codes the row; missing title falls back to
    // slug; missing slug means stream-linked unnamed, which is also
    // useful to surface ("you're in the host's chat").
    if (snap.state != ChatClient::State::Idle &&
        (!snap.roomTitle.empty() || !snap.slug.empty() || !snap.streamId.empty()))
    {
        const bool isStandalone = !snap.slug.empty();
        const ImVec4 kindColor = isStandalone
            ? ImVec4(0.65f, 0.75f, 0.95f, 1.0f)   // blueish for standalone
            : ImVec4(0.95f, 0.78f, 0.30f, 1.0f);  // gold for stream-linked
        ImGui::TextColored(kindColor, "%s",
                           isStandalone ? "[ROOM]" : "[STREAM]");
        ImGui::SameLine();
        const std::string label = !snap.roomTitle.empty()
            ? snap.roomTitle
            : (!snap.slug.empty() ? ("#" + snap.slug) : "Host stream chat");
        ImGui::TextColored(ImVec4(0.98f, 0.98f, 0.92f, 1.0f), "%s",
                           label.c_str());
        // Sub-line: surface the host's name when we know it, and the
        // slug as a copy-friendly identifier for standalone rooms.
        std::string sub;
        if (isStandalone)
        {
            // Slug only as detail if the title is what's on the main
            // line. (Avoid showing "#smash-sun" twice in a row.)
            if (!snap.roomTitle.empty() && !snap.slug.empty())
            {
                sub = "#" + snap.slug;
            }
        }
        else if (!snap.hostParticipantId.empty())
        {
            // Find the host in the participant list to surface their
            // nickname (the actual id is on the participants panel
            // tooltip; here we want the human label).
            for (const auto &p : snap.participants)
            {
                if (p.host) { sub = "hosted by " + p.nickname; break; }
            }
        }
        if (!sub.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("- %s", sub.c_str());
        }
        ImGui::Separator();
    }

    // (#84) Inline "as <nick> [Apply]" removed — identity now lives
    // in a dedicated Profile window opened via the header button.
    // Nickname/Name/Age/ID all set there; persisted to identity.json.

    // ---- profile gate -------------------------------------------------
    // When the user hasn't filled out a chat nickname yet, the chat
    // client refuses to connect (no hello = no session). Replace the
    // body area with a focused CTA so the user can fix it in one
    // click instead of seeing an empty panel with no explanation.
    const std::string activeNickname = m_uiManager
        ? m_uiManager->getChatNickname()
        : snap.nickname;
    if (activeNickname.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                           "Profile required");
        ImGui::TextWrapped(
            "You need a chat profile before you can send or receive "
            "messages. Click below to set your nickname.");
        ImGui::Spacing();
        if (ImGui::Button("Open Profile", ImVec2(-1.0f, 0.0f)))
        {
            if (!m_identityLoaded)
            {
                m_identity = identity::load();
                std::snprintf(m_profileNameBuf, sizeof(m_profileNameBuf), "%s",
                              m_identity.name.c_str());
                std::snprintf(m_profileNickBuf, sizeof(m_profileNickBuf), "%s",
                              m_identity.nickname.c_str());
                m_profileAge     = m_identity.age;
                m_identityLoaded = true;
            }
            m_showProfileWindow = true;
            m_profileError.clear();
            m_profileSavedHint.clear();
        }
        ImGui::End();
        return;
    }

    // ---- two-column body: participants (left) + messages (right) ------
    //
    // Layout: when m_showParticipants is on, the body area is split
    // horizontally into a narrow sidebar (~120 px or ~25 % of width,
    // whichever fits) on the left and the message log filling the
    // rest on the right. When off, the message log takes the full
    // width — same behaviour as before the panel existed.
    //
    // Both children share the same vertical budget: the area between
    // here and the input row at the bottom. footerHeight reserves
    // the input row + a little padding.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    const float bodyAvailX   = ImGui::GetContentRegionAvail().x;
    const float bodyAvailY   = ImGui::GetContentRegionAvail().y - footerHeight;
    const bool  showParts    = m_showParticipants && !snap.participants.empty();
    const float partsW       = showParts
                                   ? std::clamp(bodyAvailX * 0.30f, 110.0f, 180.0f)
                                   : 0.0f;
    const float gap          = showParts ? ImGui::GetStyle().ItemSpacing.x : 0.0f;
    const float logW         = bodyAvailX - partsW - gap;

    if (showParts)
    {
        std::vector<ChatClient::Participant> sorted = snap.participants;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ChatClient::Participant &a,
                     const ChatClient::Participant &b)
                  {
                      if (a.host != b.host) return a.host && !b.host;
                      return a.nickname < b.nickname;
                  });

        if (ImGui::BeginChild("##chatParts",
                              ImVec2(partsW, bodyAvailY),
                              true))
        {
            for (const auto &p : sorted)
            {
                const bool isMe = !snap.myParticipantId.empty() &&
                                  p.id == snap.myParticipantId;
                if (p.host)
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
                                       "[HOST]");
                    ImGui::SameLine();
                }
                else if (p.owner)
                {
                    ImGui::TextColored(ImVec4(0.75f, 0.90f, 1.00f, 1.0f),
                                       "[OWNER]");
                    ImGui::SameLine();
                }
                const ImVec4 nameColor = p.host
                    ? ImVec4(0.95f, 0.78f, 0.30f, 1.0f)
                    : (p.owner ? ImVec4(0.75f, 0.90f, 1.00f, 1.0f)
                    : (isMe ? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
                            : ImVec4(0.48f, 0.78f, 0.94f, 1.0f)));
                ImGui::TextColored(nameColor, "%s", p.nickname.c_str());
                // Hover shows the participant's id (rc_<…> when
                // persistent, p_<…> when anon). Same affordance as
                // the message list.
                if (!p.id.empty() && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", p.id.c_str());
                }
                // Double-click a nick to insert a mention into the
                // input — same shortcut the message list has.
                if (!isMe && !p.nickname.empty() &&
                    ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    m_inputCb.pendingInsert =
                        std::string("@") + p.nickname + " ";
                }
                if (isMe)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(you)");
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }

    // ---- message list -------------------------------------------------
    if (ImGui::BeginChild("##chatLog",
                          ImVec2(logW, bodyAvailY),
                          false,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        if (snap.messages.empty())
        {
            ImGui::TextDisabled("(no messages yet - say hi)");
        }
        for (const auto &m : snap.messages)
        {
            // Mention check: someone else cited @<myNick>. We want
            // a full-row background tint (amber) + a gold stripe on
            // the left so the row pops at a glance, matching the
            // web portal's `.mention` styling. Tinted body text
            // alone is too subtle to catch in scrolling chat.
            const bool isMention = !m.deleted &&
                                   !m.local &&
                                   !snap.nickname.empty() &&
                                   mentionsNick(m.body, snap.nickname);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (isMention) dl->ChannelsSplit(2);
            if (isMention) dl->ChannelsSetCurrent(1); // foreground

            ImGui::BeginGroup();

            const std::string when = formatTime(m.postedAtMs);
            if (!when.empty())
            {
                ImGui::TextDisabled("%s", when.c_str());
                ImGui::SameLine();
            }
            // Fallback: a message with no `is_host` field but whose
            // participant id matches the current host gets the badge.
            // Covers history rows from an older server that didn't
            // know about the role.
            const bool isHostMsg = m.host ||
                (!snap.hostParticipantId.empty() &&
                 m.participantId == snap.hostParticipantId);
            const bool isOwnerMsg = m.owner ||
                (!snap.ownerClientId.empty() &&
                 m.participantId == snap.ownerClientId);
            if (isHostMsg)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f), "[HOST]");
                ImGui::SameLine();
            }
            else if (isOwnerMsg)
            {
                ImGui::TextColored(ImVec4(0.75f, 0.90f, 1.00f, 1.0f), "[OWNER]");
                ImGui::SameLine();
            }
            // Local-author wins as the colour cue (you posted this),
            // then host (other host's messages stand out), then a
            // neutral viewer colour.
            ImVec4 nameColor;
            if (m.local)         nameColor = ImVec4(0.45f, 0.85f, 0.50f, 1.0f);
            else if (isHostMsg)  nameColor = ImVec4(0.95f, 0.78f, 0.30f, 1.0f);
            else if (isOwnerMsg) nameColor = ImVec4(0.75f, 0.90f, 1.00f, 1.0f);
            else                 nameColor = ImVec4(0.48f, 0.78f, 0.94f, 1.0f);
            ImGui::TextColored(nameColor, "%s:", m.nickname.c_str());
            // Hover shows the persistent identity (rc_<id>) when the
            // poster sent one, or the per-session p_<random> id for
            // anonymous posters. Lets viewers tell "two alices in
            // different sessions" apart.
            if (!m.participantId.empty() && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", m.participantId.c_str());
            }

            // Double-click the nick to drop "@<nick> " at the cursor
            // of the input. Routed through m_inputCb.pendingInsert so
            // the InputTextMultiline's CallbackAlways path picks it
            // up the next frame — direct writes to m_inputBuf are
            // lost while the widget is active.
            if (!m.local && !m.nickname.empty() &&
                ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_inputCb.pendingInsert = std::string("@") + m.nickname + " ";
            }
            ImGui::SameLine();
            if (m.deleted)
            {
                ImGui::TextDisabled("%s", m.body.c_str());
            }
            else if (isMention)
            {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.98f, 0.92f, 0.65f, 1.0f));
                ImGui::TextWrapped("%s", m.body.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextWrapped("%s", m.body.c_str());
            }

            ImGui::EndGroup();

            if (isMention)
            {
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const float padX  = 6.0f;
                const float padY  = 2.0f;
                dl->ChannelsSetCurrent(0); // background, drawn behind text
                dl->AddRectFilled(
                    ImVec2(rmin.x - padX, rmin.y - padY),
                    ImVec2(rmax.x + padX, rmax.y + padY),
                    IM_COL32(243, 201, 62, 28));            // soft amber
                dl->AddRectFilled(
                    ImVec2(rmin.x - padX, rmin.y - padY),
                    ImVec2(rmin.x - padX + 3.0f, rmax.y + padY),
                    IM_COL32(243, 201, 62, 255));           // gold stripe
                dl->ChannelsMerge();
            }
        }

        // Auto-scroll only when the user is at the bottom; if they
        // scrolled up to re-read history we keep their position. The
        // re-arm threshold is a few pixels of slack so a near-bottom
        // position counts as "at the bottom".
        const float scrollMaxY = ImGui::GetScrollMaxY();
        const float scrollY    = ImGui::GetScrollY();
        const bool  atBottom   = (scrollMaxY - scrollY) < 6.0f;
        if (m_autoScroll)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        m_autoScroll = atBottom;
    }
    ImGui::EndChild();

    // ---- input row ----------------------------------------------------
    //
    // InputTextMultiline at single-row height so Enter inserts '\n'
    // instead of deactivating the widget (the focus-loss problem of
    // plain InputText that no SetKeyboardFocusHere variant could
    // recover from on this build).
    //
    // Catch: while the widget is active, ImGui caches the text in
    // its own ImGuiInputTextState. Modifying m_inputBuf externally
    // (e.g. `m_inputBuf[0] = '\0'`) is ignored — ImGui reapplies its
    // cached "hello\n" on the next frame, my newline scan fires
    // again, the message gets posted in a loop. The fix is to do
    // the clear THROUGH the callback API, which mutates the active
    // widget's state directly.
    //
    // Callback design: CallbackEdit fires on every edit. Inside we
    // scan for '\n'; on finding one we capture everything before it
    // into pendingPost, then DeleteChars from 0..BufTextLen so the
    // buffer ends frame empty AND ImGui's cached state agrees.
    auto editCallback = [](ImGuiInputTextCallbackData *data) -> int {
        auto *cb = static_cast<InputEditCb *>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
        {
            if (!cb->pendingInsert.empty())
            {
                data->InsertChars(data->CursorPos,
                                  cb->pendingInsert.c_str(),
                                  cb->pendingInsert.c_str() +
                                      cb->pendingInsert.size());
                cb->pendingInsert.clear();
            }
            return 0;
        }
        if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
        {
            for (int i = 0; i < data->BufTextLen; ++i)
            {
                if (data->Buf[i] == '\n')
                {
                    cb->pendingPost.assign(data->Buf, data->Buf + i);
                    // Strip everything from 0 onwards — clears the
                    // line that just got submitted (and any junk
                    // after \n).
                    data->DeleteChars(0, data->BufTextLen);
                    break;
                }
            }
        }
        return 0;
    };

    const bool sendable = snap.state == ChatClient::State::Connected;
    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginDisabled(!sendable);
    const ImVec2 inputSize(
        ImGui::GetContentRegionAvail().x - 60.0f - ImGui::GetStyle().ItemSpacing.x,
        rowH);
    ImGui::InputTextMultiline(
        "##chatInput", m_inputBuf, sizeof(m_inputBuf),
        inputSize,
        ImGuiInputTextFlags_NoHorizontalScroll |
            ImGuiInputTextFlags_CallbackEdit       |
            ImGuiInputTextFlags_CallbackAlways,
        editCallback, &m_inputCb);
    if (ImGui::IsItemActive()) m_inputFocused = true;
    ImGui::SameLine();
    const bool sendClicked = ImGui::Button("Send", ImVec2(-1.0f, rowH));
    ImGui::EndDisabled();

    // 1) Enter path — body was captured by the callback, buffer
    //    already cleared via DeleteChars while the widget was
    //    rendering. Post once, drop the pending body.
    if (!m_inputCb.pendingPost.empty())
    {
        if (sendable)
        {
            std::string body = std::move(m_inputCb.pendingPost);
            // Trim trailing whitespace (the line never contained a
            // newline — that was the submit trigger and got stripped).
            while (!body.empty() &&
                   (body.back() == '\r' || body.back() == ' ' || body.back() == '\t'))
            {
                body.pop_back();
            }
            if (!body.empty()) m_chat->post(body);
        }
        m_inputCb.pendingPost.clear();
    }

    // 2) Send button path — widget isn't active during the click,
    //    so the callback isn't going to fire. Read the buffer
    //    directly and clear it (the deactivation that ImGui already
    //    handles for us on the button click means ImGui's cached
    //    state isn't fighting us here).
    if (sendClicked && sendable)
    {
        std::string body = m_inputBuf;
        while (!body.empty() &&
               (body.back() == '\n' || body.back() == '\r' ||
                body.back() == ' '  || body.back() == '\t'))
        {
            body.pop_back();
        }
        if (!body.empty()) m_chat->post(body);
        m_inputBuf[0] = '\0';
    }

    // Reset unread counter while the panel is on screen and current.
    if (sendable && m_autoScroll) m_chat->markRead();

    ImGui::End();
}

// Standalone Profile dialog — rendered unconditionally from render()
// so external triggers (Streaming → Configure Profile) can pop it
// even when the chat panel is hidden. The body is the same edit
// form the header's Profile button used to inline.
void OSDChat::renderProfileWindow()
{
    if (!m_showProfileWindow) return;
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat profile##chatProfileWindow",
                     &m_showProfileWindow,
                     ImGuiWindowFlags_NoCollapse))
    {
        m_inputFocused = true;

        ImGui::TextDisabled("Display name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##profName", "Your real name",
                                 m_profileNameBuf, sizeof(m_profileNameBuf));

        ImGui::TextDisabled("Nickname (visible in chat)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##profNick", "geldo",
                                 m_profileNickBuf, sizeof(m_profileNickBuf));

        ImGui::TextDisabled("Age");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("##profAge", &m_profileAge);
        if (m_profileAge < 0)   m_profileAge = 0;
        if (m_profileAge > 130) m_profileAge = 130;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Identity (read-only)");
        if (m_identity.id.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                               "Not generated yet - click Save to mint one");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f),
                               "%s", m_identity.id.c_str());
            if (!m_identity.createdAt.empty())
            {
                ImGui::TextDisabled("Created: %s",
                                    m_identity.createdAt.c_str());
            }
        }
        ImGui::TextDisabled("File: %s", identity::filePath().c_str());
        ImGui::TextDisabled(
            "Back this file up - losing it loses your chat identity.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool nickEmpty = (m_profileNickBuf[0] == '\0');
        ImGui::BeginDisabled(nickEmpty);
        if (ImGui::Button("Save", ImVec2(-1.0f, 0.0f)))
        {
            m_identity.name     = m_profileNameBuf;
            m_identity.nickname = m_profileNickBuf;
            m_identity.age      = m_profileAge;
            if (identity::save(m_identity))
            {
                if (m_chat) m_chat->setClientId(m_identity.id);
                if (m_uiManager)
                {
                    m_uiManager->setChatNickname(m_identity.nickname);
                    m_uiManager->saveConfig();
                }
                if (m_chat) m_chat->setNickname(m_identity.nickname);
                m_profileSavedHint = "Saved.";
                m_profileError.clear();
            }
            else
            {
                m_profileError = "Failed to write identity.json";
                m_profileSavedHint.clear();
            }
        }
        ImGui::EndDisabled();
        if (nickEmpty)
        {
            ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                               "Nickname is required.");
        }
        if (!m_profileError.empty())
        {
            ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                               "%s", m_profileError.c_str());
        }
        if (!m_profileSavedHint.empty())
        {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                               "%s", m_profileSavedHint.c_str());
        }
    }
    ImGui::End();
}
