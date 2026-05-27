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
    // streamId" rule because the user needs the Rooms… button to
    // even be reachable. When the panel is idle the empty state
    // tells them to click Rooms….

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
        ImGui::TextDisabled("— %s", snap.lastError.c_str());
    }
    if (!snap.participants.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("• %zu", snap.participants.size());
    }
    // Show which room we're in (slug when standalone, otherwise the
    // host stream is implicit). The header gets crowded fast so this
    // sits on its own line when there's something to surface.
    if (!snap.slug.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("• #%s", snap.slug.c_str());
    }
    // Room-picker button — opens a popup with Join-by-slug + Create.
    // Right-aligned so it doesn't shift the status line on narrow
    // panels.
    ImGui::SameLine();
    {
        const float btnW = 60.0f;
        float regionW = ImGui::GetContentRegionAvail().x;
        if (regionW > btnW) ImGui::Dummy(ImVec2(regionW - btnW, 1));
        ImGui::SameLine();
        if (ImGui::SmallButton("Rooms…"))
        {
            ImGui::OpenPopup("##chatRoomsPopup");
            m_standaloneError.clear();
        }
    }
    if (ImGui::BeginPopup("##chatRoomsPopup"))
    {
        m_inputFocused = true; // keep F8 toggle from firing while in popup

        ImGui::Text("Join existing room");
        ImGui::SetNextItemWidth(220.0f);
        const bool joinEnter = ImGui::InputText(
            "##joinSlug", m_joinSlugBuf, sizeof(m_joinSlugBuf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool joinClick = ImGui::Button("Join");
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
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::Separator();
        ImGui::Text("Create new room");
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##createTitle", m_createTitleBuf, sizeof(m_createTitleBuf));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint(
            "##createSlug", "slug (optional, auto-generated)",
            m_createSlugBuf, sizeof(m_createSlugBuf));
        ImGui::BeginDisabled(m_createInFlight);
        if (ImGui::Button("Create + Join"))
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
                ImGui::CloseCurrentPopup();
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
            ImGui::Separator();
            if (ImGui::Button("Leave / disconnect"))
            {
                m_chat->disconnect();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    // ---- nickname row -------------------------------------------------
    if (!m_nickInitialized)
    {
        std::snprintf(m_nickBuf, sizeof(m_nickBuf), "%s", snap.nickname.c_str());
        m_nickInitialized = true;
    }
    ImGui::TextDisabled("as");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-60.0f);
    const bool nickEnterSubmit =
        ImGui::InputText("##chatNick", m_nickBuf, sizeof(m_nickBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActive()) m_inputFocused = true;
    if (ImGui::IsItemEdited())  m_nickError.clear();
    ImGui::SameLine();
    const std::string typedNick = m_nickBuf;
    const bool nickChanged      = (typedNick != snap.nickname);
    const bool nickEmpty        = typedNick.empty();
    ImGui::BeginDisabled(!nickChanged || nickEmpty);
    const bool applyClicked = ImGui::Button("Apply", ImVec2(-1.0f, 0.0f));
    ImGui::EndDisabled();
    if (nickEnterSubmit || applyClicked)
    {
        // Client-side dedup: refuse if the typed nickname is held by
        // any OTHER participant in the room. Self-match (same id) is
        // allowed because that's just a no-op rename to the current
        // value. v0.5 doesn't validate server-side — race conditions
        // can let two clients pick the same name; v1 will add a
        // server `nickname_taken` error frame.
        bool collision = false;
        for (const auto &part : snap.participants)
        {
            if (part.id != snap.myParticipantId && part.nickname == typedNick)
            {
                collision = true;
                break;
            }
        }
        if (nickEmpty)
        {
            m_nickError = "Nickname can't be empty";
        }
        else if (collision)
        {
            m_nickError = "Nickname already in use";
        }
        else if (nickChanged)
        {
            m_nickError.clear();
            // Persist BEFORE poking the transport — saveConfig writes
            // synchronously, and if the user kills the app mid-
            // reconnect the new nick is still in config.json.
            if (m_uiManager)
            {
                m_uiManager->setChatNickname(typedNick);
                m_uiManager->saveConfig();
            }
            m_chat->setNickname(typedNick);
        }
    }
    if (!m_nickError.empty())
    {
        ImGui::TextColored(ImVec4(0.85f, 0.33f, 0.31f, 1.0f), "%s",
                           m_nickError.c_str());
    }
    ImGui::Separator();

    // ---- message list -------------------------------------------------
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    if (ImGui::BeginChild("##chatLog",
                          ImVec2(0, -footerHeight),
                          false,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        if (snap.messages.empty())
        {
            ImGui::TextDisabled("(no messages yet — say hi)");
        }
        for (const auto &m : snap.messages)
        {
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
            ImGui::SameLine();
            // Mention check: someone else cited @<myNick>. Renders the
            // body in a warm tint so the row stands out at a glance.
            const bool isMention = !m.deleted &&
                                   !m.local &&
                                   !snap.nickname.empty() &&
                                   mentionsNick(m.body, snap.nickname);
            if (m.deleted)
            {
                ImGui::TextDisabled("%s", m.body.c_str());
            }
            else if (isMention)
            {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.95f, 0.85f, 0.55f, 1.0f));
                ImGui::TextWrapped("%s", m.body.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextWrapped("%s", m.body.c_str());
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
    // Why InputTextMultiline at single-row height instead of plain
    // InputText: ImGui's single-line InputText deactivates the widget
    // on every Enter (regardless of EnterReturnsTrue). The user has
    // to click back into the field for the next message. Refocus
    // tricks (SetKeyboardFocusHere -1 same-frame, flag + 0 next-
    // frame, both pattern variants from imgui_demo.cpp) all bounce
    // off the actual focus state on this build. Sidestepping the
    // problem entirely: use the multi-line widget at single-row
    // visual height, where Enter inserts '\n' into the buffer
    // instead of deactivating. We detect the newline as our send
    // trigger and clear the buffer in place — focus never moves.
    const bool sendable = snap.state == ChatClient::State::Connected;
    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginDisabled(!sendable);
    const ImVec2 inputSize(
        ImGui::GetContentRegionAvail().x - 60.0f - ImGui::GetStyle().ItemSpacing.x,
        rowH);
    ImGui::InputTextMultiline("##chatInput", m_inputBuf, sizeof(m_inputBuf),
                              inputSize,
                              ImGuiInputTextFlags_NoHorizontalScroll);
    if (ImGui::IsItemActive()) m_inputFocused = true;
    ImGui::SameLine();
    const bool sendClicked = ImGui::Button("Send", ImVec2(-1.0f, rowH));
    ImGui::EndDisabled();

    // Detect Enter via the '\n' the multi-line widget appended.
    char *nlPos = std::strchr(m_inputBuf, '\n');
    const bool enterSubmit = (nlPos != nullptr);

    if ((enterSubmit || sendClicked) && sendable)
    {
        // Truncate at the first newline so the body is everything
        // the user typed BEFORE pressing Enter; anything they pasted
        // with embedded newlines past that gets dropped (mirrors a
        // single-line input's behaviour from the user's POV).
        if (nlPos) *nlPos = '\0';
        std::string body = m_inputBuf;
        while (!body.empty() &&
               (body.back() == '\r' || body.back() == ' ' || body.back() == '\t'))
        {
            body.pop_back();
        }
        if (!body.empty())
        {
            m_chat->post(body);
        }
        // Clear buffer regardless — even empty Enter strokes shouldn't
        // leave a stray \n behind.
        m_inputBuf[0] = '\0';
    }

    // Reset unread counter while the panel is on screen and current.
    if (sendable && m_autoScroll) m_chat->markRead();

    ImGui::End();
}
