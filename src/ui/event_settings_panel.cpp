#include "ui/event_settings_panel.h"
#include "ui/ui_util.h"
#include "core/notify.h"
#include "core/soundpack.h"

#include <wx/filedlg.h>

static const int kCount = static_cast<int>(EventId::Count);

EventSettingsPanel::EventSettingsPanel(wxWindow* parent, EventConfig* configs)
    : wxPanel(parent), m_override(false), m_configs(configs) {
    for (int i = 0; i < kCount; i++) m_workConfigs[i] = configs[i];
    Build();
}

EventSettingsPanel::EventSettingsPanel(wxWindow* parent, EventOverride* overrides)
    : wxPanel(parent), m_override(true), m_overrides(overrides) {
    for (int i = 0; i < kCount; i++) m_workOverrides[i] = overrides[i];
    Build();
}

void EventSettingsPanel::Build() {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, "&Events"), 0, wxLEFT | wxRIGHT | wxTOP, 4);
    m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 180));
    for (int i = 0; i < kCount; i++) m_list->Append(ListLabel(i));
    sizer->Add(m_list, 1, wxEXPAND | wxALL, 4);

    if (m_override) {
        m_soundChoice = new wxChoice(this, wxID_ANY);
        m_soundChoice->Append("Default");
        m_soundChoice->Append("On");
        m_soundChoice->Append("Off");
        sizer->Add(LabeledRow(this, "&Sound:", m_soundChoice, 0), 0, wxALL, 4);
        m_ownFile = new wxCheckBox(this, wxID_ANY, "Use a &different sound for this conversation");
        sizer->Add(m_ownFile, 0, wxALL, 4);
    } else {
        m_sound = new wxCheckBox(this, wxID_ANY, "Play a &sound");
        sizer->Add(m_sound, 0, wxALL, 4);
    }

    auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
    fileRow->Add(new wxStaticText(this, wxID_ANY, "Sound &file:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    m_file = new wxTextCtrl(this, wxID_ANY);
    m_file->SetHint("Empty: the sound pack's sound, or the built-in tone");
    fileRow->Add(m_file, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    m_browse = new wxButton(this, wxID_ANY, "&Browse...");
    m_builtIn = new wxButton(this, wxID_ANY, "Defa&ult sound");
    m_play = new wxButton(this, wxID_ANY, "&Play");
    fileRow->Add(m_browse, 0, wxRIGHT, 4);
    fileRow->Add(m_builtIn, 0, wxRIGHT, 4);
    fileRow->Add(m_play, 0);
    sizer->Add(fileRow, 0, wxEXPAND | wxALL, 4);

    if (m_override) {
        m_speakChoice = new wxChoice(this, wxID_ANY);
        m_speakChoice->Append("Default");
        m_speakChoice->Append("On");
        m_speakChoice->Append("Off");
        sizer->Add(LabeledRow(this, "Spea&k:", m_speakChoice, 0), 0, wxALL, 4);
        m_ownText = new wxCheckBox(this, wxID_ANY, "Use di&fferent spoken text for this conversation");
        sizer->Add(m_ownText, 0, wxALL, 4);
    } else {
        m_speak = new wxCheckBox(this, wxID_ANY, "Spea&k");
        sizer->Add(m_speak, 0, wxALL, 4);
    }
    m_text = new wxTextCtrl(this, wxID_ANY);
    sizer->Add(LabeledRow(this, "Spoken &text:", m_text), 0, wxEXPAND | wxALL, 4);
    sizer->Add(new wxStaticText(this, wxID_ANY, wxString("The text can include ") + EventPlaceholderHelp()), 0,
               wxALL, 4);
    SetSizer(sizer);

    m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
        if (m_current >= 0) Store(m_current);
        Load(m_list->GetSelection());
    });
    auto changed = [this](wxCommandEvent&) {
        UpdateEnabled();
        if (m_current >= 0) {
            Store(m_current);
            RefreshListLabel(m_current);
        }
    };
    if (m_sound) m_sound->Bind(wxEVT_CHECKBOX, changed);
    if (m_speak) m_speak->Bind(wxEVT_CHECKBOX, changed);
    if (m_soundChoice) m_soundChoice->Bind(wxEVT_CHOICE, changed);
    if (m_speakChoice) m_speakChoice->Bind(wxEVT_CHOICE, changed);
    if (m_ownFile) m_ownFile->Bind(wxEVT_CHECKBOX, changed);
    if (m_ownText) m_ownText->Bind(wxEVT_CHECKBOX, changed);
    m_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Browse(); });
    m_builtIn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_file->SetValue(""); });
    m_play->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Preview(); });

    m_list->SetSelection(0);
    Load(0);
}

wxString EventSettingsPanel::ListLabel(int i) const {
    const EventInfo& info = GetEventInfo(static_cast<EventId>(i));
    wxString label = info.label;
    if (m_override) {
        const EventOverride& o = m_workOverrides[i];
        if (o.Empty()) return label + ": default";
        wxString parts;
        if (o.soundEnabled) parts += *o.soundEnabled ? "sound on" : "sound off";
        if (o.soundFile) parts += wxString(parts.empty() ? "" : ", ") + "own sound";
        if (o.speakEnabled) parts += wxString(parts.empty() ? "" : ", ") + (*o.speakEnabled ? "spoken" : "not spoken");
        if (o.speechText) parts += wxString(parts.empty() ? "" : ", ") + "own text";
        return label + ": " + parts;
    }
    const EventConfig& c = m_workConfigs[i];
    wxString parts = "no sound";
    if (c.soundEnabled) {
        if (!c.soundFile.empty()) parts = "sound file";
        else if (PackHasEventSound(FindSoundPackFolder(m_pack), info.key)) parts = "sound pack";
        else parts = "tone";
    }
    parts += c.speakEnabled ? ", spoken" : ", not spoken";
    return label + ": " + parts;
}

void EventSettingsPanel::RefreshListLabel(int index) {
    if (index >= 0 && index < kCount && m_list->GetString(index) != ListLabel(index)) {
        m_list->SetString(index, ListLabel(index));
    }
}

void EventSettingsPanel::Load(int index) {
    if (index < 0 || index >= kCount) return;
    m_current = index;
    if (m_override) {
        const EventOverride& o = m_workOverrides[index];
        const EventConfig& g = AppSettings().events[index];
        m_soundChoice->SetString(0, g.soundEnabled ? "Default (on)" : "Default (off)");
        m_speakChoice->SetString(0, g.speakEnabled ? "Default (on)" : "Default (off)");
        SetTriChoice(m_soundChoice, o.soundEnabled);
        SetTriChoice(m_speakChoice, o.speakEnabled);
        m_ownFile->SetValue(o.soundFile.has_value());
        m_file->ChangeValue(U(o.soundFile ? *o.soundFile : g.soundFile));
        m_ownText->SetValue(o.speechText.has_value());
        m_text->ChangeValue(U(o.speechText ? *o.speechText : g.speechText));
    } else {
        const EventConfig& c = m_workConfigs[index];
        m_sound->SetValue(c.soundEnabled);
        m_speak->SetValue(c.speakEnabled);
        m_file->ChangeValue(U(c.soundFile));
        m_text->ChangeValue(U(c.speechText));
    }
    UpdateEnabled();
}

void EventSettingsPanel::Store(int index) {
    if (index < 0 || index >= kCount) return;
    if (m_override) {
        EventOverride& o = m_workOverrides[index];
        o.soundEnabled = GetTriChoice(m_soundChoice);
        o.speakEnabled = GetTriChoice(m_speakChoice);
        if (m_ownFile->GetValue()) o.soundFile = S(m_file->GetValue());
        else o.soundFile.reset();
        if (m_ownText->GetValue()) o.speechText = S(m_text->GetValue());
        else o.speechText.reset();
    } else {
        EventConfig& c = m_workConfigs[index];
        c.soundEnabled = m_sound->GetValue();
        c.speakEnabled = m_speak->GetValue();
        c.soundFile = S(m_file->GetValue());
        c.speechText = S(m_text->GetValue());
    }
}

void EventSettingsPanel::UpdateEnabled() {
    if (m_override) {
        bool file = m_ownFile->GetValue();
        m_file->Enable(file);
        m_browse->Enable(file);
        m_builtIn->Enable(file);
        m_text->Enable(m_ownText->GetValue());
    } else {
        bool sound = m_sound->GetValue();
        m_file->Enable(sound);
        m_browse->Enable(sound);
        m_builtIn->Enable(sound);
        m_text->Enable(m_speak->GetValue());
    }
}

void EventSettingsPanel::Browse() {
    wxFileDialog dlg(this, "Choose a sound", "", "", "Sounds (*.wav;*.mp3;*.flac;*.ogg)|*.wav;*.mp3;*.flac;*.ogg|All files|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        m_file->SetValue(dlg.GetPath());
        if (m_current >= 0) {
            Store(m_current);
            RefreshListLabel(m_current);
        }
    }
}

void EventSettingsPanel::Preview() {
    if (m_current < 0) return;
    EventConfig c;
    c.soundFile = S(m_file->GetValue());
    PreviewEventSound(static_cast<EventId>(m_current), c, m_pack);
}

void EventSettingsPanel::SetSoundPack(const std::string& pack) {
    m_pack = pack;
    for (int i = 0; i < kCount; i++) RefreshListLabel(i);
}

void EventSettingsPanel::Save() {
    if (m_current >= 0) Store(m_current);
    for (int i = 0; i < kCount; i++) {
        if (m_override) m_overrides[i] = m_workOverrides[i];
        else m_configs[i] = m_workConfigs[i];
    }
}
