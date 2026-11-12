#pragma once

// The list of events with each one's sound and speech, used by the global
// settings (edits EventConfig) and a conversation's settings (edits overrides,
// where every part can follow the global setting).

#include "core/conversation.h"
#include "core/settings.h"

#include <wx/wx.h>

class EventSettingsPanel : public wxPanel {
public:
    // Global: edits `configs` (EventId::Count entries) when Save() is called.
    EventSettingsPanel(wxWindow* parent, EventConfig* configs);
    // Per conversation: edits `overrides`.
    EventSettingsPanel(wxWindow* parent, EventOverride* overrides);

    // Write the controls back into the arrays given at construction.
    void Save();

private:
    void Build();
    void Load(int index);
    void Store(int index);
    void UpdateEnabled();
    void Browse();
    void Preview();
    void RefreshListLabel(int index);
    wxString ListLabel(int index) const;

    bool m_override;
    EventConfig* m_configs = nullptr;
    EventOverride* m_overrides = nullptr;
    EventConfig m_workConfigs[static_cast<int>(EventId::Count)];
    EventOverride m_workOverrides[static_cast<int>(EventId::Count)];
    int m_current = -1;

    wxListBox* m_list = nullptr;
    // global
    wxCheckBox* m_sound = nullptr;
    wxCheckBox* m_speak = nullptr;
    // override
    wxChoice* m_soundChoice = nullptr;
    wxCheckBox* m_ownFile = nullptr;
    wxChoice* m_speakChoice = nullptr;
    wxCheckBox* m_ownText = nullptr;
    // both
    wxTextCtrl* m_file = nullptr;
    wxButton* m_browse = nullptr;
    wxButton* m_builtIn = nullptr;
    wxButton* m_play = nullptr;
    wxTextCtrl* m_text = nullptr;
};
