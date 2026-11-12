#pragma once

// What is running alongside a conversation - subagents, background shells,
// tools - and the window that shows them (Ctrl+T in a conversation).

#include "core/providers/provider.h"

#include <wx/wx.h>

#include <functional>
#include <string>
#include <vector>

struct Activity {
    ActivityEvent info;               // the latest of everything reported
    std::vector<std::string> lines;   // a subagent's steps, a tool's output
    int64_t startedMs = 0;
    int64_t endedMs = 0;
    bool Running() const { return info.status.empty() || info.status == "running"; }
};

// "Agent 'count files' (general-purpose)", "Shell 'Ping localhost'", "Tool: Read src/main.cpp"
std::string ActivityTitle(const Activity& a);
// One line for a list: the title, its state, how many tools it used.
std::string ActivityRow(const Activity& a);
// Everything about it, for reading: prompt, steps, output, summary.
std::string ActivityDetails(const Activity& a);

class ActivityWindow : public wxFrame {
public:
    // `source` gives the conversation's activities (it outlives this window).
    ActivityWindow(wxWindow* parent, const wxString& conversation, std::function<const std::vector<Activity>&()> source);
    // Bring the list and the selected item's details up to date. `force`
    // also replaces details the user is reading.
    void Update(bool force);

private:
    std::function<const std::vector<Activity>&()> m_source;
    wxListBox* m_list;
    wxTextCtrl* m_details;
    wxTimer m_timer;
    wxString m_shownDetails;
};
