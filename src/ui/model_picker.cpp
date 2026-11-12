// Choosing a provider and one of its models. The list comes from the provider
// (cached; Refresh fetches it again) and can be filtered by typing.

#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/providers/provider.h"
#include "core/util.h"

#include <atomic>

namespace {

class ModelPickerDialog : public wxDialog {
public:
    ModelPickerDialog(wxWindow* parent, const wxString& title, bool allowDefault, const std::string& providerId,
                      const std::string& modelId)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxSize(760, 560), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_allowDefault(allowDefault) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        if (allowDefault) {
            m_useDefault = new wxCheckBox(this, wxID_ANY, "&Use the default model from Settings");
            m_useDefault->SetValue(providerId.empty() && modelId.empty());
            sizer->Add(m_useDefault, 0, wxALL, 8);
            m_useDefault->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateEnabled(); });
        }

        m_provider = new wxChoice(this, wxID_ANY);
        for (const auto& p : AppSettings().providers) {
            if (!p.enabled) continue;
            m_providerIds.push_back(p.id);
            m_provider->Append(U(p.name));
        }
        sizer->Add(LabeledRow(this, "&Provider:", m_provider), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        m_filter = new wxTextCtrl(this, wxID_ANY);
        sizer->Add(LabeledRow(this, "&Filter:", m_filter), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        sizer->Add(new wxStaticText(this, wxID_ANY, "&Models"), 0, wxLEFT | wxRIGHT | wxTOP, 8);
        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

        m_status = new wxStaticText(this, wxID_ANY, "");
        sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        m_modelId = new wxTextCtrl(this, wxID_ANY, U(modelId));
        sizer->Add(LabeledRow(this, "Model &ID:", m_modelId), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_refresh = new wxButton(this, wxID_ANY, "&Refresh list");
        buttons->Add(m_refresh, 0, wxRIGHT, 8);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_OK, "OK"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 8);
        SetSizer(sizer);

        int sel = 0;
        std::string want = providerId.empty() && AppSettings().DefaultProvider() ? AppSettings().DefaultProvider()->id
                                                                                 : providerId;
        for (size_t i = 0; i < m_providerIds.size(); i++) {
            if (m_providerIds[i] == want) sel = static_cast<int>(i);
        }
        if (!m_providerIds.empty()) m_provider->SetSelection(sel);

        m_provider->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            m_modelId->Clear();
            LoadModels(false);
        });
        m_filter->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Fill(); });
        m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
            int i = m_list->GetSelection();
            if (i >= 0 && i < static_cast<int>(m_shown.size())) m_modelId->ChangeValue(U(m_shown[i]));
        });
        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadModels(true); });
        Bind(wxEVT_BUTTON, &ModelPickerDialog::OnOk, this, wxID_OK);

        LoadModels(false);
        UpdateEnabled();
        if (m_useDefault && m_useDefault->GetValue()) m_useDefault->SetFocus();
        else m_list->SetFocus();
    }

    ~ModelPickerDialog() override { *m_cancel = true; }

    std::string ProviderId() const {
        if (m_useDefault && m_useDefault->GetValue()) return std::string();
        int i = m_provider->GetSelection();
        return i >= 0 ? m_providerIds[i] : std::string();
    }
    std::string ModelId() const {
        if (m_useDefault && m_useDefault->GetValue()) return std::string();
        return Trim(S(m_modelId->GetValue()));
    }

private:
    void UpdateEnabled() {
        bool on = !(m_useDefault && m_useDefault->GetValue());
        for (wxWindow* w : {static_cast<wxWindow*>(m_provider), static_cast<wxWindow*>(m_filter),
                            static_cast<wxWindow*>(m_list), static_cast<wxWindow*>(m_modelId),
                            static_cast<wxWindow*>(m_refresh)}) {
            w->Enable(on);
        }
    }

    void LoadModels(bool refresh) {
        int i = m_provider->GetSelection();
        if (i < 0) {
            m_status->SetLabel("Add a provider in Settings first.");
            return;
        }
        std::string id = m_providerIds[i];
        int64_t when = 0;
        if (!refresh && AppProviderCache().GetModels(id, m_models, &when)) {
            m_status->SetLabel(wxString::Format("%zu models, listed %s.", m_models.size(), U(FormatRelativeTime(when))));
            Fill();
            return;
        }
        const ProviderConfig* config = AppSettings().FindProvider(id);
        if (!config) return;
        m_models.clear();
        Fill();
        m_status->SetLabel("Getting the model list...");
        m_refresh->Disable();
        ProviderConfig copy = *config;
        auto cancel = m_cancel;
        struct Result {
            bool ok;
            std::vector<ModelInfo> models;
            std::string error;
        };
        RunInBackground<Result>(
            this,
            [copy, cancel]() {
                Result r;
                auto p = CreateProvider(copy);
                r.ok = p->ListModels(r.models, r.error, cancel.get());
                if (r.ok) AppProviderCache().SetModels(copy.id, r.models);
                return r;
            },
            [this, id](Result r) {
                m_refresh->Enable();
                if (m_provider->GetSelection() < 0 || m_providerIds[m_provider->GetSelection()] != id) return;
                if (!r.ok) {
                    m_status->SetLabel("Could not get the models: " + U(r.error));
                    return;
                }
                m_models = std::move(r.models);
                m_status->SetLabel(wxString::Format("%zu models.", m_models.size()));
                Fill();
            });
    }

    void Fill() {
        std::string filter = ToLower(Trim(S(m_filter->GetValue())));
        std::string current = S(m_modelId->GetValue());
        m_list->Freeze();
        m_list->Clear();
        m_shown.clear();
        wxArrayString items;
        int select = -1;
        for (const auto& m : m_models) {
            std::string text = DescribeModel(m);
            if (m.name != m.id) text += " (" + m.id + ")";
            if (!filter.empty() && ToLower(text).find(filter) == std::string::npos) continue;
            if (m.id == current) select = static_cast<int>(m_shown.size());
            m_shown.push_back(m.id);
            items.Add(U(text));
        }
        if (!items.empty()) m_list->Append(items);
        if (select >= 0) m_list->SetSelection(select);
        m_list->Thaw();
    }

    void OnOk(wxCommandEvent& e) {
        if (!(m_useDefault && m_useDefault->GetValue())) {
            if (m_provider->GetSelection() < 0) {
                ShowError(this, "There is no provider to choose. Add one in Settings.");
                return;
            }
            if (ModelId().empty()) {
                ShowError(this, "Choose a model from the list or type its ID.");
                m_list->SetFocus();
                return;
            }
        }
        e.Skip();
    }

    bool m_allowDefault;
    wxCheckBox* m_useDefault = nullptr;
    wxChoice* m_provider;
    wxTextCtrl* m_filter;
    wxListBox* m_list;
    wxStaticText* m_status;
    wxTextCtrl* m_modelId;
    wxButton* m_refresh;
    std::vector<std::string> m_providerIds;
    std::vector<ModelInfo> m_models;
    std::vector<std::string> m_shown;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace

bool PickModel(wxWindow* parent, std::string& providerId, std::string& modelId, bool allowDefault, const wxString& title) {
    if (AppSettings().providers.empty()) {
        ShowError(parent, "Add a provider first: Settings (Ctrl+Comma), Providers.");
        return false;
    }
    ModelPickerDialog dlg(parent, title, allowDefault, providerId, modelId);
    if (dlg.ShowModal() != wxID_OK) return false;
    providerId = dlg.ProviderId();
    modelId = dlg.ModelId();
    return true;
}
