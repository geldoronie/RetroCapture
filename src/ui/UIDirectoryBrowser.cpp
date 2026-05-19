#include "UIDirectoryBrowser.h"
#include "UIManager.h"
#include "UIRemoteConnection.h"
#include "UISectionHeader.h"
#include "../streaming/DirectoryBrowser.h"
#include "../utils/HttpClient.h"
#include "../utils/PasswordHash.h"
#include "../utils/TranslationManager.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif

namespace
{
    bool containsCI(const std::string &haystack, const std::string &needle)
    {
        if (needle.empty()) return true;
        std::string h = haystack;
        std::string n = needle;
        for (auto &c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto &c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return h.find(n) != std::string::npos;
    }

    const char *endpointModeIcon(const std::string &mode)
    {
        if (mode == "tunnel-cloudflare") return "tunnel";
        if (mode == "custom")            return "custom";
        return "direct";
    }

    // Draws a small padlock at the current cursor using ImDrawList
    // primitives. The default ImGui font ships ASCII + Latin-1 only, so
    // U+1F512 renders as the missing-glyph fallback ('?') — drawing
    // primitives sidestep the font question entirely.
    void drawPadlockIcon()
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float scale = ImGui::GetFontSize() / 13.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        const float bodyW   = 10.0f * scale;
        const float bodyH   =  7.0f * scale;
        const float radius  =  3.0f * scale;
        const float thick   =  1.5f * scale;
        const ImU32 color   = ImGui::GetColorU32(ImGuiCol_Text);

        // Vertical centering inside one text line.
        const float lineH = ImGui::GetTextLineHeight();
        const float iconH = bodyH + radius + thick * 0.5f;
        const float yPad  = (lineH - iconH) * 0.5f;
        const float x0    = origin.x;
        const float y0    = origin.y + (yPad > 0.0f ? yPad : 0.0f);

        const float PI = 3.14159265358979323846f;
        const ImVec2 shackleCenter(x0 + bodyW * 0.5f, y0 + radius);
        dl->PathArcTo(shackleCenter, radius, PI, 2.0f * PI, 16);
        dl->PathStroke(color, ImDrawFlags_None, thick);

        const ImVec2 bodyTL(x0,             y0 + radius);
        const ImVec2 bodyBR(x0 + bodyW,     y0 + radius + bodyH);
        dl->AddRectFilled(bodyTL, bodyBR, color, 1.0f * scale);

        // Reserve layout space so the cell behaves like a normal text
        // row (hover hit-testing, table sizing, etc.).
        ImGui::Dummy(ImVec2(bodyW, lineH));
    }
}

UIDirectoryBrowser::UIDirectoryBrowser(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIDirectoryBrowser::~UIDirectoryBrowser() = default;

void UIDirectoryBrowser::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(T("browse.title").c_str(), &m_visible))
    {
        renderTable();
    }
    ImGui::End();

    // The password modal is opened from inside renderTable but
    // BeginPopupModal must run unconditionally each frame the popup
    // is on screen; rendering it here keeps it visible even if the
    // main window scrolls away.
    renderPasswordModal();
    renderReportModal();
    renderReportFeedbackModal();
}

void UIDirectoryBrowser::renderTable()
{
    if (!m_browser)
    {
        ImGui::TextDisabled("Directory browser unavailable.");
        return;
    }

    ui_section_header(T("browse.title").c_str(), T("browse.intro").c_str());

    {
        char urlBuf[256];
        std::snprintf(urlBuf, sizeof(urlBuf), "%s", m_uiManager->getDirectoryUrl().c_str());
        ImGui::Text("%s", T("browse.directory").c_str());
        ImGui::SetNextItemWidth(-120);
        if (ImGui::InputText("##dirUrl", urlBuf, sizeof(urlBuf)))
        {
            m_uiManager->setDirectoryUrl(urlBuf);
            m_uiManager->saveConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("browse.refresh").c_str(), ImVec2(112, 0)))
        {
            m_browser->refreshNow();
        }
    }

    {
        const std::string lblViewers = T("browse.sort.viewers");
        const std::string lblRecent  = T("browse.sort.recent");
        const std::string lblName    = T("browse.sort.name");
        const char *sortLabels[] = { lblViewers.c_str(), lblRecent.c_str(), lblName.c_str() };
        ImGui::SetNextItemWidth(180);
        ImGui::Combo(T("browse.sort").c_str(), &m_sortIndex, sortLabels, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##browseSearch", T("browse.search_hint").c_str(),
                                 m_search, sizeof(m_search));
    }

    auto snap = m_browser->getSnapshot();

    // Local sort.
    auto entries = snap.entries;
    switch (m_sortIndex)
    {
        case 1:
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          return a.expiresAt > b.expiresAt;
                      });
            break;
        case 2:
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          auto la = a.name, lb = b.name;
                          for (auto &c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          for (auto &c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          return la < lb;
                      });
            break;
        default:
            break;
    }

    // Filter.
    const std::string filter = m_search;
    if (!filter.empty())
    {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&filter](const DirectoryBrowser::Entry &e) {
                                         return !containsCI(e.name, filter) &&
                                                !containsCI(e.hostNickname, filter);
                                     }),
                      entries.end());
    }

    ImGui::Spacing();
    if (!snap.lastError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "%s: %s", T("browse.last_fetch_error").c_str(), snap.lastError.c_str());
    }

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_BordersInnerH
                                     | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##dirTable", 10, tableFlags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing())))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(T("browse.col.name").c_str(),     ImGuiTableColumnFlags_WidthStretch, 3.0f);
        // Tiny status column for the password padlock. Header is short
        // text since the default ImGui font can't render U+1F512; the
        // padlock glyph in each cell is drawn via ImDrawList primitives.
        ImGui::TableSetupColumn(T("browse.col.pwd").c_str(), ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn(T("browse.col.host").c_str(),     ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(T("browse.col.shader").c_str(),   ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("Res\xc3\x97""FPS",  ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn(T("browse.col.codec").c_str(),    ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn(T("browse.col.clients").c_str(), ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn(T("browse.col.mode").c_str(),     ImGuiTableColumnFlags_WidthFixed, 80.0f);
        // Explicit action buttons per row. Previously the user clicked
        // the row to connect and right-clicked for Report; the context
        // menu wasn't discoverable and click-anywhere-to-connect was
        // easy to trigger accidentally. Both actions are now their
        // own column.
        ImGui::TableSetupColumn("##connect", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("##report",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        if (entries.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string emptyMsg = snap.totalCount == 0
                                             ? T("browse.no_streams")
                                             : T("browse.no_matches");
            ImGui::TextDisabled("%s", emptyMsg.c_str());
        }

        for (const auto &e : entries)
        {
            ImGui::PushID(e.streamId.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool versionMismatch = !e.version.empty() &&
                                         e.version != std::string(RETROCAPTURE_VERSION);

            // Name cell: plain text with optional ⚠ prefix when the
            // host announces a different protocol version.
            if (versionMismatch)
            {
                ImGui::TextUnformatted("\xe2\x9a\xa0");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(T("browse.version_mismatch.tip").c_str(),
                                      e.version.c_str(), RETROCAPTURE_VERSION);
                }
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(e.name.c_str());

            ImGui::TableNextColumn();
            if (e.passwordRequired)
            {
                drawPadlockIcon();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("browse.pwd.required").c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.hostNickname.empty() ? "\xe2\x80\x94" : e.hostNickname.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.shader.empty() ? "\xe2\x80\x94" : e.shader.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%ux%u@%u", e.resolutionW, e.resolutionH, e.fps);
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.codec.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", e.clientCount);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", endpointModeIcon(e.endpointMode));

            // Connect button — column 9. Stretches to fill the
            // 80-px column so the click target is generous. Triggers
            // the password modal first when the stream is gated.
            ImGui::TableNextColumn();
            if (ImGui::Button(T("browse.col.connect").c_str(), ImVec2(-1, 0)))
            {
                if (e.passwordRequired)
                {
                    m_pendingProtectedUrl = e.endpoint;
                    m_passwordBuffer[0]   = '\0';
                    m_showPasswordModal   = true;
                }
                else if (m_remoteWindow)
                {
                    if (m_uiManager) m_uiManager->setRemoteAuthToken("");
                    m_remoteWindow->triggerConnect(e.endpoint);
                }
            }

            // Report button — column 10. Opens the same modal the
            // old right-click context menu used.
            ImGui::TableNextColumn();
            if (ImGui::Button(T("browse.col.report").c_str(), ImVec2(-1, 0)))
            {
                m_reportStreamId   = e.streamId;
                m_reportStreamName = e.name;
                m_reportReason[0]  = '\0';
                m_reportContact[0] = '\0';
                {
                    std::lock_guard<std::mutex> lock(m_reportMu);
                    m_reportStatus = ReportStatus::Idle;
                    m_reportError.clear();
                    m_reportReceiptId.clear();
                }
                m_showReportModal = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", T("browse.report.tooltip").c_str());
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(T("browse.showing").c_str(), entries.size(), snap.totalCount);
}

void UIDirectoryBrowser::renderPasswordModal()
{
    // The popup ID must match between OpenPopup and BeginPopupModal,
    // so use a fixed ID and a translated title via the ###id trick.
    const std::string pwTitle = T("browse.password.title") + "###StreamPasswordPopup";
    if (m_showPasswordModal)
    {
        ImGui::OpenPopup("###StreamPasswordPopup");
        m_showPasswordModal = false;
    }
    if (ImGui::BeginPopupModal(pwTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", T("browse.password.intro").c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(280);
        bool submitted = ImGui::InputText("##pw", m_passwordBuffer, sizeof(m_passwordBuffer),
                                          ImGuiInputTextFlags_Password |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const bool accept = ImGui::Button(T("browse.password.connect").c_str(), ImVec2(120, 0)) || submitted;
        ImGui::SameLine();
        if (ImGui::Button(T("browse.password.cancel").c_str(), ImVec2(120, 0)))
        {
            m_pendingProtectedUrl.clear();
            std::memset(m_passwordBuffer, 0, sizeof(m_passwordBuffer));
            ImGui::CloseCurrentPopup();
        }
        if (accept && !m_pendingProtectedUrl.empty())
        {
            if (m_uiManager)
            {
                m_uiManager->setRemoteAuthToken(
                    PasswordHash::sha256Hex(std::string(m_passwordBuffer)));
            }
            if (m_remoteWindow) m_remoteWindow->triggerConnect(m_pendingProtectedUrl);
            m_pendingProtectedUrl.clear();
            std::memset(m_passwordBuffer, 0, sizeof(m_passwordBuffer));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─────────────────────────────────────────────────────────────────────
// Report-this-stream modal (#57)
//
// User opens it from the row context menu. Reason is required; contact
// is optional. POST /streams/<id>/report is rate-limited at 30/hour
// per source IP server-side, so the modal surfaces 429s clearly.
// There's deliberately no feedback about what the maintainer did with
// the report — the spec is firm that this is a one-way flag, not a
// community-moderation system.
// ─────────────────────────────────────────────────────────────────────
void UIDirectoryBrowser::renderReportModal()
{
    const std::string rpTitle = T("browse.report.title") + "###ReportStreamPopup";
    if (m_showReportModal) ImGui::OpenPopup("###ReportStreamPopup");
    if (!ImGui::BeginPopupModal(rpTitle.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::Text("%s: %s", T("browse.report.stream").c_str(),
                m_reportStreamName.empty() ? "(unnamed)" : m_reportStreamName.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", T("browse.report.intro").c_str());
    ImGui::Spacing();
    ImGui::Text("%s", T("browse.report.reason").c_str());
    ImGui::SetNextItemWidth(420);
    ImGui::InputTextMultiline("##reportReason", m_reportReason,
                              sizeof(m_reportReason),
                              ImVec2(420, 80));
    ImGui::Spacing();
    ImGui::Text("%s", T("browse.report.contact").c_str());
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##reportContact", m_reportContact, sizeof(m_reportContact));

    ImGui::Spacing();
    ReportStatus statusSnap;
    std::string  errSnap;
    std::string  receiptSnap;
    {
        std::lock_guard<std::mutex> lock(m_reportMu);
        statusSnap  = m_reportStatus;
        errSnap     = m_reportError;
        receiptSnap = m_reportReceiptId;
    }

    if (statusSnap == ReportStatus::Success)
    {
        // Hand off to the dedicated feedback modal. We dismiss this
        // input modal in the same frame and open the feedback one,
        // which renders the confirmation message + protocol number.
        // Reset status so a later "Report another stream" cycle
        // doesn't replay this branch on the input modal.
        {
            std::lock_guard<std::mutex> lock(m_reportMu);
            m_reportFeedbackReceiptId = receiptSnap;
            m_reportStatus = ReportStatus::Idle;
        }
        m_showReportFeedbackModal = true;
        m_showReportModal         = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (statusSnap == ReportStatus::Failed)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                           "%s", T("browse.report.submit_failed").c_str());
        ImGui::TextWrapped("%s", errSnap.c_str());
        ImGui::Spacing();
    }

    const bool busy        = (statusSnap == ReportStatus::Sending);
    const bool reasonOk    = (std::strlen(m_reportReason) > 0 &&
                              std::strlen(m_reportReason) <= 200);

    ImGui::BeginDisabled(busy || !reasonOk || m_reportStreamId.empty());
    const std::string submitLabel = busy ? T("browse.report.sending") : T("browse.report.submit");
    if (ImGui::Button(submitLabel.c_str(), ImVec2(120, 0)))
    {
        {
            std::lock_guard<std::mutex> lock(m_reportMu);
            m_reportStatus = ReportStatus::Sending;
            m_reportError.clear();
        }
        m_reportInFlight.store(true);
        // Snapshot the fields before kicking off the thread — the
        // user can still close the modal while the POST is in flight.
        const std::string directoryUrl = m_uiManager
            ? m_uiManager->getDirectoryUrl() : std::string();
        const std::string streamId = m_reportStreamId;
        std::string reason  = m_reportReason;
        std::string contact = m_reportContact;
        std::thread([this, directoryUrl, streamId, reason, contact]() {
            // Trim trailing slash from the directory URL so we don't
            // produce //streams/<id>/report.
            std::string base = directoryUrl;
            while (!base.empty() && base.back() == '/') base.pop_back();
            const std::string url = base + "/streams/" + streamId + "/report";

            // Build the JSON body. nlohmann::json is already pulled in
            // by DirectoryClient; using string concat instead keeps
            // this translation unit free of one more include.
            auto escapeJson = [](const std::string &s) {
                std::string out;
                out.reserve(s.size() + 2);
                for (char c : s)
                {
                    switch (c)
                    {
                        case '"':  out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\n': out += "\\n";  break;
                        case '\r': out += "\\r";  break;
                        case '\t': out += "\\t";  break;
                        default:
                            if (static_cast<unsigned char>(c) < 0x20)
                            {
                                char buf[8];
                                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                                out += buf;
                            }
                            else out += c;
                    }
                }
                return out;
            };
            std::string body = "{\"reason\":\"" + escapeJson(reason) + "\"";
            if (!contact.empty())
            {
                body += ",\"reporterContact\":\"" + escapeJson(contact) + "\"";
            }
            body += "}";

            HttpClient::Response resp = HttpClient::send(
                HttpClient::Method::POST, url, body, /*timeoutMs=*/5000);

            std::lock_guard<std::mutex> lock(m_reportMu);
            if (resp.ok && resp.statusCode >= 200 && resp.statusCode < 300)
            {
                m_reportStatus = ReportStatus::Success;
                // Parse the reportId out of the response. Tiny ad-hoc
                // parser to avoid pulling nlohmann::json into this TU
                // for one field; the response shape is fixed at
                // {"reportId":"R-XXXXXXXX"}.
                m_reportReceiptId.clear();
                const std::string &b = resp.body;
                const std::string key = "\"reportId\"";
                size_t k = b.find(key);
                if (k != std::string::npos)
                {
                    size_t colon = b.find(':', k + key.size());
                    if (colon != std::string::npos)
                    {
                        size_t q1 = b.find('"', colon + 1);
                        if (q1 != std::string::npos)
                        {
                            size_t q2 = b.find('"', q1 + 1);
                            if (q2 != std::string::npos && q2 > q1 + 1)
                            {
                                m_reportReceiptId = b.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
            }
            else
            {
                m_reportStatus = ReportStatus::Failed;
                if (resp.statusCode == 429)
                {
                    m_reportError = T("browse.report.rate_limited");
                    if (!resp.retryAfter.empty())
                    {
                        m_reportError += " (Retry-After: " + resp.retryAfter + " s)";
                    }
                }
                else if (resp.statusCode > 0)
                {
                    m_reportError = "Directory returned HTTP " +
                                    std::to_string(resp.statusCode);
                    if (!resp.body.empty())
                    {
                        size_t nl = resp.body.find('\n');
                        m_reportError += ": " +
                            resp.body.substr(0, nl == std::string::npos
                                                    ? resp.body.size() : nl);
                    }
                }
                else
                {
                    m_reportError = resp.error.empty()
                        ? T("browse.report.couldnt_reach")
                        : resp.error;
                }
            }
            m_reportInFlight.store(false);
        }).detach();
    }
    ImGui::EndDisabled();
    if (!reasonOk && std::strlen(m_reportReason) == 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("browse.report.reason_required").c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(T("browse.report.cancel").c_str(), ImVec2(120, 0)))
    {
        m_showReportModal = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

// ─────────────────────────────────────────────────────────────────────
// Report-feedback modal (#57 follow-up)
//
// Opened by renderReportModal() the moment a submit succeeds. The
// input modal closes; this modal opens fresh. Single concern: tell
// the user their report landed and give them the protocol number to
// quote. Independent state from the input modal so the user can
// dismiss this one without re-arming the report form.
// ─────────────────────────────────────────────────────────────────────
void UIDirectoryBrowser::renderReportFeedbackModal()
{
    const std::string fbTitle = T("browse.report_sent.title") + "###ReportSentPopup";
    if (m_showReportFeedbackModal) ImGui::OpenPopup("###ReportSentPopup");
    if (!ImGui::BeginPopupModal(fbTitle.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextWrapped("%s", T("browse.report_sent.thanks").c_str());
    ImGui::Spacing();

    if (!m_reportFeedbackReceiptId.empty())
    {
        ImGui::Text("%s", T("browse.report_sent.protocol").c_str());
        ImGui::SameLine();
        char receiptBuf[64];
        std::snprintf(receiptBuf, sizeof(receiptBuf), "%s",
                      m_reportFeedbackReceiptId.c_str());
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##feedbackReceipt", receiptBuf, sizeof(receiptBuf),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::TextDisabled("%s", T("browse.report_sent.quote_hint").c_str());
    }
    else
    {
        // Service didn't echo a reportId (older deploy, or transport
        // dropped the body). The report still landed — the 2xx we
        // got back is the signal — but there's no number to hand
        // over. Be honest about it.
        ImGui::TextDisabled("%s", T("browse.report_sent.no_protocol").c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button(T("browse.report_sent.close").c_str(), ImVec2(120, 0)))
    {
        m_showReportFeedbackModal = false;
        m_reportFeedbackReceiptId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
