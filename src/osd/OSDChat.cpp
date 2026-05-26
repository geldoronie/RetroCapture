#include "OSDChat.h"

#include "../chat/ChatClient.h"
#include "../ui/UIManager.h"

#include <imgui.h>

#include <algorithm>
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

    // Nothing to subscribe to — collapse the overlay rather than
    // surface an empty box.
    if (snap.state == ChatClient::State::Idle && snap.streamId.empty())
    {
        return;
    }

    // Top-right anchor. Width is fixed; height grows with content up
    // to a cap so the overlay doesn't cover the viewport.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float width  = 340.0f;
    const float height = std::clamp(vp->WorkSize.y * 0.45f, 240.0f, 480.0f);
    const ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x - 16.0f,
                        vp->WorkPos.y + 16.0f);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);

    // Like QuickActionsOverlay: no decoration, no save, anchored every
    // frame. NoNav so arrow keys don't get hijacked by ImGui nav.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoMove     |
                                   ImGuiWindowFlags_NoResize   |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("##chatOSD", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    // ---- header line --------------------------------------------------
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Chat");
    ImGui::SameLine();
    ImGui::TextColored(stateColor(snap.state), "(%s)", stateLabel(snap.state));
    if (!snap.lastError.empty() && snap.state != ChatClient::State::Connected)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(" — %s", snap.lastError.c_str());
    }
    // Participant count, when we know it.
    if (!snap.participants.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled(" • %zu", snap.participants.size());
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
    ImGui::SetNextItemWidth(-50.0f);
    if (ImGui::InputText("##chatNick", m_nickBuf, sizeof(m_nickBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
    {
        const std::string newNick = m_nickBuf;
        if (newNick != snap.nickname && !newNick.empty())
        {
            m_chat->setNickname(newNick);
        }
    }
    if (ImGui::IsItemActive()) m_inputFocused = true;
    ImGui::SameLine();
    ImGui::TextDisabled("apply⏎"); // U+23CE Return symbol
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
            const ImVec4 nameColor = m.local
                ? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
                : ImVec4(0.48f, 0.78f, 0.94f, 1.0f);
            ImGui::TextColored(nameColor, "%s:", m.nickname.c_str());
            ImGui::SameLine();
            if (m.deleted)
            {
                ImGui::TextDisabled("%s", m.body.c_str());
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
    const bool sendable = snap.state == ChatClient::State::Connected;
    ImGui::BeginDisabled(!sendable);
    ImGui::SetNextItemWidth(-60.0f);
    bool submitted = ImGui::InputText("##chatInput", m_inputBuf, sizeof(m_inputBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemActive()) m_inputFocused = true;
    ImGui::SameLine();
    if (ImGui::Button("Send", ImVec2(-1.0f, 0.0f))) submitted = true;
    ImGui::EndDisabled();

    if (submitted && sendable)
    {
        std::string body = m_inputBuf;
        // Trim trailing newline / whitespace ImGui's InputText
        // sometimes attaches when Enter triggered the submit.
        while (!body.empty() &&
               (body.back() == '\n' || body.back() == '\r' ||
                body.back() == ' '  || body.back() == '\t'))
        {
            body.pop_back();
        }
        if (!body.empty())
        {
            m_chat->post(body);
            m_inputBuf[0] = '\0';
        }
    }

    // Reset unread counter while the panel is on screen and current.
    if (sendable && m_autoScroll) m_chat->markRead();

    ImGui::End();
}
