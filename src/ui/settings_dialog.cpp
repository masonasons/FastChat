// Settings (Ctrl+Comma): General, Providers, Speech, Sounds and events.
// Edits a copy of the settings; OK saves it.

#include "ui/dialogs.h"
#include "ui/event_settings_panel.h"
#include "ui/ui_util.h"
#include "core/notify.h"
#include "core/providers/provider.h"
#include "core/speech.h"
#include "core/util.h"

#include <wx/notebook.h>
#include <wx/spinctrl.h>

namespace {

class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent, int page)
        : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(780, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_work(AppSettings()) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_book = new wxNotebook(this, wxID_ANY);
        m_book->AddPage(BuildGeneral(), "General");
        m_book->AddPage(BuildProviders(), "Providers");
        m_book->AddPage(BuildSpeech(), "Speech");
        m_book->AddPage(BuildSounds(), "Sounds and events");
        sizer->Add(m_book, 1, wxEXPAND | wxALL, 8);
        sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
        SetSizer(sizer);
        if (page >= 0 && page < static_cast<int>(m_book->GetPageCount())) m_book->SetSelection(page);
        Bind(wxEVT_BUTTON, &SettingsDialog::OnOk, this, wxID_OK);
    }

private:
    // ---------------- General ----------------
    wxWindow* BuildGeneral() {
        auto* panel = new wxScrolledWindow(m_book);
        panel->SetScrollRate(0, 10);
        auto* s = new wxBoxSizer(wxVERTICAL);
        const GeneralSettings& g = m_work.general;

        m_enterSends = new wxCheckBox(panel, wxID_ANY, "&Enter sends the message (Shift+Enter for a new line; off: Ctrl+Enter sends)");
        m_enterSends->SetValue(g.enterSends);
        s->Add(m_enterSends, 0, wxALL, 6);
        m_confirmDeletes = new wxCheckBox(panel, wxID_ANY, "&Confirm before deleting messages and conversations");
        m_confirmDeletes->SetValue(g.confirmDeletes);
        s->Add(m_confirmDeletes, 0, wxALL, 6);
        m_autoTitle = new wxCheckBox(panel, wxID_ANY, "&Name untitled conversations automatically after the first response");
        m_autoTitle->SetValue(g.autoTitle);
        s->Add(m_autoTitle, 0, wxALL, 6);
        m_focusResponse = new wxCheckBox(panel, wxID_ANY, "Move focus to a &response when it completes");
        m_focusResponse->SetValue(g.focusResponse);
        s->Add(m_focusResponse, 0, wxALL, 6);

        m_effort = new wxChoice(panel, wxID_ANY);
        for (const auto& [key, label] : EffortChoices()) {
            m_effort->Append(label);
            if (key == g.reasoningEffort) m_effort->SetSelection(m_effort->GetCount() - 1);
        }
        if (m_effort->GetSelection() < 0) m_effort->SetSelection(0);
        s->Add(LabeledRow(panel, "Default reasoning e&ffort:", m_effort, 0), 0, wxALL, 6);

        m_maxTokens = new wxSpinCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 1000000, g.maxTokens);
        s->Add(LabeledRow(panel, "Maximum response &tokens (0 for the model's default):", m_maxTokens, 0), 0, wxALL, 6);

        s->Add(new wxStaticText(panel, wxID_ANY, "Default system prompt for &chat conversations:"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
        m_chatPrompt = new wxTextCtrl(panel, wxID_ANY, U(g.chatSystemPrompt), wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE);
        s->Add(m_chatPrompt, 0, wxEXPAND | wxALL, 6);
        s->Add(new wxStaticText(panel, wxID_ANY, "Extra instructions for c&ode conversations:"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
        m_codePrompt = new wxTextCtrl(panel, wxID_ANY, U(g.codeSystemPrompt), wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE);
        s->Add(m_codePrompt, 0, wxEXPAND | wxALL, 6);

        m_approval = new wxChoice(panel, wxID_ANY);
        m_approval->Append("Ask each time");
        m_approval->Append("Run without asking");
        m_approval->Append("Never run commands");
        m_approval->SetSelection(g.commandApproval == "auto" ? 1 : g.commandApproval == "deny" ? 2 : 0);
        s->Add(LabeledRow(panel, "Commands in code conversations:", m_approval, 0), 0, wxALL, 6);

        m_computerApproval = new wxChoice(panel, wxID_ANY);
        m_computerApproval->Append("Ask before each action");
        m_computerApproval->Append("Act without asking");
        m_computerApproval->Append("Only look, never act");
        m_computerApproval->SetSelection(g.computerApproval == "auto" ? 1 : g.computerApproval == "deny" ? 2 : 0);
        s->Add(LabeledRow(panel, "Actions in computer use conversations:", m_computerApproval, 0), 0, wxALL, 6);

        m_autoCompact = new wxSpinCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 99, g.autoCompactPercent);
        s->Add(LabeledRow(panel, "Compact automatically when the context is this many percent full (0: never):", m_autoCompact, 0), 0, wxALL, 6);

        m_maxSteps = new wxSpinCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 1000, g.maxToolSteps);
        s->Add(LabeledRow(panel, "Most tool &steps per message:", m_maxSteps, 0), 0, wxALL, 6);
        m_timeout = new wxSpinCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 5, 7200, g.commandTimeoutSec);
        s->Add(LabeledRow(panel, "Command time &limit (seconds):", m_timeout, 0), 0, wxALL, 6);

        auto* titleRow = new wxBoxSizer(wxHORIZONTAL);
        m_titleModelLabel = new wxStaticText(panel, wxID_ANY, "");
        auto* titleButton = new wxButton(panel, wxID_ANY, "Model for &naming...");
        titleRow->Add(m_titleModelLabel, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        titleRow->Add(titleButton, 0);
        s->Add(titleRow, 0, wxEXPAND | wxALL, 6);
        titleButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::string p = m_work.general.titleProviderId, m = m_work.general.titleModel;
            if (PickModelFromWork(p, m, true, "Model for naming conversations")) {
                m_work.general.titleProviderId = p;
                m_work.general.titleModel = m;
                UpdateTitleModelLabel();
            }
        });
        UpdateTitleModelLabel();
        panel->SetSizer(s);
        return panel;
    }

    void UpdateTitleModelLabel() {
        const auto& g = m_work.general;
        if (g.titleProviderId.empty()) m_titleModelLabel->SetLabel("Conversations are named by their own model.");
        else m_titleModelLabel->SetLabel("Conversations are named by " + U(ModelDisplayName(g.titleProviderId, g.titleModel)) + ".");
    }

    // The model picker reads the live settings; let it see providers added in this dialog.
    bool PickModelFromWork(std::string& providerId, std::string& modelId, bool allowDefault, const wxString& title) {
        Settings saved = AppSettings();
        AppSettings().providers = m_work.providers;
        bool ok = PickModel(this, providerId, modelId, allowDefault, title);
        AppSettings().providers = saved.providers;
        return ok;
    }

    // ---------------- Providers ----------------
    wxWindow* BuildProviders() {
        auto* panel = new wxPanel(m_book);
        auto* s = new wxBoxSizer(wxVERTICAL);
        s->Add(new wxStaticText(panel, wxID_ANY, "&Providers"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
        m_providerList = new wxListBox(panel, wxID_ANY);
        s->Add(m_providerList, 1, wxEXPAND | wxALL, 6);

        auto* row1 = new wxBoxSizer(wxHORIZONTAL);
        auto* add = new wxButton(panel, wxID_ANY, "&Add...");
        auto* edit = new wxButton(panel, wxID_ANY, "&Edit...");
        auto* remove = new wxButton(panel, wxID_ANY, "&Remove");
        auto* makeDefault = new wxButton(panel, wxID_ANY, "Make &default");
        row1->Add(add, 0, wxRIGHT, 6);
        row1->Add(edit, 0, wxRIGHT, 6);
        row1->Add(remove, 0, wxRIGHT, 6);
        row1->Add(makeDefault, 0);
        s->Add(row1, 0, wxALL, 6);
        auto* row2 = new wxBoxSizer(wxHORIZONTAL);
        auto* account = new wxButton(panel, wxID_ANY, "A&ccount and credits...");
        auto* models = new wxButton(panel, wxID_ANY, "&Models...");
        row2->Add(account, 0, wxRIGHT, 6);
        row2->Add(models, 0);
        s->Add(row2, 0, wxALL, 6);

        auto* defRow = new wxBoxSizer(wxHORIZONTAL);
        m_defaultLabel = new wxStaticText(panel, wxID_ANY, "");
        auto* defButton = new wxButton(panel, wxID_ANY, "Default m&odel...");
        defRow->Add(m_defaultLabel, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        defRow->Add(defButton, 0);
        s->Add(defRow, 0, wxEXPAND | wxALL, 6);
        panel->SetSizer(s);

        add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            ProviderConfig c;
            c.id = NewId();
            c.kind = "openrouter";
            if (EditProvider(this, c, true)) {
                m_work.providers.push_back(c);
                if (m_work.general.defaultProviderId.empty() || !m_work.FindProvider(m_work.general.defaultProviderId)) {
                    m_work.general.defaultProviderId = c.id;
                }
                FillProviders(static_cast<int>(m_work.providers.size()) - 1);
            }
        });
        auto editSelected = [this]() {
            int i = m_providerList->GetSelection();
            if (i < 0) return;
            ProviderConfig c = m_work.providers[i];
            if (EditProvider(this, c, false)) {
                m_work.providers[i] = c;
                FillProviders(i);
            }
        };
        edit->Bind(wxEVT_BUTTON, [editSelected](wxCommandEvent&) { editSelected(); });
        m_providerList->Bind(wxEVT_LISTBOX_DCLICK, [editSelected](wxCommandEvent&) { editSelected(); });
        remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int i = m_providerList->GetSelection();
            if (i < 0) return;
            if (!AskYesNo(this, "Remove " + U(m_work.providers[i].name) + "?", "Remove provider")) return;
            m_work.providers.erase(m_work.providers.begin() + i);
            FillProviders(std::min(i, static_cast<int>(m_work.providers.size()) - 1));
        });
        makeDefault->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int i = m_providerList->GetSelection();
            if (i < 0) return;
            m_work.general.defaultProviderId = m_work.providers[i].id;
            m_work.general.defaultModel.clear();
            FillProviders(i);
        });
        account->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int i = m_providerList->GetSelection();
            if (i >= 0) ShowAccountInfo(this, m_work.providers[i]);
        });
        models->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int i = m_providerList->GetSelection();
            if (i < 0) return;
            std::string p = m_work.providers[i].id, m = m_work.providers[i].defaultModel;
            if (PickModelFromWork(p, m, false, "Models of " + U(m_work.providers[i].name)) && !m.empty()) {
                if (ProviderConfig* c = m_work.FindProvider(p)) {
                    if (AskYesNo(this, "Make " + U(m) + " the default model of " + U(c->name) + "?", "Models")) {
                        c->defaultModel = m;
                        FillProviders(i);
                    }
                }
            }
        });
        defButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::string p = m_work.general.defaultProviderId, m = m_work.general.defaultModel;
            if (PickModelFromWork(p, m, false, "Default model")) {
                m_work.general.defaultProviderId = p;
                m_work.general.defaultModel = m;
                FillProviders(m_providerList->GetSelection());
            }
        });
        FillProviders(0);
        return panel;
    }

    void FillProviders(int select) {
        m_providerList->Clear();
        for (const auto& p : m_work.providers) {
            const ProviderKindInfo* kind = FindProviderKind(p.kind);
            wxString item = U(p.name) + " (" + (kind ? kind->displayName : p.kind.c_str()) + ")";
            if (p.id == m_work.general.defaultProviderId) item += ", default";
            if (!p.enabled) item += ", turned off";
            if (kind && kind->needsApiKey && p.apiKey.empty()) item += ", no API key";
            m_providerList->Append(item);
        }
        if (select >= 0 && select < static_cast<int>(m_providerList->GetCount())) m_providerList->SetSelection(select);
        const ProviderConfig* def = m_work.FindProvider(m_work.general.defaultProviderId);
        if (!def) {
            m_defaultLabel->SetLabel("No default provider yet.");
        } else {
            std::string model = m_work.general.defaultModel.empty() ? def->defaultModel : m_work.general.defaultModel;
            m_defaultLabel->SetLabel("Default: " + U(model.empty() ? std::string("the provider's default model")
                                                                   : ModelDisplayName(def->id, model)) +
                                     " on " + U(def->name));
        }
    }

    // ---------------- Speech ----------------
    wxWindow* BuildSpeech() {
        auto* panel = new wxScrolledWindow(m_book);
        panel->SetScrollRate(0, 10);
        auto* s = new wxBoxSizer(wxVERTICAL);
        const SpeechSettings& sp = m_work.speech;

        m_speechOn = new wxCheckBox(panel, wxID_ANY, "&Speech on");
        m_speechOn->SetValue(sp.enabled);
        s->Add(m_speechOn, 0, wxALL, 6);

        m_engine = new wxChoice(panel, wxID_ANY);
        if (Speech::PrismAvailable()) {
            m_engine->Append("Prism (screen reader, or a speech engine)");
            m_engineKeys.push_back("prism");
        }
        if (Speech::SapiAvailable()) {
            m_engine->Append("SAPI 5 voices");
            m_engineKeys.push_back("sapi");
        }
        for (size_t i = 0; i < m_engineKeys.size(); i++) {
            if (m_engineKeys[i] == sp.engine) m_engine->SetSelection(static_cast<int>(i));
        }
        if (m_engine->GetSelection() < 0 && m_engine->GetCount()) m_engine->SetSelection(0);
        s->Add(LabeledRow(panel, "&Engine:", m_engine, 0), 0, wxALL, 6);

        m_backend = new wxChoice(panel, wxID_ANY);
        m_backend->Append("Automatic (the screen reader that is running, else a speech engine)");
        m_backendKeys.push_back("auto");
        for (const auto& name : Speech::PrismBackendNames()) {
            m_backend->Append(U(name));
            m_backendKeys.push_back(name);
        }
        m_backend->SetSelection(0);
        for (size_t i = 0; i < m_backendKeys.size(); i++) {
            if (m_backendKeys[i] == sp.prismBackend) m_backend->SetSelection(static_cast<int>(i));
        }
        m_backendLabel = new wxStaticText(panel, wxID_ANY, "Prism &output:");
        auto* brow = new wxBoxSizer(wxHORIZONTAL);
        brow->Add(m_backendLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        brow->Add(m_backend, 0);
        s->Add(brow, 0, wxALL, 6);

        m_voice = new wxChoice(panel, wxID_ANY);
        s->Add(LabeledRow(panel, "&Voice:", m_voice, 0), 0, wxALL, 6);

        m_rate = new wxSlider(panel, wxID_ANY, sp.rate, 0, 100);
        s->Add(LabeledRow(panel, "&Rate:", m_rate), 0, wxEXPAND | wxALL, 6);
        m_volume = new wxSlider(panel, wxID_ANY, sp.volume, 0, 100);
        s->Add(LabeledRow(panel, "Vol&ume:", m_volume), 0, wxEXPAND | wxALL, 6);
        auto* test = new wxButton(panel, wxID_ANY, "&Test");
        s->Add(test, 0, wxALL, 6);

        m_speakResponses = new wxChoice(panel, wxID_ANY);
        m_speakResponses->Append("Off");
        m_speakResponses->Append("When complete");
        m_speakResponses->Append("While they arrive (sentence by sentence)");
        m_speakResponses->SetSelection(sp.speakResponses);
        s->Add(LabeledRow(panel, "Speak res&ponses:", m_speakResponses, 0), 0, wxALL, 6);
        m_speakReasoning = new wxCheckBox(panel, wxID_ANY, "Speak the model's t&hinking");
        m_speakReasoning->SetValue(sp.speakReasoning);
        s->Add(m_speakReasoning, 0, wxALL, 6);
        m_speakToolCalls = new wxCheckBox(panel, wxID_ANY, "Speak t&ool calls and commands (code conversations)");
        m_speakToolCalls->SetValue(sp.speakToolCalls);
        s->Add(m_speakToolCalls, 0, wxALL, 6);
        m_speakBackground = new wxCheckBox(panel, wxID_ANY, "Speak responses of conversations in the &background");
        m_speakBackground->SetValue(sp.speakInBackground);
        s->Add(m_speakBackground, 0, wxALL, 6);
        m_interrupt = new wxCheckBox(panel, wxID_ANY, "New speech &interrupts what is being spoken");
        m_interrupt->SetValue(sp.interrupt);
        s->Add(m_interrupt, 0, wxALL, 6);
        m_stripMarkdown = new wxCheckBox(panel, wxID_ANY, "Remove &Markdown formatting before speaking");
        m_stripMarkdown->SetValue(sp.stripMarkdown);
        s->Add(m_stripMarkdown, 0, wxALL, 6);
        m_skipCode = new wxCheckBox(panel, wxID_ANY, "Say \"code block\" instead of reading &code");
        m_skipCode->SetValue(sp.skipCodeBlocks);
        s->Add(m_skipCode, 0, wxALL, 6);
        m_ctrlStops = new wxCheckBox(panel, wxID_ANY, "Pressing Ctrl stops spee&ch");
        m_ctrlStops->SetValue(sp.ctrlStops);
        s->Add(m_ctrlStops, 0, wxALL, 6);
        m_announceWith = new wxChoice(panel, wxID_ANY);
        m_announceWith->Append("The screen reader (Prism)");
        m_announceWith->Append("The speech engine above");
        m_announceWith->SetSelection(sp.announceWith == "speech" ? 1 : 0);
        s->Add(LabeledRow(panel, "Say &buffer names and other announcements with:", m_announceWith, 0), 0, wxALL, 6);
        panel->SetSizer(s);

        m_engine->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateSpeechControls(true); });
        m_backend->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateSpeechControls(true); });
        test->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            SpeechSettings saved = AppSettings().speech;
            StoreSpeech();
            AppSpeech().Configure(m_work.speech);
            AppSpeech().Speak("This is how FastChat sounds with " + AppSpeech().BackendName() + ".", true);
            AppSettings().speech = saved;
        });
        UpdateSpeechControls(true);
        return panel;
    }

    std::string EngineKey() const {
        int i = m_engine->GetSelection();
        return i >= 0 ? m_engineKeys[i] : std::string("prism");
    }

    void UpdateSpeechControls(bool reloadVoices) {
        bool prism = EngineKey() == "prism";
        m_backendLabel->Enable(prism);
        m_backend->Enable(prism);
        if (!reloadVoices) return;
        std::string backend = m_backendKeys[std::max(0, m_backend->GetSelection())];
        std::string want = m_voice->GetSelection() > 0 ? S(m_voice->GetStringSelection()) : m_work.speech.voice;
        m_voice->Clear();
        m_voice->Append("Default");
        m_voice->SetSelection(0);
        for (const auto& v : Speech::ListVoices(EngineKey(), backend)) {
            m_voice->Append(U(v));
            if (v == want) m_voice->SetSelection(m_voice->GetCount() - 1);
        }
        m_voice->Enable(m_voice->GetCount() > 1);
    }

    void StoreSpeech() {
        SpeechSettings& sp = m_work.speech;
        sp.enabled = m_speechOn->GetValue();
        sp.engine = EngineKey();
        sp.prismBackend = m_backendKeys[std::max(0, m_backend->GetSelection())];
        sp.voice = m_voice->GetSelection() > 0 ? S(m_voice->GetStringSelection()) : std::string();
        sp.rate = m_rate->GetValue();
        sp.volume = m_volume->GetValue();
        sp.speakResponses = m_speakResponses->GetSelection();
        sp.speakReasoning = m_speakReasoning->GetValue();
        sp.speakToolCalls = m_speakToolCalls->GetValue();
        sp.speakInBackground = m_speakBackground->GetValue();
        sp.interrupt = m_interrupt->GetValue();
        sp.stripMarkdown = m_stripMarkdown->GetValue();
        sp.skipCodeBlocks = m_skipCode->GetValue();
        sp.ctrlStops = m_ctrlStops->GetValue();
        sp.announceWith = m_announceWith->GetSelection() == 1 ? "speech" : "prism";
    }

    // ---------------- Sounds ----------------
    wxWindow* BuildSounds() {
        auto* panel = new wxPanel(m_book);
        auto* s = new wxBoxSizer(wxVERTICAL);
        m_soundsOn = new wxCheckBox(panel, wxID_ANY, "&Sounds on");
        m_soundsOn->SetValue(m_work.sounds.enabled);
        s->Add(m_soundsOn, 0, wxALL, 6);
        m_soundVolume = new wxSlider(panel, wxID_ANY, m_work.sounds.volume, 0, 100);
        s->Add(LabeledRow(panel, "Sound &volume:", m_soundVolume), 0, wxEXPAND | wxALL, 6);
        m_events = new EventSettingsPanel(panel, m_work.events);
        s->Add(m_events, 1, wxEXPAND | wxALL, 6);
        panel->SetSizer(s);
        return panel;
    }

    void OnOk(wxCommandEvent& e) {
        GeneralSettings& g = m_work.general;
        g.enterSends = m_enterSends->GetValue();
        g.confirmDeletes = m_confirmDeletes->GetValue();
        g.autoTitle = m_autoTitle->GetValue();
        g.focusResponse = m_focusResponse->GetValue();
        g.reasoningEffort = EffortChoices()[std::max(0, m_effort->GetSelection())].first;
        g.maxTokens = m_maxTokens->GetValue();
        g.chatSystemPrompt = S(m_chatPrompt->GetValue());
        g.codeSystemPrompt = S(m_codePrompt->GetValue());
        g.commandApproval = m_approval->GetSelection() == 1 ? "auto" : m_approval->GetSelection() == 2 ? "deny" : "ask";
        g.computerApproval = m_computerApproval->GetSelection() == 1 ? "auto" : m_computerApproval->GetSelection() == 2 ? "deny" : "ask";
        g.maxToolSteps = m_maxSteps->GetValue();
        g.autoCompactPercent = m_autoCompact->GetValue();
        g.commandTimeoutSec = m_timeout->GetValue();
        if (!m_work.FindProvider(g.defaultProviderId)) {
            g.defaultProviderId = m_work.providers.empty() ? std::string() : m_work.providers.front().id;
        }
        StoreSpeech();
        m_work.sounds.enabled = m_soundsOn->GetValue();
        m_work.sounds.volume = m_soundVolume->GetValue();
        m_events->Save();

        json windowState = AppSettings().windowState;
        AppSettings() = m_work;
        AppSettings().windowState = windowState;
        if (!AppSettings().Save()) ShowError(this, "The settings could not be saved to " + U(Settings::FilePath()));
        ApplyOutputSettings();
        e.Skip();
    }

    Settings m_work;
    wxNotebook* m_book;
    // general
    wxCheckBox* m_enterSends;
    wxCheckBox* m_confirmDeletes;
    wxCheckBox* m_autoTitle;
    wxCheckBox* m_focusResponse;
    wxChoice* m_effort;
    wxSpinCtrl* m_maxTokens;
    wxTextCtrl* m_chatPrompt;
    wxTextCtrl* m_codePrompt;
    wxChoice* m_approval;
    wxChoice* m_computerApproval;
    wxSpinCtrl* m_maxSteps;
    wxSpinCtrl* m_autoCompact;
    wxSpinCtrl* m_timeout;
    wxStaticText* m_titleModelLabel;
    // providers
    wxListBox* m_providerList;
    wxStaticText* m_defaultLabel;
    // speech
    wxCheckBox* m_speechOn;
    wxChoice* m_engine;
    std::vector<std::string> m_engineKeys;
    wxStaticText* m_backendLabel;
    wxChoice* m_backend;
    std::vector<std::string> m_backendKeys;
    wxChoice* m_voice;
    wxSlider* m_rate;
    wxSlider* m_volume;
    wxChoice* m_speakResponses;
    wxCheckBox* m_speakReasoning;
    wxCheckBox* m_speakToolCalls;
    wxCheckBox* m_speakBackground;
    wxCheckBox* m_interrupt;
    wxCheckBox* m_stripMarkdown;
    wxCheckBox* m_skipCode;
    wxCheckBox* m_ctrlStops;
    wxChoice* m_announceWith;
    // sounds
    wxCheckBox* m_soundsOn;
    wxSlider* m_soundVolume;
    EventSettingsPanel* m_events;
};

}  // namespace

bool ShowSettingsDialog(wxWindow* parent, int page) {
    SettingsDialog dlg(parent, page);
    bool ok = dlg.ShowModal() == wxID_OK;
    if (!ok) ApplyOutputSettings();  // undo a Test with unsaved speech settings
    return ok;
}
