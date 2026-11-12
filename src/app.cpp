// FastChat: startup and shutdown.

#include "ui/dialogs.h"
#include "ui/main_frame.h"
#include "ui/ui_util.h"
#include "core/database.h"
#include "core/notify.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/settings.h"
#include "core/sound.h"
#include "core/speech.h"
#include "version.h"

#include <wx/snglinst.h>

#include <cstdlib>
#include <exception>
#include <memory>

#ifdef __APPLE__
// An app opened from the Finder or the Dock gets only /usr/bin:/bin:/usr/sbin:/sbin,
// so Claude Code, Codex (and the node it runs on) and the tools that code
// conversations run - git, cmake, Homebrew's - would not be found. Take the PATH
// the user's own shell sets up, as Terminal would have it.
static void AdoptLoginShellPath() {
    const char* shell = std::getenv("SHELL");
    std::string sh = shell && *shell ? shell : "/bin/zsh";
    const std::string mark = "__FASTCHAT_PATH__";
    ProcessResult r = RunShellCommand("'" + sh + "' -ilc 'printf \"" + mark + "%s" + mark + "\" \"$PATH\"'",
                                      GetHomeDir(), 5000, 1 << 16, nullptr);
    std::string path;
    size_t a = r.output.find(mark);
    size_t b = a == std::string::npos ? a : r.output.find(mark, a + mark.size());
    if (b != std::string::npos) path = r.output.substr(a + mark.size(), b - a - mark.size());
    const char* current = std::getenv("PATH");
    std::string merged = path.empty() ? std::string(current ? current : "/usr/bin:/bin:/usr/sbin:/sbin") : path;
    // Where Claude Code and Homebrew install, in case the shell did not say.
    for (const std::string& dir : {GetHomeDir() + "/.local/bin", std::string("/opt/homebrew/bin"),
                                   std::string("/usr/local/bin")}) {
        if ((":" + merged + ":").find(":" + dir + ":") == std::string::npos) merged += ":" + dir;
    }
    setenv("PATH", merged.c_str(), 1);
}
#endif

class FastChatApp : public wxApp {
public:
    bool OnInit() override {
        wxDISABLE_ASSERTS_IN_RELEASE_BUILD();
        SetAppName(APP_NAME);
        // Anything that still ends the program leaves a line in the log first.
        std::set_terminate([]() {
            std::string what = "terminate called";
            if (std::exception_ptr e = std::current_exception()) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    what = std::string("uncaught exception: ") + ex.what();
                } catch (...) {
                    what = "uncaught exception";
                }
            }
            AppendLog("Fatal: " + what);
            std::abort();
        });

#ifdef __APPLE__
        AdoptLoginShellPath();
#endif

        m_instance = std::make_unique<wxSingleInstanceChecker>();
        if (m_instance->Create("FastChat-" + wxGetUserId()) && m_instance->IsAnotherRunning()) {
            wxMessageBox("FastChat is already running.", APP_NAME, wxOK | wxICON_INFORMATION);
            return false;
        }

        AppSettings().Load();
        std::string dbPath = JoinPath(GetDataDir(), "fastchat.db");
        if (!AppDatabase().Open(dbPath)) {
            wxMessageBox("FastChat could not open its database at " + U(dbPath) + ":\n" + U(AppDatabase().LastError()),
                         APP_NAME, wxOK | wxICON_ERROR);
            return false;
        }
        AppProviderCache().Load();
        AppSound().Init();
        ApplyOutputSettings();

        auto* frame = new MainFrame();
        if (auto it = AppSettings().windowState.find("main"); it != AppSettings().windowState.end()) {
            const json& w = *it;
            if (w.contains("w")) frame->SetSize(wxSize(w.value("w", 900), w.value("h", 600)));
            if (w.value("maximized", false)) frame->Maximize();
        }
        SetTopWindow(frame);
        frame->Show();

        if (AppSettings().providers.empty()) {
            CallAfter([frame]() {
                if (AskYesNo(frame,
                             "Welcome to FastChat. To start, add an AI provider such as OpenRouter, OpenAI, Anthropic, "
                             "Google Gemini or Ollama. Open the provider settings now?",
                             "Welcome")) {
                    ShowSettingsDialog(frame, 1);
                    frame->UpdateStatus();
                }
            });
        }
        return true;
    }

    // An exception that reaches the event loop: log it, say so, and carry on
    // rather than ending the program and losing what the user was doing.
    bool OnExceptionInMainLoop() override {
        std::string what = "unknown exception";
        try {
            throw;
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        AppendLog("Unhandled exception in the main loop: " + what);
        wxMessageBox("Something went wrong: " + U(what) + "\n\nFastChat will keep running. The details are in " +
                         U(JoinPath(GetDataDir(), "fastchat.log")) + ".",
                     APP_NAME, wxOK | wxICON_ERROR);
        return true;
    }

    // Ctrl, pressed anywhere in FastChat, silences speech (as it does a screen reader).
    // On the Mac that is Control, as in VoiceOver, not Command.
    int FilterEvent(wxEvent& event) override {
        if (event.GetEventType() == wxEVT_KEY_DOWN && AppSettings().speech.ctrlStops) {
            int key = static_cast<wxKeyEvent&>(event).GetKeyCode();
#ifdef __APPLE__
            if (key == WXK_RAW_CONTROL) AppSpeech().Stop();
#else
            if (key == WXK_CONTROL || key == WXK_RAW_CONTROL) AppSpeech().Stop();
#endif
        }
        return Event_Skip;
    }

    void OnUnhandledException() override {
        AppendLog("Unhandled exception outside the main loop");
    }

    int OnExit() override {
        if (MainFrame* frame = GetMainFrame()) {
            (void)frame;
        }
        AppSettings().Save();
        AppSpeech().Shutdown();
        AppSound().Shutdown();
        AppDatabase().Close();
        m_instance.reset();
        return wxApp::OnExit();
    }

private:
    std::unique_ptr<wxSingleInstanceChecker> m_instance;
};

wxIMPLEMENT_APP(FastChatApp);
