#include "ui/ui_util.h"
#include "core/providers/provider.h"

#include <wx/clipbrd.h>

void PostToUi(std::function<void()> fn) {
    if (wxTheApp) wxTheApp->CallAfter(std::move(fn));
}

bool CopyToClipboard(const std::string& text) {
    wxClipboardLocker locker;
    if (!locker) return false;
    return wxTheClipboard->SetData(new wxTextDataObject(U(text)));
}

bool AskYesNo(wxWindow* parent, const wxString& question, const wxString& title) {
    return wxMessageBox(question, title, wxYES_NO | wxICON_QUESTION, parent) == wxYES;
}

void ShowError(wxWindow* parent, const wxString& message, const wxString& title) {
    wxMessageBox(message, title, wxOK | wxICON_ERROR, parent);
}

wxBoxSizer* LabeledRow(wxWindow* parent, const wxString& label, wxWindow* control, int proportion) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxStaticText(parent, wxID_ANY, label);
    // Screen readers name a control after the label just before it in window
    // order, and the control was created first. wxWidgets reorders only its own
    // list; on Windows the native order has to move too.
    text->MoveBeforeInTabOrder(control);
#ifdef __WXMSW__
    HWND before = ::GetWindow(static_cast<HWND>(control->GetHWND()), GW_HWNDPREV);
    ::SetWindowPos(static_cast<HWND>(text->GetHWND()), before ? before : HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    row->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    row->Add(control, proportion, wxALIGN_CENTER_VERTICAL);
    return row;
}

wxChoice* MakeTriChoice(wxWindow* parent, bool globalValue) {
    auto* c = new wxChoice(parent, wxID_ANY);
    c->Append(globalValue ? "Default (on)" : "Default (off)");
    c->Append("On");
    c->Append("Off");
    c->SetSelection(0);
    return c;
}

void SetTriChoice(wxChoice* choice, const std::optional<bool>& value) {
    choice->SetSelection(!value ? 0 : (*value ? 1 : 2));
}

std::optional<bool> GetTriChoice(wxChoice* choice) {
    switch (choice->GetSelection()) {
        case 1: return true;
        case 2: return false;
        default: return std::nullopt;
    }
}

const std::vector<std::pair<std::string, wxString>>& EffortChoices() {
    static const std::vector<std::pair<std::string, wxString>> choices = {
        {"", "Model default"}, {"none", "None (least thinking)"}, {"low", "Low"}, {"medium", "Medium"},
        {"high", "High"},      {"xhigh", "Extra high"},           {"max", "Maximum"},
    };
    return choices;
}

std::string ModelDisplayName(const std::string& providerId, const std::string& modelId) {
    if (modelId.empty()) return "no model";
    ModelInfo info;
    if (AppProviderCache().FindModel(providerId, modelId, info) && !info.name.empty()) return info.name;
    return modelId;
}
