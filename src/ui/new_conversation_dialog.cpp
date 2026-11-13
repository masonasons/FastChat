// New conversation: a name (optional), chat or code, the folder for code, and
// the model (the default unless changed).

#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/util.h"

#include <wx/dirdlg.h>
#include <wx/filename.h>

namespace {

class NewConversationDialog : public wxDialog {
public:
    explicit NewConversationDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "New conversation", wxDefaultPosition, wxSize(560, -1)) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_name = new wxTextCtrl(this, wxID_ANY);
        m_name->SetHint("Leave empty to name it automatically");
        sizer->Add(LabeledRow(this, "&Name:", m_name), 0, wxEXPAND | wxALL, 8);

        wxString types[] = {"&Chat", "C&ode (work on files in a folder)", "Com&puter use (operate this computer)"};
        m_type = new wxRadioBox(this, wxID_ANY, "Type", wxDefaultPosition, wxDefaultSize, 3, types, 1, wxRA_SPECIFY_COLS);
        sizer->Add(m_type, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

        auto* folderRow = new wxBoxSizer(wxHORIZONTAL);
        m_folderLabel = new wxStaticText(this, wxID_ANY, "&Folder:");
        // Code conversations start in the base folder from the settings, where a
        // project's name is enough.
        const std::string& base = AppSettings().general.codeFolder;
        m_folder = new wxTextCtrl(this, wxID_ANY, U(base));
        if (!base.empty()) m_folder->SetHint("A project's name in " + U(base) + ", or any folder");
        m_browse = new wxButton(this, wxID_ANY, "&Browse...");
        folderRow->Add(m_folderLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        folderRow->Add(m_folder, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        folderRow->Add(m_browse, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(folderRow, 0, wxEXPAND | wxALL, 8);

        auto* modelRow = new wxBoxSizer(wxHORIZONTAL);
        m_modelLabel = new wxStaticText(this, wxID_ANY, "");
        auto* change = new wxButton(this, wxID_ANY, "Change &model...");
        modelRow->Add(m_modelLabel, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        modelRow->Add(change, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(modelRow, 0, wxEXPAND | wxALL, 8);

        auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
        SetSizerAndFit(sizer);
        SetSize(wxSize(560, -1));

        m_type->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { UpdateEnabled(); });
        m_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Browse(); });
        change->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::string p = m_providerId, m = m_model;
            if (PickModel(this, p, m, true)) {
                m_providerId = p;
                m_model = m;
                UpdateModelLabel();
            }
        });
        Bind(wxEVT_BUTTON, &NewConversationDialog::OnOk, this, wxID_OK);
        UpdateEnabled();
        UpdateModelLabel();
        m_name->SetFocus();
    }

    NewConversationChoice Choice() const {
        NewConversationChoice c;
        c.title = S(m_name->GetValue());
        int sel = m_type->GetSelection();
        c.type = sel == 1 ? ConversationType::Code : sel == 2 ? ConversationType::Computer : ConversationType::Chat;
        c.folder = c.type == ConversationType::Code ? Folder() : std::string();
        c.providerId = m_providerId;
        c.model = m_model;
        return c;
    }

private:
    void UpdateEnabled() {
        bool code = m_type->GetSelection() == 1;
        m_folderLabel->Enable(code);
        m_folder->Enable(code);
        m_browse->Enable(code);
    }

    void UpdateModelLabel() {
        if (m_providerId.empty()) {
            const ProviderConfig* p = AppSettings().DefaultProvider();
            std::string model = AppSettings().general.defaultModel;
            if (p && model.empty()) model = CreateDefaultModelName(*p);
            m_modelLabel->SetLabel(p ? "Model: " + U(ModelDisplayName(p->id, model)) + " on " + U(p->name) + " (default)"
                                     : wxString("Model: none yet - add a provider in Settings"));
        } else {
            const ProviderConfig* p = AppSettings().FindProvider(m_providerId);
            m_modelLabel->SetLabel("Model: " + U(ModelDisplayName(m_providerId, m_model)) + (p ? " on " + U(p->name) : wxString()));
        }
        Layout();
    }

    static std::string CreateDefaultModelName(const ProviderConfig& p) {
        std::string model = CreateProvider(p)->DefaultModel();
        return model.empty() ? std::string("the provider's default") : model;
    }

    // The folder typed, with a name or relative path inside the base folder.
    std::string Folder() const {
        return ResolveFolder(S(m_folder->GetValue()), AppSettings().general.codeFolder, GetHomeDir());
    }

    void Browse() {
        // From the folder typed if it is there, else the base folder, else home.
        std::string start = Folder();
        if (start.empty() || !wxFileName::DirExists(U(start))) start = AppSettings().general.codeFolder;
        if (start.empty() || !wxFileName::DirExists(U(start))) start = GetHomeDir();
        wxDirDialog dlg(this, "Choose the project folder", U(start), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) m_folder->SetValue(dlg.GetPath());
    }

    void OnOk(wxCommandEvent& e) {
        if (m_type->GetSelection() == 1) {
            std::string folder = Folder();
            if (folder.empty()) {
                ShowError(this, "Choose a folder for a code conversation.");
                m_folder->SetFocus();
                return;
            }
            wxFileName name = wxFileName::DirName(U(folder));
            if (!name.IsAbsolute()) {
                ShowError(this, "Type the folder's full path, or set a base folder for code conversations in Settings.");
                m_folder->SetFocus();
                return;
            }
            if (!wxFileName::DirExists(U(folder))) {
                // A new project: make its folder.
                if (!AskYesNo(this, U(folder) + " does not exist. Create it?", "New folder")) {
                    m_folder->SetFocus();
                    return;
                }
                if (!wxFileName::Mkdir(U(folder), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
                    ShowError(this, "Could not create " + U(folder) + ".");
                    m_folder->SetFocus();
                    return;
                }
            }
        }
        e.Skip();
    }

    wxTextCtrl* m_name;
    wxRadioBox* m_type;
    wxStaticText* m_folderLabel;
    wxTextCtrl* m_folder;
    wxButton* m_browse;
    wxStaticText* m_modelLabel;
    std::string m_providerId;
    std::string m_model;
};

}  // namespace

bool AskNewConversation(wxWindow* parent, NewConversationChoice& out) {
    NewConversationDialog dlg(parent);
    if (dlg.ShowModal() != wxID_OK) return false;
    out = dlg.Choice();
    return true;
}
