#pragma once

// Helpers shared by FastChat's windows.

#include <wx/wx.h>
#include <wx/weakref.h>

#include "core/platform.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// UTF-8 std::string <-> wxString
inline wxString U(const std::string& s) { return wxString::FromUTF8(s.data(), s.size()); }
inline std::string S(const wxString& s) {
    const wxScopedCharBuffer buf = s.utf8_str();
    return std::string(buf.data(), buf.length());
}

// Run `work` on a worker thread, then `done` with its result on the UI thread -
// unless `owner` has been destroyed by then.
template <typename T>
void RunInBackground(wxWindow* owner, std::function<T()> work, std::function<void(T)> done) {
    wxWeakRef<wxWindow> ref(owner);
    std::thread([ref, work = std::move(work), done = std::move(done)]() mutable {
        T result{};
        try {
            result = work();
        } catch (const std::exception& e) {
            AppendLog(std::string("Background task: ") + e.what());
            return;
        } catch (...) {
            AppendLog("Background task: unknown exception");
            return;
        }
        if (!wxTheApp) return;
        wxTheApp->CallAfter([ref, done = std::move(done), result = std::move(result)]() mutable {
            if (ref.get()) done(std::move(result));
        });
    }).detach();
}

// Run on the UI thread later (any thread).
void PostToUi(std::function<void()> fn);

bool CopyToClipboard(const std::string& text);
bool AskYesNo(wxWindow* parent, const wxString& question, const wxString& title);
// Delete in a list: the Delete key, and on the Mac also Command+Delete (Backspace),
// since most Mac keyboards have no forward delete. `modifiers` is wxMOD_*.
bool IsDeleteKey(int keyCode, int modifiers);
void ShowError(wxWindow* parent, const wxString& message, const wxString& title = "FastChat");

// A static label followed by a control, in a horizontal row (the label names
// the control for screen readers). Returns the row sizer.
// Make `text` the label screen readers give `control` (for rows built by hand).
void LinkLabel(wxStaticText* text, wxWindow* control);
wxBoxSizer* LabeledRow(wxWindow* parent, const wxString& label, wxWindow* control, int proportion = 1);

// A three-way choice for per-conversation overrides: "Default (on)", "On", "Off".
wxChoice* MakeTriChoice(wxWindow* parent, bool globalValue);
void SetTriChoice(wxChoice* choice, const std::optional<bool>& value);
std::optional<bool> GetTriChoice(wxChoice* choice);

// The names of reasoning effort settings, with "" first ("Model default").
const std::vector<std::pair<std::string, wxString>>& EffortChoices();
// One effort setting's name ("High"; "" gives "Model default").
wxString EffortName(const std::string& key);

// Fills a choice with a speech engine's voices after a first item ("Default"),
// without holding up the window: the voices are listed on a worker thread the
// first time (with many voices that takes a while) and kept for later.
class VoiceChoice {
public:
    VoiceChoice(wxChoice* choice, const wxString& first) : m_choice(choice), m_first(first) {}
    ~VoiceChoice() { *m_alive = false; }
    VoiceChoice(const VoiceChoice&) = delete;
    VoiceChoice& operator=(const VoiceChoice&) = delete;
    // Lists the engine's voices, selecting `want` ("" for the first item).
    void Load(const std::string& engine, const std::string& prismBackend, const std::string& want);
    // The chosen voice, "" for the first item; while listing, the one wanted.
    std::string Selected() const;

private:
    void Fill(const std::vector<std::string>& voices);

    wxChoice* m_choice;
    wxString m_first;
    std::string m_want;
    bool m_loaded = false;
    int m_generation = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// A model's name for lists and the status bar: "Claude Opus 5.5" or the id.
std::string ModelDisplayName(const std::string& providerId, const std::string& modelId);
