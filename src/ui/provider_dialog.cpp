// Adding and editing a provider, and showing what it reports about the account.

#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/chat_engine.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/util.h"

#include <wx/filedlg.h>
#include <wx/hyperlink.h>

#include <thread>

namespace {

class ProviderDialog : public wxDialog {
public:
    ProviderDialog(wxWindow* parent, ProviderConfig& config, bool adding)
        : wxDialog(parent, wxID_ANY, adding ? "Add provider" : "Edit provider", wxDefaultPosition, wxSize(640, -1),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_config(config) {
        auto* s = new wxBoxSizer(wxVERTICAL);

        m_kind = new wxChoice(this, wxID_ANY);
        for (const auto& k : ProviderKinds()) {
            m_kind->Append(k.displayName);
            m_kindKeys.push_back(k.key);
            if (config.kind == k.key) m_kind->SetSelection(m_kind->GetCount() - 1);
        }
        if (m_kind->GetSelection() < 0) m_kind->SetSelection(0);
        m_kind->Enable(adding);
        s->Add(LabeledRow(this, "&Service:", m_kind), 0, wxEXPAND | wxALL, 6);

        m_description = new wxStaticText(this, wxID_ANY, "");
        s->Add(m_description, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

        m_name = new wxTextCtrl(this, wxID_ANY, U(config.name));
        s->Add(LabeledRow(this, "&Name:", m_name), 0, wxEXPAND | wxALL, 6);

        m_keyLabel = new wxStaticText(this, wxID_ANY, "API &key:");
        m_key = new wxTextCtrl(this, wxID_ANY, U(config.apiKey), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_keyPlain = new wxTextCtrl(this, wxID_ANY, U(config.apiKey));
        m_keyPlain->Hide();
        auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
        keyRow->Add(m_keyLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        keyRow->Add(m_key, 1, wxALIGN_CENTER_VERTICAL);
        keyRow->Add(m_keyPlain, 1, wxALIGN_CENTER_VERTICAL);
        s->Add(keyRow, 0, wxEXPAND | wxALL, 6);
        m_showKey = new wxCheckBox(this, wxID_ANY, "Sho&w the key");
        s->Add(m_showKey, 0, wxLEFT | wxRIGHT, 6);
        m_link = new wxHyperlinkCtrl(this, wxID_ANY, "Get a key", "https://example.com");
        s->Add(m_link, 0, wxALL, 6);

        m_adminLabel = new wxStaticText(this, wxID_ANY, "A&dmin key:");
        m_admin = new wxTextCtrl(this, wxID_ANY, U(config.adminKey), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        auto* adminRow = new wxBoxSizer(wxHORIZONTAL);
        adminRow->Add(m_adminLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        adminRow->Add(m_admin, 1, wxALIGN_CENTER_VERTICAL);
        s->Add(adminRow, 0, wxEXPAND | wxALL, 6);

        auto* urlRow = new wxBoxSizer(wxHORIZONTAL);
        m_urlLabel = new wxStaticText(this, wxID_ANY, "Base &URL:");
        m_url = new wxTextCtrl(this, wxID_ANY, U(config.baseUrl));
        m_browseProgram = new wxButton(this, wxID_ANY, "B&rowse...");
        urlRow->Add(m_urlLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        urlRow->Add(m_url, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        urlRow->Add(m_browseProgram, 0, wxALIGN_CENTER_VERTICAL);
        s->Add(urlRow, 0, wxEXPAND | wxALL, 6);
        m_browseProgram->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
#ifdef __WXMSW__
            const char* filter = "Programs (*.exe;*.cmd;*.bat)|*.exe;*.cmd;*.bat|All files|*.*";
#else
            const char* filter = "All files|*";
#endif
            wxFileDialog dlg(this, "Choose the program", "", "", filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK) m_url->SetValue(dlg.GetPath());
        });

        m_model = new wxTextCtrl(this, wxID_ANY, U(config.defaultModel));
        auto* modelRow = new wxBoxSizer(wxHORIZONTAL);
        modelRow->Add(new wxStaticText(this, wxID_ANY, "Default &model:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        modelRow->Add(m_model, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        auto* choose = new wxButton(this, wxID_ANY, "C&hoose...");
        modelRow->Add(choose, 0);
        s->Add(modelRow, 0, wxEXPAND | wxALL, 6);

        m_enabled = new wxCheckBox(this, wxID_ANY, "&Enabled");
        m_enabled->SetValue(config.enabled);
        s->Add(m_enabled, 0, wxALL, 6);

        auto* testRow = new wxBoxSizer(wxHORIZONTAL);
        m_test = new wxButton(this, wxID_ANY, "&Test connection");
        m_testResult = new wxStaticText(this, wxID_ANY, "");
        testRow->Add(m_test, 0, wxRIGHT, 8);
        testRow->Add(m_testResult, 1, wxALIGN_CENTER_VERTICAL);
        s->Add(testRow, 0, wxEXPAND | wxALL, 6);

        s->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 6);
        SetSizerAndFit(s);
        SetSize(wxSize(640, -1));

        m_kind->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { KindChanged(true); });
        m_showKey->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            bool show = m_showKey->GetValue();
            if (show) m_keyPlain->ChangeValue(m_key->GetValue());
            else m_key->ChangeValue(m_keyPlain->GetValue());
            m_key->Show(!show);
            m_keyPlain->Show(show);
            Layout();
            (show ? m_keyPlain : m_key)->SetFocus();
        });
        choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ChooseModel(); });
        m_test->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Test(); });
        Bind(wxEVT_BUTTON, &ProviderDialog::OnOk, this, wxID_OK);
        KindChanged(adding && config.name.empty());
        (adding ? static_cast<wxWindow*>(m_kind) : static_cast<wxWindow*>(m_name))->SetFocus();
    }

    ~ProviderDialog() override { *m_cancel = true; }

private:
    const ProviderKindInfo& Kind() const { return *FindProviderKind(m_kindKeys[std::max(0, m_kind->GetSelection())]); }

    void KindChanged(bool setName) {
        const ProviderKindInfo& k = Kind();
        m_description->SetLabel(k.description);
        m_description->Wrap(600);
        if (setName) m_name->ChangeValue(k.displayName);
        bool agent = k.style == ApiStyle::ClaudeCode || k.style == ApiStyle::Codex;
        m_browseProgram->Show(agent);
        if (agent) {
            const char* program = k.style == ApiStyle::ClaudeCode ? "claude" : "codex";
            std::string found = FindProgram(program);
            m_urlLabel->SetLabel("&Program:");
            m_url->SetHint(found.empty() ? wxString::Format("Not found: install %s, or choose the program", program)
                                         : "Empty: " + U(found));
        } else {
            m_urlLabel->SetLabel("Base &URL:");
            m_url->SetHint(wxString("Default: ") + k.defaultBaseUrl);
        }
        bool admin = k.hasAdminKey;
        m_adminLabel->Show(admin);
        m_admin->Show(admin);
        if (admin) m_adminLabel->SetLabel(wxString(k.adminKeyLabel) + ":");
        if (agent) m_keyLabel->SetLabel("API &key (optional; empty uses your login):");
        else m_keyLabel->SetLabel(k.needsApiKey ? "API &key:" : "API &key (if the server needs one):");
        bool hasLink = k.keyPageUrl && *k.keyPageUrl;
        m_link->Show(hasLink);
        if (hasLink) {
            m_link->SetURL(k.keyPageUrl);
            m_link->SetLabel(wxString("Get a key: ") + k.keyPageUrl);
        }
        m_model->SetHint(k.defaultModel && *k.defaultModel ? wxString("Default: ") + k.defaultModel
                                                           : wxString("Choose after testing the connection"));
        Layout();
        Fit();
        SetSize(wxSize(640, -1));
    }

    ProviderConfig Current() const {
        ProviderConfig c = m_config;
        c.kind = Kind().key;
        c.name = Trim(S(m_name->GetValue()));
        c.apiKey = Trim(S(m_showKey->GetValue() ? m_keyPlain->GetValue() : m_key->GetValue()));
        c.adminKey = Kind().hasAdminKey ? Trim(S(m_admin->GetValue())) : std::string();
        c.baseUrl = Trim(S(m_url->GetValue()));
        c.defaultModel = Trim(S(m_model->GetValue()));
        c.enabled = m_enabled->GetValue();
        return c;
    }

    void ChooseModel() {
        ProviderConfig c = Current();
        // The picker works from the saved settings; show it this provider as edited.
        Settings saved = AppSettings();
        bool found = false;
        for (auto& p : AppSettings().providers) {
            if (p.id == c.id) {
                p = c;
                found = true;
            }
        }
        if (!found) AppSettings().providers.push_back(c);
        std::string providerId = c.id, model = c.defaultModel;
        c.enabled = true;
        bool ok = PickModel(this, providerId, model, false, "Default model of " + U(c.name));
        AppSettings().providers = saved.providers;
        if (ok && providerId == c.id) m_model->SetValue(U(model));
    }

    void Test() {
        ProviderConfig c = Current();
        m_test->Disable();
        m_testResult->SetLabel("Connecting...");
        auto cancel = m_cancel;
        struct Result {
            bool ok;
            size_t count;
            std::string error;
            std::vector<ModelInfo> models;
        };
        RunInBackground<Result>(
            this,
            [c, cancel]() {
                Result r{};
                auto p = CreateProvider(c);
                r.ok = p->ListModels(r.models, r.error, cancel.get());
                r.count = r.models.size();
                if (r.ok) AppProviderCache().SetModels(c.id, r.models);
                return r;
            },
            [this](Result r) {
                m_test->Enable();
                if (r.ok) {
                    m_testResult->SetLabel(wxString::Format("Connected: %zu models available.", r.count));
                } else {
                    m_testResult->SetLabel("Failed: " + U(r.error));
                }
                m_testResult->Wrap(450);
                Layout();
                m_test->SetFocus();
            });
    }

    void OnOk(wxCommandEvent& e) {
        ProviderConfig c = Current();
        if (c.name.empty()) {
            ShowError(this, "Give the provider a name.");
            m_name->SetFocus();
            return;
        }
        if (Kind().needsApiKey && c.apiKey.empty()) {
            if (!AskYesNo(this, "There is no API key. Save anyway?", "Add provider")) return;
        }
        m_config = c;
        e.Skip();
    }

    ProviderConfig& m_config;
    std::vector<std::string> m_kindKeys;
    wxChoice* m_kind;
    wxStaticText* m_description;
    wxTextCtrl* m_name;
    wxStaticText* m_keyLabel;
    wxTextCtrl* m_key;
    wxTextCtrl* m_keyPlain;
    wxCheckBox* m_showKey;
    wxHyperlinkCtrl* m_link;
    wxStaticText* m_adminLabel;
    wxTextCtrl* m_admin;
    wxStaticText* m_urlLabel;
    wxTextCtrl* m_url;
    wxButton* m_browseProgram;
    wxTextCtrl* m_model;
    wxCheckBox* m_enabled;
    wxButton* m_test;
    wxStaticText* m_testResult;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

std::string FormatAccount(const ProviderConfig& config, const AccountInfo& info) {
    std::string text = config.name + "\n\n";
    if (!info.error.empty()) text += "Problem: " + info.error + "\n\n";
    for (const auto& [label, value] : info.lines) text += label + ": " + value + "\n";

    int64_t when = 0;
    auto limits = AppProviderCache().GetRateLimits(config.id, &when);
    if (!limits.empty()) {
        text += "\nRate limits, as of the last response (" + FormatRelativeTime(when) + "):\n";
        for (const auto& [name, value] : limits) text += name + ": " + value + "\n";
    } else {
        text += "\nRate limits appear here after a response from this provider.\n";
    }

    std::vector<ModelInfo> models;
    int64_t fetched = 0;
    if (AppProviderCache().GetModels(config.id, models, &fetched)) {
        text += "\n" + std::to_string(models.size()) + " models, listed " + FormatRelativeTime(fetched) + ".\n";
    }
    return text;
}

}  // namespace

bool EditProvider(wxWindow* parent, ProviderConfig& config, bool adding) {
    ProviderDialog dlg(parent, config, adding);
    return dlg.ShowModal() == wxID_OK;
}

void ShowAccountInfo(wxWindow* parent, const ProviderConfig& config) {
    wxBusyCursor busy;
    wxWindowDisabler disabler;
    // Fetch on a worker thread while the UI stays responsive enough to repaint.
    std::atomic<bool> done{false};
    AccountInfo result;
    std::thread worker([&]() {
        auto p = CreateProvider(config);
        result = p->GetAccountInfo(nullptr);
        done = true;
    });
    while (!done) {
        wxYield();
        wxMilliSleep(30);
    }
    worker.join();
    AppProviderCache().SetAccount(config.id, result);
    ShowTextDialog(parent, "Account: " + U(config.name), U(FormatAccount(config, result)));
}
