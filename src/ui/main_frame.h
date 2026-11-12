#pragma once

// The main window: the list of conversations.

#include "core/conversation.h"

#include <wx/listctrl.h>
#include <wx/wx.h>

#include <map>
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

private:
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
    void OnSettings(wxCommandEvent&);
    void OnAccount(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);
    void OnContextMenu(wxContextMenuEvent&);
    void OnClose(wxCloseEvent&);

    wxTextCtrl* m_search;
    wxListCtrl* m_list;
    std::vector<Conversation> m_all;
    std::vector<Conversation> m_shown;
    std::map<int64_t, ConversationFrame*> m_open;
};

MainFrame* GetMainFrame();

// Write a conversation as text (for Export).
std::string ConversationToText(const Conversation& c, const std::vector<Message>& messages);
