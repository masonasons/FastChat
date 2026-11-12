#include "ui/dialogs.h"
#include "ui/ui_util.h"

void ShowTextDialog(wxWindow* parent, const wxString& title, const wxString& text) {
    wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxSize(720, 520),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* label = new wxStaticText(&dlg, wxID_ANY, "&Text");
    auto* edit = new wxTextCtrl(&dlg, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    sizer->Add(edit, 1, wxEXPAND | wxALL, 8);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* copy = new wxButton(&dlg, wxID_ANY, "&Copy all");
    auto* close = new wxButton(&dlg, wxID_CANCEL, "Close");
    buttons->Add(copy, 0, wxRIGHT, 8);
    buttons->Add(close, 0);
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    copy->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { CopyToClipboard(S(text)); });
    dlg.SetSizer(sizer);
    dlg.SetEscapeId(wxID_CANCEL);
    edit->SetFocus();
    edit->SetInsertionPoint(0);
    dlg.ShowModal();
}
