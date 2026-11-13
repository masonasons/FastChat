#include "ui/ui_util.h"
#include "core/providers/provider.h"
#include "core/speech.h"

#include <wx/clipbrd.h>
#ifdef __WXOSX__
#include <wx/osx/core/private.h>
#endif

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

bool IsDeleteKey(int keyCode, int modifiers) {
    if (keyCode == WXK_DELETE || keyCode == WXK_NUMPAD_DELETE) return modifiers == wxMOD_NONE;
#ifdef __WXOSX__
    if (keyCode == WXK_BACK) return modifiers == wxMOD_CMD;
#endif
    return false;
}

void ShowError(wxWindow* parent, const wxString& message, const wxString& title) {
    wxMessageBox(message, title, wxOK | wxICON_ERROR, parent);
}

void LinkLabel(wxStaticText* text, wxWindow* control) {
    // Screen readers name a control after the label just before it in window
    // order, and the control may have been created first. wxWidgets reorders
    // only its own list; on Windows the native order has to move too.
    text->MoveBeforeInTabOrder(control);
#ifdef __WXMSW__
    HWND before = ::GetWindow(static_cast<HWND>(control->GetHWND()), GW_HWNDPREV);
    ::SetWindowPos(static_cast<HWND>(text->GetHWND()), before ? before : HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#elif defined(__WXOSX__)
    // VoiceOver takes a control's name from the label created just before it,
    // which here is the previous row's: point it at its own.
    if (control->GetPeer() && text->GetLabelPeer()) control->GetPeer()->SetAccessibilityTitleElement(text->GetLabelPeer());
#endif
}

wxBoxSizer* LabeledRow(wxWindow* parent, const wxString& label, wxWindow* control, int proportion) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxStaticText(parent, wxID_ANY, label);
    LinkLabel(text, control);
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

wxString EffortName(const std::string& key) {
    for (const auto& [k, label] : EffortChoices()) {
        if (k == key) return label;
    }
    return U(key);
}

void VoiceChoice::Load(const std::string& engine, const std::string& prismBackend, const std::string& want) {
    m_want = want;
    int generation = ++m_generation;
    std::vector<std::string> voices;
    if (Speech::CachedVoices(engine, prismBackend, voices)) {
        Fill(voices);
        return;
    }
    m_loaded = false;
    m_choice->Clear();
    m_choice->Append("Loading voices...");
    m_choice->SetSelection(0);
    m_choice->Disable();
    std::shared_ptr<bool> alive = m_alive;
    Speech::LoadVoices(engine, prismBackend, [this, alive, generation](const std::vector<std::string>& list) {
        PostToUi([this, alive, generation, list]() {
            if (*alive && generation == m_generation) Fill(list);
        });
    });
}

void VoiceChoice::Fill(const std::vector<std::string>& voices) {
    wxArrayString items;
    items.reserve(voices.size() + 1);
    items.Add(m_first);
    int select = 0;
    for (const auto& v : voices) {
        if (v == m_want) select = static_cast<int>(items.size());
        items.Add(U(v));
    }
    // All at once: item by item, hundreds of voices take long to add.
    m_choice->Freeze();
    m_choice->Clear();
    m_choice->Append(items);
    m_choice->SetSelection(select);
    m_choice->Thaw();
    m_choice->Enable(items.size() > 1);
    m_loaded = true;
}

std::string VoiceChoice::Selected() const {
    if (!m_loaded) return m_want;
    return m_choice->GetSelection() > 0 ? S(m_choice->GetStringSelection()) : std::string();
}

std::string ModelDisplayName(const std::string& providerId, const std::string& modelId) {
    if (modelId.empty()) return "no model";
    ModelInfo info;
    if (AppProviderCache().FindModel(providerId, modelId, info) && !info.name.empty()) return info.name;
    return modelId;
}
