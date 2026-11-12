// A conversation's own settings: model and prompt, speech, and sounds. Anything
// left at "Default" follows the global settings.

#include "ui/dialogs.h"
#include "ui/event_settings_panel.h"
#include "ui/ui_util.h"
#include "core/speech.h"
#include "core/util.h"

#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>

namespace {

class ChatSettingsDialog : public wxDialog {
public:
    ChatSettingsDialog(wxWindow* parent, Conversation& conv)
        : wxDialog(parent, wxID_ANY, "Conversation settings: " + U(conv.title.empty() ? std::string("untitled") : conv.title),
                   wxDefaultPosition, wxSize(760, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_conv(conv), m_work(conv.settings) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_book = new wxNotebook(this, wxID_ANY);
        m_book->AddPage(BuildModel(), "Model");
        m_book->AddPage(BuildSpeech(), "Speech");
        m_book->AddPage(BuildSounds(), "Sounds and events");
        sizer->Add(m_book, 1, wxEXPAND | wxALL, 8);
        sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
        SetSizer(sizer);
        Bind(wxEVT_BUTTON, &ChatSettingsDialog::OnOk, this, wxID_OK);
    }

private:
    wxWindow* BuildModel() {
        auto* panel = new wxScrolledWindow(m_book);
        panel->SetScrollRate(0, 10);
        auto* s = new wxBoxSizer(wxVERTICAL);

        m_title = new wxTextCtrl(panel, wxID_ANY, U(m_conv.title));
        s->Add(LabeledRow(panel, "&Name:", m_title), 0, wxEXPAND | wxALL, 6);

        auto* modelRow = new wxBoxSizer(wxHORIZONTAL);
        m_modelLabel = new wxStaticText(panel, wxID_ANY, "");
        auto* change = new wxButton(panel, wxID_ANY, "Change &model...");
        modelRow->Add(m_modelLabel, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        modelRow->Add(change, 0);
        s->Add(modelRow, 0, wxEXPAND | wxALL, 6);
        change->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::string p = m_work.providerId, m = m_work.model;
            if (PickModel(this, p, m, true)) {
                m_work.providerId = p;
                m_work.model = m;
                UpdateModelLabel();
            }
        });
        UpdateModelLabel();

        m_ownPrompt = new wxCheckBox(panel, wxID_ANY, "Use its own &system prompt");
        m_ownPrompt->SetValue(m_work.systemPrompt.has_value());
        s->Add(m_ownPrompt, 0, wxALL, 6);
        const std::string& globalPrompt = m_conv.type == ConversationType::Code ? AppSettings().general.codeSystemPrompt
                                                                                 : AppSettings().general.chatSystemPrompt;
        s->Add(new wxStaticText(panel, wxID_ANY, "System &prompt:"), 0, wxLEFT | wxRIGHT, 6);
        m_prompt = new wxTextCtrl(panel, wxID_ANY, U(m_work.systemPrompt.value_or(globalPrompt)), wxDefaultPosition,
                                  wxSize(-1, 110), wxTE_MULTILINE);
        s->Add(m_prompt, 0, wxEXPAND | wxALL, 6);
        m_ownPrompt->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateEnabled(); });

        m_effort = new wxChoice(panel, wxID_ANY);
        for (const auto& [key, label] : EffortChoices()) {
            m_effort->Append(key.empty() ? wxString("Default") : label);
            if (key == m_work.reasoningEffort) m_effort->SetSelection(m_effort->GetCount() - 1);
        }
        if (m_effort->GetSelection() < 0) m_effort->SetSelection(0);
        s->Add(LabeledRow(panel, "Reasoning e&ffort:", m_effort, 0), 0, wxALL, 6);

        m_ownTemp = new wxCheckBox(panel, wxID_ANY, "Set the &temperature (not every model accepts one)");
        m_ownTemp->SetValue(m_work.temperature.has_value());
        s->Add(m_ownTemp, 0, wxALL, 6);
        m_temp = new wxSpinCtrlDouble(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.0, 2.0,
                                      m_work.temperature.value_or(1.0), 0.05);
        s->Add(LabeledRow(panel, "T&emperature:", m_temp, 0), 0, wxALL, 6);
        m_ownTemp->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateEnabled(); });

        m_maxTokens = new wxSpinCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 1000000,
                                     m_work.maxTokens.value_or(0));
        s->Add(LabeledRow(panel, "Maximum response to&kens (0 for the default):", m_maxTokens, 0), 0, wxALL, 6);

        if (m_conv.type == ConversationType::Code) {
            auto* folderRow = new wxBoxSizer(wxHORIZONTAL);
            folderRow->Add(new wxStaticText(panel, wxID_ANY, "F&older:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_folder = new wxTextCtrl(panel, wxID_ANY, U(m_conv.folder));
            folderRow->Add(m_folder, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* browse = new wxButton(panel, wxID_ANY, "&Browse...");
            folderRow->Add(browse, 0);
            s->Add(folderRow, 0, wxEXPAND | wxALL, 6);
            browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxDirDialog dlg(this, "Choose the project folder", m_folder->GetValue());
                if (dlg.ShowModal() == wxID_OK) m_folder->SetValue(dlg.GetPath());
            });

        }
        if (m_conv.type == ConversationType::Code || m_conv.type == ConversationType::Computer) {
            bool computer = m_conv.type == ConversationType::Computer;
            m_approval = new wxChoice(panel, wxID_ANY);
            m_approval->Append("Default");
            m_approval->Append(computer ? "Ask before each action" : "Ask each time");
            m_approval->Append(computer ? "Act without asking" : "Run without asking");
            m_approval->Append(computer ? "Only look, never act" : "Never run commands");
            const std::string& a = m_work.commandApproval;
            m_approval->SetSelection(a == "ask" ? 1 : a == "auto" ? 2 : a == "deny" ? 3 : 0);
            s->Add(LabeledRow(panel, computer ? "&Actions:" : "&Commands:", m_approval, 0), 0, wxALL, 6);
        }
        panel->SetSizer(s);
        UpdateEnabled();
        return panel;
    }

    void UpdateModelLabel() {
        if (m_work.providerId.empty()) {
            m_modelLabel->SetLabel("Model: the default from Settings");
        } else {
            const ProviderConfig* p = AppSettings().FindProvider(m_work.providerId);
            m_modelLabel->SetLabel("Model: " + U(ModelDisplayName(m_work.providerId, m_work.model)) +
                                   (p ? " on " + U(p->name) : wxString(" (provider removed)")));
        }
    }

    void UpdateEnabled() {
        m_prompt->Enable(m_ownPrompt->GetValue());
        m_temp->Enable(m_ownTemp->GetValue());
        if (m_rateOwn) m_rate->Enable(m_rateOwn->GetValue());
    }

    wxWindow* BuildSpeech() {
        auto* panel = new wxScrolledWindow(m_book);
        panel->SetScrollRate(0, 10);
        auto* s = new wxBoxSizer(wxVERTICAL);
        const SpeechSettings& g = AppSettings().speech;
        const SpeechOverrides& o = m_work.speech;

        m_speechOn = MakeTriChoice(panel, g.enabled);
        SetTriChoice(m_speechOn, o.enabled);
        s->Add(LabeledRow(panel, "&Speech:", m_speechOn, 0), 0, wxALL, 6);

        m_speakResponses = new wxChoice(panel, wxID_ANY);
        static const char* kModes[] = {"Off", "When complete", "While they arrive"};
        m_speakResponses->Append(wxString("Default (") + kModes[std::clamp(g.speakResponses, 0, 2)] + ")");
        for (const char* m : kModes) m_speakResponses->Append(m);
        m_speakResponses->SetSelection(o.speakResponses ? *o.speakResponses + 1 : 0);
        s->Add(LabeledRow(panel, "Speak &responses:", m_speakResponses, 0), 0, wxALL, 6);

        m_speakReasoning = MakeTriChoice(panel, g.speakReasoning);
        SetTriChoice(m_speakReasoning, o.speakReasoning);
        s->Add(LabeledRow(panel, "Speak &thinking:", m_speakReasoning, 0), 0, wxALL, 6);

        m_speakToolCalls = MakeTriChoice(panel, g.speakToolCalls);
        SetTriChoice(m_speakToolCalls, o.speakToolCalls);
        s->Add(LabeledRow(panel, "Speak tool calls and c&ommands:", m_speakToolCalls, 0), 0, wxALL, 6);

        m_speakBackground = MakeTriChoice(panel, g.speakInBackground);
        SetTriChoice(m_speakBackground, o.speakInBackground);
        s->Add(LabeledRow(panel, "Speak in the &background:", m_speakBackground, 0), 0, wxALL, 6);

        m_interrupt = MakeTriChoice(panel, g.interrupt);
        SetTriChoice(m_interrupt, o.interrupt);
        s->Add(LabeledRow(panel, "&Interrupt speech:", m_interrupt, 0), 0, wxALL, 6);

        m_voice = new wxChoice(panel, wxID_ANY);
        m_voice->Append("Default");
        m_voice->SetSelection(0);
        for (const auto& v : Speech::ListVoices(g.engine, g.prismBackend)) {
            m_voice->Append(U(v));
            if (o.voice && *o.voice == v) m_voice->SetSelection(m_voice->GetCount() - 1);
        }
        s->Add(LabeledRow(panel, "&Voice:", m_voice, 0), 0, wxALL, 6);

        m_rateOwn = new wxCheckBox(panel, wxID_ANY, "Its own speech r&ate");
        m_rateOwn->SetValue(o.rate.has_value());
        s->Add(m_rateOwn, 0, wxALL, 6);
        m_rate = new wxSlider(panel, wxID_ANY, o.rate.value_or(g.rate), 0, 100);
        s->Add(LabeledRow(panel, "R&ate:", m_rate), 0, wxEXPAND | wxALL, 6);
        m_rateOwn->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateEnabled(); });
        panel->SetSizer(s);
        UpdateEnabled();
        return panel;
    }

    wxWindow* BuildSounds() {
        auto* panel = new wxPanel(m_book);
        auto* s = new wxBoxSizer(wxVERTICAL);
        m_soundsOn = MakeTriChoice(panel, AppSettings().sounds.enabled);
        SetTriChoice(m_soundsOn, m_work.soundsEnabled);
        s->Add(LabeledRow(panel, "&Sounds:", m_soundsOn, 0), 0, wxALL, 6);
        m_events = new EventSettingsPanel(panel, m_work.events);
        s->Add(m_events, 1, wxEXPAND | wxALL, 6);
        panel->SetSizer(s);
        return panel;
    }

    void OnOk(wxCommandEvent& e) {
        if (m_folder && !wxFileName::DirExists(m_folder->GetValue())) {
            ShowError(this, "The folder does not exist.");
            m_book->SetSelection(0);
            m_folder->SetFocus();
            return;
        }
        m_conv.title = Trim(S(m_title->GetValue()));
        if (m_ownPrompt->GetValue()) m_work.systemPrompt = S(m_prompt->GetValue());
        else m_work.systemPrompt.reset();
        m_work.reasoningEffort = EffortChoices()[std::max(0, m_effort->GetSelection())].first;
        if (m_ownTemp->GetValue()) m_work.temperature = m_temp->GetValue();
        else m_work.temperature.reset();
        if (m_maxTokens->GetValue() > 0) m_work.maxTokens = m_maxTokens->GetValue();
        else m_work.maxTokens.reset();
        if (m_folder) m_conv.folder = S(m_folder->GetValue());
        if (m_approval) {
            static const char* kApproval[] = {"", "ask", "auto", "deny"};
            m_work.commandApproval = kApproval[std::max(0, m_approval->GetSelection())];
        }
        SpeechOverrides& o = m_work.speech;
        o.enabled = GetTriChoice(m_speechOn);
        if (m_speakResponses->GetSelection() > 0) o.speakResponses = m_speakResponses->GetSelection() - 1;
        else o.speakResponses.reset();
        o.speakReasoning = GetTriChoice(m_speakReasoning);
        o.speakToolCalls = GetTriChoice(m_speakToolCalls);
        o.speakInBackground = GetTriChoice(m_speakBackground);
        o.interrupt = GetTriChoice(m_interrupt);
        if (m_voice->GetSelection() > 0) o.voice = S(m_voice->GetStringSelection());
        else o.voice.reset();
        if (m_rateOwn->GetValue()) o.rate = m_rate->GetValue();
        else o.rate.reset();
        m_work.soundsEnabled = GetTriChoice(m_soundsOn);
        m_events->Save();
        m_conv.settings = m_work;
        e.Skip();
    }

    Conversation& m_conv;
    ConversationSettings m_work;
    wxNotebook* m_book;
    wxTextCtrl* m_title;
    wxStaticText* m_modelLabel;
    wxCheckBox* m_ownPrompt;
    wxTextCtrl* m_prompt;
    wxChoice* m_effort;
    wxCheckBox* m_ownTemp;
    wxSpinCtrlDouble* m_temp;
    wxSpinCtrl* m_maxTokens;
    wxTextCtrl* m_folder = nullptr;
    wxChoice* m_approval = nullptr;
    wxChoice* m_speechOn;
    wxChoice* m_speakResponses;
    wxChoice* m_speakReasoning;
    wxChoice* m_speakToolCalls;
    wxChoice* m_speakBackground;
    wxChoice* m_interrupt;
    wxChoice* m_voice;
    wxCheckBox* m_rateOwn = nullptr;
    wxSlider* m_rate = nullptr;
    wxChoice* m_soundsOn;
    EventSettingsPanel* m_events;
};

}  // namespace

bool EditConversationSettings(wxWindow* parent, Conversation& conversation) {
    Conversation copy = conversation;
    ChatSettingsDialog dlg(parent, copy);
    if (dlg.ShowModal() != wxID_OK) return false;
    conversation = copy;
    return true;
}
