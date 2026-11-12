#pragma once

// The main window: the list of conversations.

#include "core/conversation.h"
#include "core/updater.h"

#include <wx/listctrl.h>
#include <wx/wx.h>

#include <map>
#include <memory>
#include <vector>

class ConversationFrame;

class MainFrame : public wxFrame {
public:
    MainFrame();

    // Reload the list from the database, keeping (or moving to) a selection.
    void RefreshList(int64_t selectId = 0);
    void OpenConversation(int64_t id);
    // Called by conversation windows.
    void ConversationChanged(int64_t id);
    void ConversationClosed(int64_t id);
    void UpdateStatus();
    // The update settings changed: check on the new schedule.
    void ScheduleUpdateChecks();

private:
    // Updates (Help > Check for updates, and the automatic checks).
    void CheckForUpdates(bool automatic);
    void UpdateChecked(const UpdateInfo& info, bool automatic);
    void InstallUpdate(const UpdateInfo& info, bool automatic);
    bool AnyConversationBusy() const;
    void BuildMenu();
    void Fill();
    int64_t SelectedId() const;
    Conversation* Selected();
    std::string DisplayTitle(const Conversation& c) const;

    void OnNew(wxCommandEvent&);
    void OnOpen(wxCommandEvent&);
    void OnRename(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnPin(wxCommandEvent&);
    void OnDuplicate(wxCommandEvent&);
    void OnConversationSettings(wxCommandEvent&);
    void OnOpenFolder(wxCommandEvent&);
    void OnExport(wxCommandEvent&);
    void OnImport(wxCommandEvent&);
    void OnSettings(wxCommandEvent&);
    void OnAccount(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);
    void OnContextMenu(wxContextMenuEvent&);
    void OnClose(wxCloseEvent&);

    long SelectedRow() const;
    void SelectRow(long row);
    long RowCount() const;

    wxTextCtrl* m_search;
    // A report list with columns. On the Mac, wxListCtrl is drawn by wxWidgets
    // and VoiceOver cannot read it, so there it is a list box (a native table)
    // with each conversation on one line.
#ifdef __WXOSX__
    wxListBox* m_list;
#else
    wxListCtrl* m_list;
#endif
    std::vector<Conversation> m_all;
    std::vector<Conversation> m_shown;
    std::map<int64_t, ConversationFrame*> m_open;
    wxTimer m_updateTimer;         // the next automatic check
    int m_updateTimerHours = -1;   // what it is set for (-1: not set)
    bool m_updateChecking = false;
    bool m_updateInstalling = false;
    // An automatic install waiting for responses to finish.
    std::unique_ptr<UpdateInfo> m_pendingUpdate;
    wxTimer m_pendingTimer;
};

MainFrame* GetMainFrame();

// Write a conversation as text (for Export).
std::string ConversationToText(const Conversation& c, const std::vector<Message>& messages);
