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

#include <exception>
#include <memory>

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
