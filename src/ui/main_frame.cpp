#include "ui/main_frame.h"
#include "ui/conversation_frame.h"
#include "ui/dialogs.h"
#include "ui/ui_util.h"
#include "core/database.h"
#include "core/notify.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/util.h"
#include "version.h"

#include <wx/filedlg.h>
#include <wx/filename.h>

namespace {

MainFrame* g_mainFrame = nullptr;

enum {
    ID_NEW = wxID_HIGHEST + 1,
    ID_OPEN,
    ID_RENAME,
    ID_DELETE,
    ID_PIN,
    ID_DUPLICATE,
    ID_CONV_SETTINGS,
    ID_OPEN_FOLDER,
    ID_EXPORT,
    ID_SETTINGS,
    ID_ACCOUNT,
    ID_SEARCH_FOCUS,
    ID_LIST_FOCUS,
};

enum Column { COL_NAME, COL_TYPE, COL_MODEL, COL_UPDATED, COL_MESSAGES, COL_COST };

}  // namespace

MainFrame* GetMainFrame() {
    return g_mainFrame;
}

MainFrame::MainFrame() : wxFrame(nullptr, wxID_ANY, APP_NAME, wxDefaultPosition, wxSize(900, 600)) {
    g_mainFrame = this;
    auto* panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_search = new wxTextCtrl(panel, wxID_ANY);
    m_search->SetHint("Type to filter conversations");
    sizer->Add(LabeledRow(panel, "&Search:", m_search), 0, wxEXPAND | wxALL, 6);

    sizer->Add(new wxStaticText(panel, wxID_ANY, "&Conversations"), 0, wxLEFT | wxRIGHT, 6);
    m_list = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn("Name", wxLIST_FORMAT_LEFT, 320);
    m_list->AppendColumn("Type", wxLIST_FORMAT_LEFT, 60);
    m_list->AppendColumn("Model", wxLIST_FORMAT_LEFT, 180);
    m_list->AppendColumn("Last used", wxLIST_FORMAT_LEFT, 140);
    m_list->AppendColumn("Messages", wxLIST_FORMAT_RIGHT, 80);
    m_list->AppendColumn("Cost", wxLIST_FORMAT_RIGHT, 80);
    sizer->Add(m_list, 1, wxEXPAND | wxALL, 6);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* newButton = new wxButton(panel, ID_NEW, "&New conversation");
    auto* settingsButton = new wxButton(panel, ID_SETTINGS, "Se&ttings");
    buttons->Add(newButton, 0, wxRIGHT, 6);
    buttons->Add(settingsButton, 0);
    sizer->Add(buttons, 0, wxALL, 6);
    panel->SetSizer(sizer);

    CreateStatusBar(2);
    int widths[] = {-2, -3};
    GetStatusBar()->SetStatusWidths(2, widths);
    BuildMenu();

    Bind(wxEVT_MENU, &MainFrame::OnNew, this, ID_NEW);
    Bind(wxEVT_BUTTON, &MainFrame::OnNew, this, ID_NEW);
    Bind(wxEVT_MENU, &MainFrame::OnOpen, this, ID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::OnRename, this, ID_RENAME);
    Bind(wxEVT_MENU, &MainFrame::OnDelete, this, ID_DELETE);
    Bind(wxEVT_MENU, &MainFrame::OnPin, this, ID_PIN);
    Bind(wxEVT_MENU, &MainFrame::OnDuplicate, this, ID_DUPLICATE);
    Bind(wxEVT_MENU, &MainFrame::OnConversationSettings, this, ID_CONV_SETTINGS);
    Bind(wxEVT_MENU, &MainFrame::OnOpenFolder, this, ID_OPEN_FOLDER);
    Bind(wxEVT_MENU, &MainFrame::OnExport, this, ID_EXPORT);
    Bind(wxEVT_MENU, &MainFrame::OnSettings, this, ID_SETTINGS);
    Bind(wxEVT_BUTTON, &MainFrame::OnSettings, this, ID_SETTINGS);
    Bind(wxEVT_MENU, &MainFrame::OnAccount, this, ID_ACCOUNT);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_search->SetFocus(); }, ID_SEARCH_FOCUS);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Fill(); });
    m_search->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_DOWN && m_list->GetItemCount() > 0) {
            m_list->SetFocus();
            return;
        }
        e.Skip();
    });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
        wxCommandEvent e;
        OnOpen(e);
    });
    m_list->Bind(wxEVT_LIST_KEY_DOWN, [this](wxListEvent& e) {
        wxCommandEvent ce;
        if (e.GetKeyCode() == WXK_DELETE) OnDelete(ce);
        else e.Skip();
    });
    m_list->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnContextMenu, this);

    RefreshList();
    UpdateStatus();
    m_list->SetFocus();
}

void MainFrame::BuildMenu() {
    auto* file = new wxMenu();
    file->Append(ID_NEW, "&New conversation...\tCtrl+N");
    file->Append(ID_OPEN, "&Open\tCtrl+O");
    file->AppendSeparator();
    file->Append(ID_SETTINGS, "&Settings...\tCtrl+,");
    file->Append(ID_ACCOUNT, "&Account and credits...\tCtrl+Shift+A");
    file->AppendSeparator();
    file->Append(wxID_EXIT, "E&xit");

    auto* conv = new wxMenu();
    conv->Append(ID_RENAME, "&Rename...\tF2");
    conv->Append(ID_PIN, "&Pin or unpin\tCtrl+P");
    conv->Append(ID_DUPLICATE, "D&uplicate");
    conv->Append(ID_CONV_SETTINGS, "Conversation &settings...\tCtrl+Shift+,");
    conv->Append(ID_OPEN_FOLDER, "Open &folder");
    conv->Append(ID_EXPORT, "&Export...\tCtrl+E");
    conv->AppendSeparator();
    conv->Append(ID_DELETE, "&Delete\tDel");
    conv->AppendSeparator();
    conv->Append(ID_SEARCH_FOCUS, "Searc&h\tCtrl+F");

    auto* help = new wxMenu();
    help->Append(wxID_ABOUT, "&About FastChat");

    auto* bar = new wxMenuBar();
    bar->Append(file, "&File");
    bar->Append(conv, "&Conversation");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
}

std::string MainFrame::DisplayTitle(const Conversation& c) const {
    if (!c.title.empty()) return c.title;
    if (c.type == ConversationType::Code && !c.folder.empty()) {
        return PathToUtf8(PathFromUtf8(c.folder).filename()) + " (code)";
    }
    if (c.type == ConversationType::Computer) return "Untitled computer use";
    return "Untitled " + std::string(c.type == ConversationType::Code ? "code conversation" : "chat");
}

void MainFrame::RefreshList(int64_t selectId) {
    if (!selectId) selectId = SelectedId();
    m_all = AppDatabase().ListConversations();
    Fill();
    for (size_t i = 0; i < m_shown.size(); i++) {
        if (m_shown[i].id == selectId) {
            m_list->SetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(static_cast<long>(i));
            break;
        }
    }
    UpdateStatus();
}

void MainFrame::Fill() {
    int64_t selected = SelectedId();
    std::string filter = ToLower(Trim(S(m_search->GetValue())));
    m_shown.clear();
    for (const auto& c : m_all) {
        if (!filter.empty() && ToLower(DisplayTitle(c)).find(filter) == std::string::npos &&
            ToLower(c.folder).find(filter) == std::string::npos) {
            continue;
        }
        m_shown.push_back(c);
    }
    m_list->Freeze();
    m_list->DeleteAllItems();
    long selectRow = m_shown.empty() ? -1 : 0;
    for (size_t i = 0; i < m_shown.size(); i++) {
        const Conversation& c = m_shown[i];
        long row = m_list->InsertItem(static_cast<long>(i), U((c.pinned ? "Pinned: " : "") + DisplayTitle(c)));
        m_list->SetItem(row, COL_TYPE, ConversationTypeName(c.type));
        std::string model;
        if (!c.settings.model.empty()) model = ModelDisplayName(c.settings.providerId, c.settings.model);
        else model = "default";
        m_list->SetItem(row, COL_MODEL, U(model));
        m_list->SetItem(row, COL_UPDATED, U(FormatRelativeTime(c.updatedAt)));
        m_list->SetItem(row, COL_MESSAGES, wxString::Format("%lld", static_cast<long long>(c.messageCount)));
        m_list->SetItem(row, COL_COST, c.totalCost > 0 ? U(FormatMoney(c.totalCost)) : wxString(""));
        if (c.id == selected) selectRow = row;
    }
    if (selectRow >= 0) {
        m_list->SetItemState(selectRow, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                             wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }
    m_list->Thaw();
}

int64_t MainFrame::SelectedId() const {
    long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(m_shown.size())) return 0;
    return m_shown[row].id;
}

Conversation* MainFrame::Selected() {
    long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(m_shown.size())) return nullptr;
    return &m_shown[row];
}

void MainFrame::UpdateStatus() {
    SetStatusText(wxString::Format("%zu conversations", m_all.size()), 0);
    const ProviderConfig* p = AppSettings().DefaultProvider();
    if (!p) {
        SetStatusText("No provider yet: Settings (Ctrl+Comma), Providers", 1);
        return;
    }
    std::string model = AppSettings().general.defaultModel.empty() ? CreateProvider(*p)->DefaultModel()
                                                                   : AppSettings().general.defaultModel;
    wxString text = "Default: " + U(ModelDisplayName(p->id, model)) + " on " + U(p->name);
    AccountInfo account;
    if (AppProviderCache().GetAccount(p->id, account)) {
        if (account.creditsRemaining >= 0) text += ", credits " + U(FormatMoney(account.creditsRemaining));
        else if (!account.summary.empty()) text += ", " + U(account.summary);
    }
    SetStatusText(text, 1);
}

void MainFrame::OpenConversation(int64_t id) {
    auto it = m_open.find(id);
    if (it != m_open.end()) {
        it->second->Raise();
        it->second->Show();
        it->second->FocusInput();
        return;
    }
    Conversation c;
    if (!AppDatabase().GetConversation(id, c)) return;
    auto* frame = new ConversationFrame(this, c);
    m_open[id] = frame;
    frame->Show();
    frame->Raise();
}

void MainFrame::ConversationChanged(int64_t id) {
    RefreshList(SelectedId() ? SelectedId() : id);
}

void MainFrame::ConversationClosed(int64_t id) {
    m_open.erase(id);
    RefreshList();
}

void MainFrame::OnNew(wxCommandEvent&) {
    if (AppSettings().providers.empty()) {
        if (AskYesNo(this, "FastChat needs an AI provider first (OpenRouter, OpenAI, Anthropic, Gemini, Ollama...). "
                           "Add one now?",
                     "No provider")) {
            ShowSettingsDialog(this, 1);
            UpdateStatus();
        }
        if (AppSettings().providers.empty()) return;
    }
    NewConversationChoice choice;
    if (!AskNewConversation(this, choice)) return;
    Conversation c;
    c.title = Trim(choice.title);
    c.type = choice.type;
    c.folder = choice.folder;
    c.settings.providerId = choice.providerId;
    c.settings.model = choice.model;
    if (c.title.empty() && !AppSettings().general.autoTitle) {
        c.title = std::string(c.type == ConversationType::Code ? "Code " : c.type == ConversationType::Computer ? "Computer use " : "Chat ") +
                  FormatLocalTime(NowSeconds());
    }
    if (!AppDatabase().CreateConversation(c)) {
        ShowError(this, "The conversation could not be created: " + U(AppDatabase().LastError()));
        return;
    }
    EventContext ctx;
    ctx.Set("conversation", DisplayTitle(c));
    NotifyEvent(EventId::ConversationCreated, ctx, &c.settings);
    RefreshList(c.id);
    OpenConversation(c.id);
}

void MainFrame::OnOpen(wxCommandEvent&) {
    if (int64_t id = SelectedId()) OpenConversation(id);
}

void MainFrame::OnRename(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    wxTextEntryDialog dlg(this, "Name:", "Rename conversation", U(c->title));
    if (dlg.ShowModal() != wxID_OK) return;
    Conversation fresh;
    if (!AppDatabase().GetConversation(c->id, fresh)) return;
    fresh.title = Trim(S(dlg.GetValue()));
    AppDatabase().UpdateConversation(fresh);
    if (auto it = m_open.find(fresh.id); it != m_open.end()) it->second->ConversationUpdated(fresh);
    RefreshList(fresh.id);
}

void MainFrame::OnDelete(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    int64_t id = c->id;
    if (AppSettings().general.confirmDeletes &&
        !AskYesNo(this, "Delete " + U(DisplayTitle(*c)) + " and all its messages?", "Delete conversation")) {
        return;
    }
    if (auto it = m_open.find(id); it != m_open.end()) {
        it->second->CloseSilently();
        m_open.erase(it);
    }
    long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    AppDatabase().DeleteConversation(id);
    NotifyEvent(EventId::MessageDeleted, EventContext(), nullptr);
    int64_t next = 0;
    if (row + 1 < static_cast<long>(m_shown.size())) next = m_shown[row + 1].id;
    else if (row > 0) next = m_shown[row - 1].id;
    RefreshList(next);
}

void MainFrame::OnPin(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    Conversation fresh;
    if (!AppDatabase().GetConversation(c->id, fresh)) return;
    fresh.pinned = !fresh.pinned;
    AppDatabase().UpdateConversation(fresh);
    RefreshList(fresh.id);
    SetStatusText(fresh.pinned ? "Pinned" : "Unpinned", 0);
}

void MainFrame::OnDuplicate(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    Conversation copy = *c;
    copy.id = 0;
    copy.createdAt = 0;
    copy.pinned = false;
    copy.title = DisplayTitle(*c) + " (copy)";
    if (!AppDatabase().CreateConversation(copy)) return;
    for (auto m : AppDatabase().LoadMessages(c->id)) {
        m.id = 0;
        m.conversationId = copy.id;
        AppDatabase().AddMessage(m);
    }
    RefreshList(copy.id);
}

void MainFrame::OnConversationSettings(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    if (auto it = m_open.find(c->id); it != m_open.end()) {
        it->second->EditSettings();
        return;
    }
    Conversation fresh;
    if (!AppDatabase().GetConversation(c->id, fresh)) return;
    if (EditConversationSettings(this, fresh)) {
        AppDatabase().UpdateConversation(fresh);
        RefreshList(fresh.id);
    }
}

void MainFrame::OnOpenFolder(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    if (c->folder.empty()) {
        SetStatusText("This conversation has no folder", 0);
        return;
    }
    OpenWithSystem(c->folder);
}

std::string ConversationToText(const Conversation& c, const std::vector<Message>& messages) {
    std::string out = (c.title.empty() ? std::string("Conversation") : c.title) + "\n";
    out += "Created " + FormatLocalTime(c.createdAt) + "\n\n";
    for (const auto& m : messages) {
        switch (m.role) {
            case Role::User: out += "You:\n" + m.content + "\n\n"; break;
            case Role::Assistant:
                out += (m.model.empty() ? std::string("Assistant") : m.model) + ":\n";
                if (!m.content.empty()) out += m.content + "\n";
                for (const auto& call : m.toolCalls) out += "[Tool call] " + call.name + " " + call.arguments + "\n";
                out += "\n";
                break;
            case Role::Tool: out += "[Tool result: " + m.toolName + "]\n" + m.content + "\n\n"; break;
            case Role::System: out += "[System]\n" + m.content + "\n\n"; break;
        }
    }
    return out;
}

void MainFrame::OnExport(wxCommandEvent&) {
    Conversation* c = Selected();
    if (!c) return;
    wxString name = U(DisplayTitle(*c));
    name.Replace("/", "-");
    name.Replace("\\", "-");
    name.Replace(":", "-");
    wxFileDialog dlg(this, "Export conversation", "", name + ".txt", "Text files (*.txt)|*.txt|Markdown (*.md)|*.md",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string text = ConversationToText(*c, AppDatabase().LoadMessages(c->id));
    if (!WriteFileText(S(dlg.GetPath()), text)) ShowError(this, "The file could not be written.");
    else SetStatusText("Exported", 0);
}

void MainFrame::OnSettings(wxCommandEvent&) {
    if (ShowSettingsDialog(this)) {
        for (auto& [id, frame] : m_open) frame->SettingsChanged();
        RefreshList();
    }
}

void MainFrame::OnAccount(wxCommandEvent&) {
    const ProviderConfig* p = AppSettings().DefaultProvider();
    if (Conversation* c = Selected()) {
        if (const ProviderConfig* cp = AppSettings().FindProvider(c->settings.providerId)) p = cp;
    }
    if (!p) {
        ShowError(this, "There is no provider yet.");
        return;
    }
    ShowAccountInfo(this, *p);
    UpdateStatus();
}

void MainFrame::OnAbout(wxCommandEvent&) {
    wxMessageBox(wxString(APP_NAME " " APP_VERSION "\n\nChat and code with AI models from many providers.\n\nData folder: ") +
                     U(GetDataDir()),
                 "About FastChat", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::OnContextMenu(wxContextMenuEvent& e) {
    Conversation* c = Selected();
    wxMenu menu;
    if (c) {
        menu.Append(ID_OPEN, "&Open");
        menu.Append(ID_RENAME, "&Rename...\tF2");
        menu.Append(ID_PIN, c->pinned ? "Un&pin" : "&Pin");
        menu.Append(ID_DUPLICATE, "D&uplicate");
        menu.Append(ID_CONV_SETTINGS, "&Settings...");
        if (!c->folder.empty()) menu.Append(ID_OPEN_FOLDER, "Open &folder");
        menu.Append(ID_EXPORT, "&Export...");
        menu.AppendSeparator();
        menu.Append(ID_DELETE, "&Delete\tDel");
        menu.AppendSeparator();
    }
    menu.Append(ID_NEW, "&New conversation...");
    wxPoint pos = e.GetPosition();
    if (pos == wxDefaultPosition) {
        long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        wxRect rect;
        if (row >= 0 && m_list->GetItemRect(row, rect)) pos = m_list->ClientToScreen(rect.GetPosition());
        else pos = m_list->ClientToScreen(wxPoint(0, 0));
    }
    PopupMenu(&menu, ScreenToClient(pos));
}

void MainFrame::OnClose(wxCloseEvent& e) {
    // Close the conversation windows first; each stops its response.
    std::vector<ConversationFrame*> frames;
    for (auto& [id, frame] : m_open) frames.push_back(frame);
    for (auto* f : frames) f->CloseSilently();
    m_open.clear();
    AppSettings().windowState["main"] = {{"w", GetSize().x}, {"h", GetSize().y}, {"maximized", IsMaximized()}};
    g_mainFrame = nullptr;
    e.Skip();
}
