#include "ui/activity_window.h"
#include "ui/ui_util.h"
#include "core/util.h"

#include <cstdio>
#include <filesystem>

namespace {

std::string Seconds(int64_t ms) {
    if (ms < 0) return std::string();
    int64_t s = ms / 1000;
    if (s < 60) return std::to_string(s) + (s == 1 ? " second" : " seconds");
    return std::to_string(s / 60) + " min " + std::to_string(s % 60) + " s";
}

int64_t Elapsed(const Activity& a) {
    if (a.info.durationMs >= 0 && !a.Running()) return a.info.durationMs;
    int64_t end = a.endedMs ? a.endedMs : NowMillis();
    return a.startedMs ? end - a.startedMs : -1;
}

// The end of a file that is still being written (a background shell's output).
std::string Tail(const std::string& path, size_t maxBytes) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return std::string();
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long start = size > static_cast<long>(maxBytes) ? size - static_cast<long>(maxBytes) : 0;
    std::fseek(f, start, SEEK_SET);
    std::string data(static_cast<size_t>(size - start), '\0');
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    data.resize(got);
    return (start > 0 ? "...\n" : "") + SanitizeUtf8(data);
}

}  // namespace

std::string ActivityTitle(const Activity& a) {
    const ActivityEvent& i = a.info;
    std::string desc = i.description.empty() ? i.id : i.description;
    if (i.kind == "agent") return "Agent '" + desc + "'" + (i.agentType.empty() ? "" : " (" + i.agentType + ")");
    if (i.kind == "shell") return (i.background ? "Background shell '" : "Shell '") + desc + "'";
    return "Tool: " + desc;
}

std::string ActivityRow(const Activity& a) {
    const ActivityEvent& i = a.info;
    std::string row = ActivityTitle(a) + ": " + (a.Running() ? std::string("running") : i.status);
    if (i.toolUses > 0) row += ", " + std::to_string(i.toolUses) + (i.toolUses == 1 ? " tool use" : " tool uses");
    if (a.Running() && !i.progress.empty()) row += ", " + OneLine(i.progress, 120);
    return row;
}

std::string ActivityDetails(const Activity& a) {
    const ActivityEvent& i = a.info;
    std::string t = ActivityTitle(a) + "\n";
    t += "Status: " + (a.Running() ? std::string("running") : i.status) + "\n";
    std::string time = Seconds(Elapsed(a));
    if (!time.empty()) t += (a.Running() ? "Running for: " : "Took: ") + time + "\n";
    if (i.toolUses >= 0) t += "Tool uses: " + std::to_string(i.toolUses) + "\n";
    if (i.tokens >= 0) t += "Tokens: " + FormatThousands(i.tokens) + "\n";
    if (!i.lastTool.empty()) t += "Last tool: " + i.lastTool + "\n";
    if (!i.progress.empty()) t += "Latest step: " + i.progress + "\n";
    if (!i.prompt.empty()) t += "\n" + std::string(i.kind == "shell" ? "Command" : "Asked to") + ":\n" + i.prompt + "\n";
    if (!a.lines.empty()) {
        t += i.kind == "agent" ? "\nSteps:\n" : "\nOutput:\n";
        for (const auto& line : a.lines) t += (i.kind == "agent" ? "- " : "") + line + "\n";
    }
    if (!i.outputFile.empty()) {
        std::string output = Tail(i.outputFile, 16000);
        t += "\nOutput" + std::string(a.Running() ? " so far" : "") + ":\n" +
             (Trim(output).empty() ? std::string("(none yet)") : output) + "\n";
    }
    if (!i.summary.empty()) t += "\nSummary:\n" + i.summary + "\n";
    return t;
}

ActivityWindow::ActivityWindow(wxWindow* parent, const wxString& conversation,
                               std::function<const std::vector<Activity>&()> source)
    : wxFrame(parent, wxID_ANY, "Activity - " + conversation, wxDefaultPosition, wxSize(760, 560),
              wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT),
      m_source(std::move(source)),
      m_timer(this) {
    auto* panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(panel, wxID_ANY, "&Running and finished"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
    m_list = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 180));
    sizer->Add(m_list, 1, wxEXPAND | wxALL, 6);
    sizer->Add(new wxStaticText(panel, wxID_ANY, "&Details"), 0, wxLEFT | wxRIGHT, 6);
    m_details = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    sizer->Add(m_details, 2, wxEXPAND | wxALL, 6);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh = new wxButton(panel, wxID_ANY, "&Refresh (F5)");
    auto* close = new wxButton(panel, wxID_CLOSE, "&Close");
    buttons->Add(refresh, 0, wxRIGHT, 6);
    buttons->Add(close, 0);
    sizer->Add(buttons, 0, wxALL, 6);
    panel->SetSizer(sizer);

    m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { Update(true); });
    m_list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER) {
            Update(true);
            m_details->SetFocus();
            m_details->SetInsertionPoint(0);
            return;
        }
        e.Skip();
    });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Update(true); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            Close();
            return;
        }
        if (e.GetKeyCode() == WXK_F5) {
            Update(true);
            return;
        }
        e.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Update(false); });
    m_timer.Start(1000);
    Update(true);
    const auto& all = m_source();
    if (!all.empty()) {
        // Start on the most recent thing still running, else the last one.
        int select = static_cast<int>(all.size()) - 1;
        for (int i = static_cast<int>(all.size()) - 1; i >= 0; i--) {
            if (all[i].Running()) {
                select = i;
                break;
            }
        }
        m_list->SetSelection(select);
        Update(true);
    }
    m_list->SetFocus();
}

void ActivityWindow::Update(bool force) {
    const auto& all = m_source();
    m_list->Freeze();
    if (all.empty()) {
        if (m_list->GetCount() != 1 || m_list->GetString(0) != "Nothing has run yet in this conversation.") {
            m_list->Clear();
            m_list->Append("Nothing has run yet in this conversation.");
        }
    } else {
        if (m_list->GetCount() == 1 && m_list->GetString(0) == "Nothing has run yet in this conversation.") m_list->Clear();
        // Rows change only when their words do, so a screen reader is not made
        // to read the selected one again every second.
        for (size_t i = 0; i < all.size(); i++) {
            wxString row = U(ActivityRow(all[i]));
            if (i < m_list->GetCount()) {
                if (m_list->GetString(static_cast<unsigned>(i)) != row) m_list->SetString(static_cast<unsigned>(i), row);
            } else {
                m_list->Append(row);
            }
        }
        while (m_list->GetCount() > all.size()) m_list->Delete(m_list->GetCount() - 1);
    }
    m_list->Thaw();

    // The details, unless the user is reading them (F5 updates them then).
    if (!force && m_details->HasFocus()) return;
    int sel = m_list->GetSelection();
    wxString details;
    if (sel >= 0 && sel < static_cast<int>(all.size())) details = U(ActivityDetails(all[static_cast<size_t>(sel)]));
    if (details != m_shownDetails) {
        m_shownDetails = details;
        m_details->ChangeValue(details);
    }
}
