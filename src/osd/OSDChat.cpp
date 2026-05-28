#include "OSDChat.h"

#include "../chat/ChatClient.h"
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
    if (!m_visible || !m_chat) return;

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
    // reads on the title bar.
    if (!ImGui::Begin("Chat##osdChat", nullptr, flags))
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
    // Header buttons — Profile + Rooms, right-aligned with a tiny
    // breathing space from the title bar X.
    ImGui::SameLine();
    {
        const float profW  = 64.0f;
        const float roomsW = 64.0f;
        const float gap    = ImGui::GetStyle().ItemSpacing.x;
        const float rightPad = 8.0f;
        const float regionW  = ImGui::GetContentRegionAvail().x;
        const float spacer   = regionW - profW - roomsW - gap - rightPad;
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
            m_showRoomsWindow = true;
            m_standaloneError.clear();
        }
    }
    // Render the Rooms window OUTSIDE the chat panel's Begin/End so
    // it lives as its own draggable top-level window with a native
    // close button (X). The bool is wired into ImGui::Begin's p_open
    // so clicking X closes it cleanly.
    if (m_showRoomsWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Chat rooms##chatRoomsWindow", &m_showRoomsWindow,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings))
        {
            // Suppress the F8 chat-toggle hotkey while the user is
            // typing into this window's inputs.
            m_inputFocused = true;

            ImGui::TextDisabled("Join existing room");
            ImGui::SetNextItemWidth(-60.0f);
            const bool joinEnter = ImGui::InputText(
                "##joinSlug", m_joinSlugBuf, sizeof(m_joinSlugBuf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool joinClick = ImGui::Button("Join", ImVec2(-1.0f, 0.0f));
            if (joinEnter || joinClick)
            {
                std::string slug = m_joinSlugBuf;
                // Trim + lowercase to match server-side validation.
                while (!slug.empty() && std::isspace((unsigned char)slug.back())) slug.pop_back();
                for (auto &c : slug) c = static_cast<char>(std::tolower((unsigned char)c));
                if (slug.empty())
                {
                    m_standaloneError = "Slug can't be empty";
                }
                else
                {
                    m_chat->connectBySlug(slug, snap.nickname);
                    m_standaloneError.clear();
                    m_showRoomsWindow = false;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Create new room");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##createTitle", "title",
                m_createTitleBuf, sizeof(m_createTitleBuf));
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##createSlug", "slug (optional, auto-generated)",
                m_createSlugBuf, sizeof(m_createSlugBuf));
            ImGui::BeginDisabled(m_createInFlight);
            if (ImGui::Button("Create + Join", ImVec2(-1.0f, 0.0f)))
            {
                m_standaloneError.clear();
                m_createInFlight = true;
                std::string newSlug;
                std::string err;
                const bool ok = m_chat->createStandaloneRoom(
                    m_createTitleBuf, m_createSlugBuf, newSlug, err);
                m_createInFlight = false;
                if (!ok)
                {
                    m_standaloneError = err;
                }
                else
                {
                    m_chat->connectBySlug(newSlug, snap.nickname);
                    m_createTitleBuf[0] = '\0';
                    m_createSlugBuf[0]  = '\0';
                    m_showRoomsWindow = false;
                }
            }
            ImGui::EndDisabled();

            if (!m_standaloneError.empty())
            {
                ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f),
                                   "%s", m_standaloneError.c_str());
            }
            if (!snap.streamId.empty() || !snap.slug.empty())
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("Leave / disconnect", ImVec2(-1.0f, 0.0f)))
                {
                    m_chat->disconnect();
                    m_showRoomsWindow = false;
                }
            }
        }
        ImGui::End();
    }

    // Profile window (#84). Standalone top-level window with native
    // X close (p_open bound to m_showProfileWindow). The user fills
    // Name / Nickname / Age and clicks Save; the first save derives
    // the immutable ID from those inputs + timestamp + random bytes.
    // Subsequent saves keep the ID and just refresh editable fields.
    if (m_showProfileWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Chat profile##chatProfileWindow",
                         &m_showProfileWindow,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings))
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
            if (m_profileAge < 0)  m_profileAge = 0;
            if (m_profileAge > 130) m_profileAge = 130;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ID block. Shown read-only — generated on first save,
            // immutable afterwards. The disk path is surfaced so the
            // user knows what to back up.
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

            // Save button. Validates nickname (non-empty) before
            // calling identity::save which mints the ID on first
            // save. Pushes the result into ChatClient so the next
            // hello carries it.
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
                    // Mirror nickname into UIManager so the chat
                    // connect path picks the same value up across
                    // reconnects.
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
                const ImVec4 nameColor = p.host
                    ? ImVec4(0.95f, 0.78f, 0.30f, 1.0f)
                    : (isMe ? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
                            : ImVec4(0.48f, 0.78f, 0.94f, 1.0f));
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
            if (isHostMsg)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f), "[HOST]");
                ImGui::SameLine();
            }
            // Local-author wins as the colour cue (you posted this),
            // then host (other host's messages stand out), then a
            // neutral viewer colour.
            ImVec4 nameColor;
            if (m.local)        nameColor = ImVec4(0.45f, 0.85f, 0.50f, 1.0f);
            else if (isHostMsg) nameColor = ImVec4(0.95f, 0.78f, 0.30f, 1.0f);
            else                nameColor = ImVec4(0.48f, 0.78f, 0.94f, 1.0f);
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
