// Asking before a command or action runs, and before a coding agent starts on
// its plan. What is asked about is in a read-only box, to read line by line.

#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/providers/provider.h"
#include "core/util.h"

namespace {

// Text on Windows' edit controls needs CR LF line ends.
wxString BoxText(const std::string& text) {
    wxString t = U(text);
#ifdef __WXMSW__
    t.Replace("\r\n", "\n");
    t.Replace("\n", "\r\n");
#endif
    return t;
}

wxTextCtrl* ReadOnlyBox(wxWindow* parent, const std::string& text, const wxSize& size) {
    auto* box = new wxTextCtrl(parent, wxID_ANY, BoxText(text), wxDefaultPosition, size,
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    box->SetInsertionPoint(0);
    return box;
}

}  // namespace

bool AskToAllow(wxWindow* parent, const ApprovalRequest& request, bool& allowAll) {
    wxDialog dlg(parent, wxID_ANY, request.title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* question = new wxStaticText(&dlg, wxID_ANY, request.question);
    question->Wrap(560);
    sizer->Add(question, 0, wxALL, 10);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, request.boxLabel), 0, wxLEFT | wxRIGHT, 10);
    wxTextCtrl* box = ReadOnlyBox(&dlg, request.what, wxSize(580, 140));
    sizer->Add(box, 1, wxEXPAND | wxALL, 10);
    auto* all = new wxCheckBox(&dlg, wxID_ANY, "Allow &everything without asking for the rest of this conversation");
    sizer->Add(all, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* yes = new wxButton(&dlg, wxID_YES, request.yesLabel);
    auto* no = new wxButton(&dlg, wxID_NO, request.noLabel);
    buttons->Add(yes, 0, wxRIGHT, 8);
    buttons->Add(no, 0);
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    dlg.SetSizerAndFit(sizer);
    yes->SetDefault();
    dlg.SetEscapeId(wxID_NO);
    yes->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_YES); });
    no->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_NO); });
    // Enter in the box answers as the default button does, as in any dialog.
    dlg.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& e) {
        int key = e.GetKeyCode();
        if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !e.HasAnyModifiers() && wxWindow::FindFocus() == box) {
            dlg.EndModal(wxID_YES);
            return;
        }
        e.Skip();
    });
    dlg.CentreOnParent();
    // Focus on what is asked about, so it is read first.
    box->SetFocus();
    bool allowed = dlg.ShowModal() == wxID_YES;
    allowAll = allowed && all->GetValue();
    return allowed;
}

PlanDecision AskPlanApproval(wxWindow* parent, const wxString& asker, const std::string& plan) {
    enum { ID_ACCEPT_EDITS = wxID_HIGHEST + 1, ID_ASK_EDITS, ID_KEEP };
    wxDialog dlg(parent, wxID_ANY, "Plan ready", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* question = new wxStaticText(&dlg, wxID_ANY, asker + " has a plan. Start on it?");
    sizer->Add(question, 0, wxALL, 10);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, "&Plan:"), 0, wxLEFT | wxRIGHT, 10);
    wxTextCtrl* box = ReadOnlyBox(&dlg, plan.empty() ? std::string("(no plan text)") : plan, wxSize(640, 320));
    sizer->Add(box, 1, wxEXPAND | wxALL, 10);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, "&What to change, to keep planning (optional):"), 0, wxLEFT | wxRIGHT, 10);
    auto* feedback = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxSize(640, 60), wxTE_MULTILINE);
    sizer->Add(feedback, 0, wxEXPAND | wxALL, 10);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* acceptEdits = new wxButton(&dlg, ID_ACCEPT_EDITS, "Start, &accepting edits");
    auto* askEdits = new wxButton(&dlg, ID_ASK_EDITS, "Start, asking &before edits");
    auto* keep = new wxButton(&dlg, ID_KEEP, "&Keep planning");
    buttons->Add(acceptEdits, 0, wxRIGHT, 8);
    buttons->Add(askEdits, 0, wxRIGHT, 8);
    buttons->Add(keep, 0);
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    dlg.SetSizerAndFit(sizer);
    dlg.SetEscapeId(ID_KEEP);
    for (int id : {static_cast<int>(ID_ACCEPT_EDITS), static_cast<int>(ID_ASK_EDITS), static_cast<int>(ID_KEEP)}) {
        dlg.Bind(wxEVT_BUTTON, [&dlg, id](wxCommandEvent&) { dlg.EndModal(id); }, id);
    }
    dlg.CentreOnParent();
    box->SetFocus();
    int answer = dlg.ShowModal();
    PlanDecision decision;
    decision.start = answer == ID_ACCEPT_EDITS || answer == ID_ASK_EDITS;
    decision.mode = answer == ID_ACCEPT_EDITS ? "acceptEdits" : answer == ID_ASK_EDITS ? "default" : "";
    if (!decision.start) decision.feedback = Trim(S(feedback->GetValue()));
    return decision;
}
