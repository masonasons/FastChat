#include "ui/conversation_frame.h"
#include "ui/dialogs.h"
#include "ui/main_frame.h"
#include "ui/ui_util.h"
#include "core/database.h"
#include "core/platform.h"
#include "core/speech.h"
#include "core/util.h"

#include <wx/filedlg.h>
#include <wx/richmsgdlg.h>

namespace {

enum {
    ID_SEND = wxID_HIGHEST + 100,
    ID_ACTIONS,
    ID_STOP,
    ID_CHANGE_MODEL,
    ID_CHAT_SETTINGS,
    ID_GLOBAL_SETTINGS,
    ID_REGENERATE,
    ID_SPEAK_LAST,
    ID_STOP_SPEECH,
    ID_COPY_LAST,
    ID_RENAME,
    ID_INFO,
    ID_EXPORT,
    ID_CLEAR,
    ID_OPEN_FOLDER,
    ID_ACCOUNT,
    ID_CLOSE,
    ID_FOCUS_LIST,
    ID_FOCUS_INPUT,
    // message menu
    ID_MSG_VIEW,
    ID_MSG_COPY,
    ID_MSG_COPY_CODE,
    ID_MSG_SPEAK,
    ID_MSG_THINKING,
    ID_MSG_DETAILS,
    ID_MSG_EDIT,
    ID_MSG_REGENERATE,
    ID_MSG_DELETE,
    ID_MSG_DELETE_AFTER,
};

const size_t kMaxRowChars = 8000;
const int kStopHotkeyId = 0xB0F0;

// Tools that run a shell command: FastChat's, and Claude Code's.
bool IsCommandToolName(const std::string& name) {
    return name == "run_command" || name == "Bash" || name == "PowerShell";
}

}  // namespace

ConversationFrame::ConversationFrame(MainFrame* main, const Conversation& conversation)
    : wxFrame(nullptr, wxID_ANY, "", wxDefaultPosition, wxSize(900, 650)), m_main(main), m_conv(conversation),
      m_timer(this) {
    m_runner = std::make_shared<ChatRunner>([](std::function<void()> fn) { PostToUi(std::move(fn)); });
    m_runner->SetListener(this);

    m_panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(m_panel, wxID_ANY, "&Messages"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
    m_list = new wxListBox(m_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE | wxLB_NEEDED_SB);
    sizer->Add(m_list, 3, wxEXPAND | wxALL, 6);
    sizer->Add(new wxStaticText(m_panel, wxID_ANY, "&Write a message"), 0, wxLEFT | wxRIGHT, 6);
    m_input = new wxTextCtrl(m_panel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE);
    sizer->Add(m_input, 1, wxEXPAND | wxALL, 6);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_send = new wxButton(m_panel, ID_SEND, "&Send");
    m_actions = new wxButton(m_panel, ID_ACTIONS, "&Actions");
    buttons->Add(m_send, 0, wxRIGHT, 6);
    buttons->Add(m_actions, 0);
    sizer->Add(buttons, 0, wxALL, 6);
    m_panel->SetSizer(sizer);

    CreateStatusBar(5);
    int widths[] = {-3, -3, -3, -4, -2};
    GetStatusBar()->SetStatusWidths(5, widths);

    wxAcceleratorEntry accel[] = {
        {wxACCEL_CTRL, 'M', ID_CHANGE_MODEL},
        {wxACCEL_CTRL | wxACCEL_SHIFT, ',', ID_CHAT_SETTINGS},
        {wxACCEL_CTRL, ',', ID_GLOBAL_SETTINGS},
        {wxACCEL_CTRL, 'R', ID_REGENERATE},
        {wxACCEL_CTRL | wxACCEL_SHIFT, 'R', ID_SPEAK_LAST},
        {wxACCEL_CTRL | wxACCEL_SHIFT, 'S', ID_STOP_SPEECH},
        {wxACCEL_CTRL | wxACCEL_SHIFT, 'C', ID_COPY_LAST},
        {wxACCEL_NORMAL, WXK_F2, ID_RENAME},
        {wxACCEL_CTRL, 'I', ID_INFO},
        {wxACCEL_CTRL, 'E', ID_EXPORT},
        {wxACCEL_CTRL | wxACCEL_SHIFT, 'A', ID_ACCOUNT},
        {wxACCEL_CTRL, 'W', ID_CLOSE},
        {wxACCEL_NORMAL, WXK_F6, ID_FOCUS_LIST},
    };
    SetAcceleratorTable(wxAcceleratorTable(static_cast<int>(sizeof(accel) / sizeof(accel[0])), accel));

    Bind(wxEVT_MENU, &ConversationFrame::OnCommand, this);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Send(); }, ID_SEND);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowActions(); }, ID_ACTIONS);
    Bind(wxEVT_CHAR_HOOK, &ConversationFrame::OnCharHook, this);
    Bind(wxEVT_CLOSE_WINDOW, &ConversationFrame::OnClose, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { SetPendingRow(false); });
    Bind(wxEVT_HOTKEY, [this](wxKeyEvent&) { Stop(); }, kStopHotkeyId);

    m_input->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        int key = e.GetKeyCode();
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            bool enterSends = AppSettings().general.enterSends;
            if (e.ControlDown() || (enterSends && !e.ShiftDown() && !e.AltDown())) {
                Send();
                return;
            }
        }
        e.Skip();
    });
    m_list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        int key = e.GetKeyCode();
        int index = SelectedMessage();
        if (key == WXK_DELETE && index >= 0) {
            DeleteMessage(index, e.ShiftDown());
            return;
        }
        if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && index >= 0) {
            ViewMessage(index);
            return;
        }
        if (key == 'C' && e.ControlDown() && !e.ShiftDown() && index >= 0) {
            CopyMessage(index);
            return;
        }
        e.Skip();
    });
    m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
        if (SelectedMessage() >= 0) ViewMessage(SelectedMessage());
    });
    m_list->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) { ShowMessageMenu(); });

    m_messages = AppDatabase().LoadMessages(m_conv.id);
    FillList(static_cast<int>(m_messages.size()) - 1);
    UpdateTitle();
    m_statusText = "Ready";
    UpdateStatusBar();

    if (auto it = AppSettings().windowState.find("conversation"); it != AppSettings().windowState.end()) {
        const json& w = *it;
        if (w.contains("w") && w.contains("h")) SetSize(wxSize(w.value("w", 900), w.value("h", 650)));
    }
    EventContext ctx = MakeContext();
    NotifyEvent(EventId::ConversationOpened, ctx, &m_conv.settings);
    m_input->SetFocus();
}

ConversationFrame::~ConversationFrame() {
    m_runner->SetListener(nullptr);
    m_runner->Stop();
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

std::string ConversationFrame::AssistantName(const Message& m) const {
    std::string name = ModelDisplayName(m.providerId, m.model);
    return name.empty() ? std::string("Assistant") : name;
}

// The assistant turn that asked for this tool result.
const Message* ConversationFrame::CallerOf(int index) const {
    const std::string& id = m_messages[index].toolCallId;
    for (int i = index - 1; i >= 0; i--) {
        if (m_messages[i].role != Role::Assistant) continue;
        for (const auto& c : m_messages[i].toolCalls) {
            if (c.id == id) return &m_messages[i];
        }
        if (m_messages[i].toolCalls.empty()) break;
    }
    return nullptr;
}

std::string ConversationFrame::CommandOf(int index) const {
    if (const Message* caller = CallerOf(index)) {
        for (const auto& c : caller->toolCalls) {
            if (c.id == m_messages[index].toolCallId) {
                json args = ArgumentsToObject(c.arguments);
                if (args.contains("command") && args["command"].is_string()) return args["command"].get<std::string>();
            }
        }
    }
    return std::string();
}

// A turn that only calls tools has no row of its own: its tool results show
// what it did.
bool ConversationFrame::ShowsRow(const Message& m) const {
    return !(m.role == Role::Assistant && Trim(m.content).empty() && !m.toolCalls.empty() && m.stopReason != "cancelled");
}

wxString ConversationFrame::RowText(int index) const {
    const Message& m = m_messages[index];
    std::string text;
    switch (m.role) {
        case Role::User:
            text = "You: " + OneLine(m.content);
            break;
        case Role::System:
            text = "System: " + OneLine(m.content);
            break;
        case Role::Assistant:
            text = AssistantName(m) + ": ";
            if (!m.content.empty()) text += OneLine(m.content);
            else if (!m.reasoning.empty()) text += "(thinking only) " + OneLine(m.reasoning);
            if (m.stopReason == "cancelled") text += " (stopped)";
            break;
        case Role::Tool: {
            std::string command = IsCommandToolName(m.toolName) ? CommandOf(index) : std::string();
            if (!command.empty()) {
                // "Command: cmake --build build, exit code 0", from the output's last line.
                text = "Command: " + OneLine(command);
                size_t pos = m.content.rfind("Exit code: ");
                if (pos != std::string::npos) text += ", exit code " + Trim(m.content.substr(pos + 11));
                if (m.content.find("(The command was stopped after") != std::string::npos) text += ", timed out";
                else if (m.content.find("(The command was stopped by the user.)") != std::string::npos) text += ", stopped";
                else if (pos == std::string::npos && m.toolError) text += ": " + OneLine(m.content, 200);
            } else if (IsComputerTool(m.toolName)) {
                // Computer use: the action itself reads as the row ("Pressed Save button").
                text = (m.reasoning.empty() ? m.toolName : m.reasoning) + (m.toolError ? " (failed)" : "");
                if (!m.images.empty()) text += ", with a screenshot";
            } else {
                text = "Tool: " + (m.reasoning.empty() ? m.toolName : m.reasoning) + (m.toolError ? " (failed)" : "");
            }
            break;
        }
    }
    if (text.size() > kMaxRowChars) text = TruncateUtf8(text, kMaxRowChars) + "...";
    return U(text);
}

int ConversationFrame::RowForMessage(int index) const {
    int row = -1;
    for (int r = 0; r < static_cast<int>(m_rows.size()); r++) {
        if (m_rows[r] <= index) row = r;
    }
    return row;
}

void ConversationFrame::AppendRow(int index) {
    if (!ShowsRow(m_messages[index])) return;
    int pending = static_cast<int>(m_rows.size());
    if (m_pendingRow && pending < static_cast<int>(m_list->GetCount())) {
        m_list->Insert(RowText(index), pending);
    } else {
        m_list->Append(RowText(index));
    }
    m_rows.push_back(index);
}

void ConversationFrame::FillList(int select) {
    m_list->Freeze();
    m_list->Clear();
    m_rows.clear();
    wxArrayString rows;
    for (int i = 0; i < static_cast<int>(m_messages.size()); i++) {
        if (!ShowsRow(m_messages[i])) continue;
        rows.Add(RowText(i));
        m_rows.push_back(i);
    }
    if (!rows.empty()) m_list->Append(rows);
    m_pendingRow = false;
    if (m_running) SetPendingRow(true);
    int row = RowForMessage(select);
    if (row >= 0) m_list->SetSelection(row);
    m_list->Thaw();
}

int ConversationFrame::SelectedMessage() const {
    int row = m_list->GetSelection();
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return -1;
    return m_rows[row];
}

void ConversationFrame::SetPendingRow(bool force) {
    if (!m_running) return;
    std::string name = m_streamModel.empty() ? std::string("Assistant") : ModelDisplayName(m_conv.settings.providerId, m_streamModel);
    std::string text;
    if (!m_runningTool.empty()) text = m_runningTool;
    else if (!m_streamText.empty()) text = name + ": " + OneLine(m_streamText, kMaxRowChars);
    else if (!m_streamReasoning.empty()) text = name + " is thinking: " + OneLine(m_streamReasoning, 400);
    else if (StartsWith(m_statusText, "Waiting")) text = name + ": waiting for a response";
    else text = name + ": " + m_statusText;
    wxString row = U(text);
    int index = static_cast<int>(m_rows.size());
    if (!m_pendingRow) {
        m_list->Append(row);
        m_pendingRow = true;
        return;
    }
    // Leave the row alone while it is selected, so a screen reader is not made
    // to read it again and again; it is brought up to date when the reply ends.
    if (!force && m_list->GetSelection() == index && m_list->HasFocus()) return;
    if (index < static_cast<int>(m_list->GetCount()) && m_list->GetString(index) != row) m_list->SetString(index, row);
}

void ConversationFrame::RemovePendingRow() {
    if (!m_pendingRow) return;
    int index = static_cast<int>(m_rows.size());
    bool wasSelected = m_list->GetSelection() == index;
    if (index < static_cast<int>(m_list->GetCount())) m_list->Delete(index);
    m_pendingRow = false;
    if (wasSelected && !m_rows.empty()) m_list->SetSelection(static_cast<int>(m_rows.size()) - 1);
}

void ConversationFrame::UpdateTitle() {
    std::string title = m_conv.title;
    if (title.empty()) {
        title = m_conv.type == ConversationType::Code       ? "Untitled code conversation"
                : m_conv.type == ConversationType::Computer ? "Untitled computer use"
                                                            : "Untitled chat";
    }
    SetTitle(U(title) + " - FastChat");
}

void ConversationFrame::SetStatus(const wxString& text) {
    m_statusText = S(text);
    SetStatusText(text, 0);
}

void ConversationFrame::UpdateStatusBar() {
    SetStatusText(U(m_statusText), 0);
    ProviderConfig provider;
    std::string model, error;
    if (!ResolveModel(provider, model, error)) {
        SetStatusText(U(error), 1);
        SetStatusText("", 2);
    } else {
        SetStatusText(U(ModelDisplayName(provider.id, model)) + " on " + U(provider.name), 1);
        ModelInfo info;
        AppProviderCache().FindModel(provider.id, model, info);
        // The context: what the last response read and wrote, and anything since.
        int64_t used = 0;
        bool exact = false;
        int last = -1;
        for (int i = static_cast<int>(m_messages.size()) - 1; i >= 0; i--) {
            if (m_messages[i].role == Role::Assistant && m_messages[i].usage.HasTokens()) {
                last = i;
                break;
            }
        }
        if (last >= 0) {
            used = m_messages[last].usage.inputTokens + m_messages[last].usage.outputTokens;
            exact = true;
            for (size_t i = last + 1; i < m_messages.size(); i++) used += EstimateTokens(m_messages[i].content);
        } else {
            used = EstimateTokens(SystemPrompt());
            for (const auto& m : m_messages) used += EstimateTokens(m.content) + EstimateTokens(m.reasoning) / 2;
        }
        wxString ctx = "Context " + wxString(exact ? "" : "about ") + U(FormatTokensShort(used));
        if (info.contextLength > 0) {
            ctx += " of " + U(FormatTokensShort(info.contextLength)) + " (" +
                   U(FormatPercent(static_cast<double>(used) / info.contextLength)) + ")";
        }
        SetStatusText(ctx, 2);

        // Credits, where the provider reports them
        AccountInfo account;
        bool haveAccount = AppProviderCache().GetAccount(provider.id, account);
        if (haveAccount && account.creditsRemaining >= 0) {
            SetStatusText("Credits " + U(FormatMoney(account.creditsRemaining)), 4);
        } else if (haveAccount && !account.summary.empty()) {
            SetStatusText(U(account.summary), 4);
        } else {
            SetStatusText("", 4);
        }
    }

    // Last response and totals
    double total = 0;
    bool anyCost = false;
    int64_t totalIn = 0, totalOut = 0;
    const Message* lastReply = nullptr;
    for (const auto& m : m_messages) {
        if (m.role != Role::Assistant) continue;
        totalIn += m.usage.inputTokens;
        totalOut += m.usage.outputTokens;
        if (m.usage.cost >= 0) {
            total += m.usage.cost;
            anyCost = true;
        }
        if (m.usage.HasTokens()) lastReply = &m;
    }
    wxString usage;
    if (lastReply) {
        usage = "Last " + U(FormatTokensShort(lastReply->usage.inputTokens)) + " in, " +
                U(FormatTokensShort(lastReply->usage.outputTokens)) + " out";
        if (lastReply->usage.cost >= 0) usage += ", " + U(FormatMoney(lastReply->usage.cost));
        if (anyCost) usage += "; total " + U(FormatMoney(total));
    }
    SetStatusText(usage, 3);
}

bool ConversationFrame::WindowActive() const {
    return const_cast<ConversationFrame*>(this)->IsActive();
}

EventContext ConversationFrame::MakeContext() const {
    EventContext ctx;
    ctx.windowActive = WindowActive();
    std::string title = m_conv.title.empty() ? "untitled conversation" : m_conv.title;
    ctx.Set("conversation", title);
    ProviderConfig provider;
    std::string model, error;
    if (ResolveModel(provider, model, error)) {
        ctx.Set("model", ModelDisplayName(provider.id, model));
        ctx.Set("provider", provider.name);
    }
    return ctx;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool ConversationFrame::ResolveModel(ProviderConfig& provider, std::string& model, std::string& error) const {
    const Settings& s = AppSettings();
    const ProviderConfig* p = nullptr;
    if (!m_conv.settings.providerId.empty()) {
        p = s.FindProvider(m_conv.settings.providerId);
        if (!p) {
            error = "Its provider was removed; choose a model (Ctrl+M)";
            return false;
        }
        model = m_conv.settings.model;
    } else {
        p = s.DefaultProvider();
        if (!p) {
            error = "No provider: add one in Settings (Ctrl+Comma)";
            return false;
        }
        model = s.general.defaultModel;
    }
    if (model.empty()) model = CreateProvider(*p)->DefaultModel();
    if (model.empty()) {
        error = "No model chosen: press Ctrl+M";
        return false;
    }
    provider = *p;
    return true;
}

std::string ConversationFrame::SystemPrompt() const {
    const GeneralSettings& g = AppSettings().general;
    if (m_conv.type == ConversationType::Code) {
        std::string prompt = CodeSystemPrompt(m_conv.folder);
        std::string extra = m_conv.settings.systemPrompt ? *m_conv.settings.systemPrompt : g.codeSystemPrompt;
        if (!Trim(extra).empty()) prompt += "\n\n" + extra;
        return prompt;
    }
    return m_conv.settings.systemPrompt ? *m_conv.settings.systemPrompt : g.chatSystemPrompt;
}

void ConversationFrame::Send() {
    std::string text = Trim(S(m_input->GetValue()));
    if (text.empty()) return;
    if (m_running) {
        SetStatus("Still responding; press Escape to stop it first");
        wxBell();
        return;
    }
    ProviderConfig provider;
    std::string model, error;
    if (!ResolveModel(provider, model, error)) {
        ShowError(this, U(error));
        return;
    }
    Message m;
    m.role = Role::User;
    m.conversationId = m_conv.id;
    m.content = S(m_input->GetValue());
    if (!AppDatabase().AddMessage(m)) {
        ShowError(this, "The message could not be saved: " + U(AppDatabase().LastError()));
        return;
    }
    m_messages.push_back(m);
    AppendRow(static_cast<int>(m_messages.size()) - 1);
    m_input->Clear();
    NotifyEvent(EventId::MessageSent, MakeContext(), &m_conv.settings);
    StartTurn();
    m_main->ConversationChanged(m_conv.id);
}

bool ConversationFrame::StartTurn() {
    if (m_running) return false;
    ProviderConfig provider;
    std::string model, error;
    if (!ResolveModel(provider, model, error)) {
        ShowError(this, U(error));
        return false;
    }
    const GeneralSettings& g = AppSettings().general;
    ChatTurn turn;
    turn.provider = provider;
    turn.request.model = model;
    turn.request.systemPrompt = SystemPrompt();
    if (const ProviderKindInfo* kind = FindProviderKind(provider.kind);
        kind && (kind->style == ApiStyle::ClaudeCode || kind->style == ApiStyle::Codex)) {
        // Coding agents have their own instructions; add only the user's.
        const GeneralSettings& gs = AppSettings().general;
        if (m_conv.settings.systemPrompt) turn.request.systemPrompt = *m_conv.settings.systemPrompt;
        else turn.request.systemPrompt = m_conv.type == ConversationType::Code ? gs.codeSystemPrompt : gs.chatSystemPrompt;
    }
    turn.request.messages = m_messages;
    turn.request.temperature = m_conv.settings.temperature;
    turn.request.maxTokens = m_conv.settings.maxTokens.value_or(g.maxTokens);
    turn.request.reasoningEffort = m_conv.settings.reasoningEffort.empty() ? g.reasoningEffort : m_conv.settings.reasoningEffort;
    turn.tools = m_conv.type == ConversationType::Code;
    turn.maxSteps = g.maxToolSteps;
    turn.toolContext.root = m_conv.folder;
    turn.toolContext.commandApproval = m_conv.settings.commandApproval.empty() ? g.commandApproval : m_conv.settings.commandApproval;
    if (m_conv.type == ConversationType::Computer) {
        const ProviderKindInfo* kind = FindProviderKind(provider.kind);
        if (kind && (kind->style == ApiStyle::ClaudeCode || kind->style == ApiStyle::Codex)) {
            ShowError(this, "Computer use needs a model from an API provider (Anthropic, OpenAI, Gemini, OpenRouter...), "
                            "not a coding agent. Choose another model with Ctrl+M.");
            return false;
        }
        turn.computer = true;
        // Screenshots go to models that can see them; the others work from the accessibility tree.
        ModelInfo info;
        AppProviderCache().FindModel(provider.id, model, info);
        turn.vision = info.vision || provider.kind == "openai" || provider.kind == "anthropic" ||
                      provider.kind == "gemini" || provider.kind == "xai";
        // The engine adds the computer use instructions; this is only the user's own.
        turn.request.systemPrompt = m_conv.settings.systemPrompt.value_or(std::string());
        turn.toolContext.commandApproval =
            m_conv.settings.commandApproval.empty() ? g.computerApproval : m_conv.settings.commandApproval;
    }
    if (m_autoApprove) turn.toolContext.commandApproval = "auto";
    turn.toolContext.commandTimeoutMs = static_cast<unsigned>(g.commandTimeoutSec) * 1000;

    m_running = true;
    m_gotText = m_gotReasoning = false;
    m_streamText.clear();
    m_streamReasoning.clear();
    m_speechBuffer.clear();
    m_reasoningSpeechBuffer.clear();
    m_spokeFirstSentence = false;
    m_streamModel = model;
    m_turnStartMs = NowMillis();
    SetStatus("Waiting for " + U(ModelDisplayName(provider.id, model)));
    SetPendingRow(true);
    m_timer.Start(700);
    m_send->SetLabel("&Send");
    bool computer = turn.computer;
    if (!m_runner->Start(std::move(turn))) {
        m_running = false;
        m_timer.Stop();
        RemovePendingRow();
        return false;
    }
    if (computer) {
        // Focus will be in other windows, out of Escape's reach: a key that works anywhere.
        m_stopHotkey = RegisterHotKey(kStopHotkeyId, wxMOD_CONTROL | wxMOD_ALT, WXK_ESCAPE);
        SetStatus(m_stopHotkey ? wxString("Working on your computer. Ctrl+Alt+Escape stops it from any window.")
                               : wxString("Working on your computer. Escape here stops it."));
    }
    return true;
}

void ConversationFrame::ReleaseStopHotkey() {
    if (m_stopHotkey) {
        UnregisterHotKey(kStopHotkeyId);
        m_stopHotkey = false;
    }
}

// ---------------------------------------------------------------------------
// Streaming speech
// ---------------------------------------------------------------------------

void ConversationFrame::StreamSpeech(const std::string& text, bool reasoning) {
    EffectiveSpeech s = ResolveSpeech(&m_conv.settings);
    if (!s.enabled || s.speakResponses != SpeakResponses::WhileStreaming) return;
    if (reasoning && !s.speakReasoning) return;
    std::string& buffer = reasoning ? m_reasoningSpeechBuffer : m_speechBuffer;
    buffer += text;
    // Up to the last sentence end (". ", "! ", "? ", ": " or a line break).
    size_t cut = std::string::npos;
    for (size_t i = 0; i + 1 < buffer.size(); i++) {
        char c = buffer[i];
        char next = buffer[i + 1];
        if (c == '\n' || ((c == '.' || c == '!' || c == '?' || c == ':') && (next == ' ' || next == '\n'))) cut = i + 1;
    }
    if (cut == std::string::npos) return;
    std::string sentence = buffer.substr(0, cut);
    buffer.erase(0, cut);
    // Inside an unfinished code block, wait for its end so it can be handled whole.
    size_t fences = 0;
    for (size_t p = sentence.find("```"); p != std::string::npos; p = sentence.find("```", p + 3)) fences++;
    if (fences % 2 == 1) {
        buffer = sentence + buffer;
        return;
    }
    if (!WindowActive() && !s.speakInBackground) return;
    std::string spoken = PrepareForSpeech(sentence);
    if (Trim(spoken).empty()) return;
    bool interrupt = !m_spokeFirstSentence && s.interrupt;
    m_spokeFirstSentence = true;
    AppSpeech().Speak(spoken, interrupt, s.voice, s.rate);
}

void ConversationFrame::FlushStreamSpeech(bool reasoning) {
    std::string& buffer = reasoning ? m_reasoningSpeechBuffer : m_speechBuffer;
    if (Trim(buffer).empty()) {
        buffer.clear();
        return;
    }
    // Two line breaks: the scan for sentence ends looks one character ahead.
    std::string rest = buffer + "\n\n";
    buffer.clear();
    StreamSpeech(rest, reasoning);
    buffer.clear();
}

// ---------------------------------------------------------------------------
// ChatListener
// ---------------------------------------------------------------------------

void ConversationFrame::OnReasoningDelta(const std::string& text) {
    if (!m_gotReasoning) {
        m_gotReasoning = true;
        SetStatus("Thinking");
        NotifyEvent(EventId::ReasoningStarted, MakeContext(), &m_conv.settings);
    }
    m_streamReasoning += text;
    StreamSpeech(text, true);
}

void ConversationFrame::OnTextDelta(const std::string& text) {
    if (!m_gotText) {
        m_gotText = true;
        FlushStreamSpeech(true);
        SetStatus("Responding");
        NotifyEvent(EventId::ResponseStarted, MakeContext(), &m_conv.settings);
        SetPendingRow(true);
    }
    m_streamText += text;
    StreamSpeech(text, false);
}

void ConversationFrame::OnToolCallStarted(const std::string& toolName) {
    SetStatus("Preparing " + U(toolName));
}

void ConversationFrame::OnMessage(Message& message) {
    message.conversationId = m_conv.id;
    AppDatabase().AddMessage(message);
    bool followLast = m_list->GetSelection() >= static_cast<int>(m_rows.size()) - 1;
    if (message.role == Role::Tool) m_runningTool.clear();
    RemovePendingRow();
    m_messages.push_back(message);
    size_t rowsBefore = m_rows.size();
    AppendRow(static_cast<int>(m_messages.size()) - 1);
    if (followLast && m_list->HasFocus() && m_rows.size() > rowsBefore) {
        m_list->SetSelection(static_cast<int>(m_rows.size()) - 1);
    }

    if (message.role == Role::Assistant) {
        EffectiveSpeech s = ResolveSpeech(&m_conv.settings);
        if (s.speakResponses == SpeakResponses::WhileStreaming) {
            FlushStreamSpeech(true);
            FlushStreamSpeech(false);
        } else if (s.speakResponses == SpeakResponses::WhenComplete) {
            bool interrupt = s.interrupt;
            if (s.speakReasoning && !message.reasoning.empty()) {
                SpeakResponseText(message.reasoning, &m_conv.settings, WindowActive(), interrupt);
                interrupt = false;
            }
            if (!message.content.empty()) SpeakResponseText(message.content, &m_conv.settings, WindowActive(), interrupt);
        }
        // The next step of a tool loop streams afresh.
        m_streamText.clear();
        m_streamReasoning.clear();
        m_gotText = m_gotReasoning = false;
        m_spokeFirstSentence = false;
    } else if (message.role == Role::Tool) {
        EventContext ctx = MakeContext();
        ctx.Set("tool", message.toolName).Set("summary", message.reasoning);
        NotifyEvent(EventId::ToolResult, ctx, &m_conv.settings);
    }
    if (m_running) SetPendingRow(true);
    UpdateStatusBar();
}

void ConversationFrame::OnToolRunning(const ToolCall& call, const std::string& description) {
    SetStatus(U(description));
    if (IsCommandToolName(call.name)) {
        json args = ArgumentsToObject(call.arguments);
        std::string command = args.contains("command") && args["command"].is_string() ? args["command"].get<std::string>() : description;
        m_runningTool = "Running command: " + OneLine(command);
    } else {
        m_runningTool = "Running: " + description;
    }
    EventContext ctx = MakeContext();
    ctx.Set("tool", description).Set("summary", description);
    NotifyEvent(EventId::ToolCall, ctx, &m_conv.settings);
    SetPendingRow(true);
    (void)call;
}

void ConversationFrame::OnApproveCommand(const std::string& what, bool isCommand, std::function<void(bool)> answer) {
    if (m_autoApprove) {
        answer(true);
        return;
    }
    EventContext ctx = MakeContext();
    ctx.Set("summary", what).Set("tool", what);
    NotifyEvent(EventId::ApprovalNeeded, ctx, &m_conv.settings);
    if (IsIconized()) Iconize(false);
    Raise();
    // Commands, and a coding agent's other actions (writing or editing files...).
    wxString question = isCommand ? "Run this command in " + U(m_conv.folder) + "?\n\n" + U(what)
                                  : "Allow this?\n\n" + U(what);
    wxRichMessageDialog dlg(this, question, isCommand ? "Run command" : "Allow action", wxYES_NO | wxICON_QUESTION);
    if (isCommand) dlg.SetYesNoLabels("&Run", "&Don't run");
    else dlg.SetYesNoLabels("&Allow", "&Don't allow");
    dlg.ShowCheckBox("Allow everything without asking for the rest of this conversation");
    bool yes = dlg.ShowModal() == wxID_YES;
    if (yes && dlg.IsCheckBoxChecked()) m_autoApprove = true;
    if (isCommand) SetStatus(yes ? "Running " + U(OneLine(what, 80)) : wxString("Command declined"));
    else SetStatus(yes ? "Allowed" : "Declined");
    answer(yes);
}

void ConversationFrame::OnFinished(Outcome outcome, const std::string& error) {
    m_running = false;
    ReleaseStopHotkey();
    m_runningTool.clear();
    m_timer.Stop();
    RemovePendingRow();
    int64_t ms = NowMillis() - m_turnStartMs;
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f seconds", ms / 1000.0);
    EventContext ctx = MakeContext();
    ctx.Set("duration", seconds);
    // The last reply's numbers
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if (it->role == Role::Assistant) {
            ctx.Set("tokens", FormatThousands(it->usage.outputTokens));
            if (it->usage.cost >= 0) ctx.Set("cost", FormatMoney(it->usage.cost));
            break;
        }
    }
    switch (outcome) {
        case Outcome::Done:
            if (!error.empty()) {
                SetStatus(U(error));
                ctx.Set("error", error);
                NotifyEvent(EventId::ResponseError, ctx, &m_conv.settings);
            } else {
                SetStatus(wxString("Done in ") + seconds);
                NotifyEvent(EventId::ResponseComplete, ctx, &m_conv.settings);
            }
            break;
        case Outcome::Stopped:
            SetStatus("Stopped");
            NotifyEvent(EventId::GenerationStopped, ctx, &m_conv.settings);
            break;
        case Outcome::Failed:
            SetStatus("Error: " + U(error));
            ctx.Set("error", error);
            NotifyEvent(EventId::ResponseError, ctx, &m_conv.settings);
            break;
    }
    if (AppSettings().general.focusResponse && outcome == Outcome::Done && !m_messages.empty()) {
        if (!m_rows.empty()) m_list->SetSelection(static_cast<int>(m_rows.size()) - 1);
        m_list->SetFocus();
    }
    UpdateStatusBar();
    m_main->ConversationChanged(m_conv.id);
    if (outcome == Outcome::Done) {
        MaybeGenerateTitle();
        RefreshCredits();
    }
}

void ConversationFrame::MaybeGenerateTitle() {
    if (!m_conv.title.empty() || !AppSettings().general.autoTitle) return;
    std::string userText, assistantText;
    for (const auto& m : m_messages) {
        if (m.role == Role::User && userText.empty()) userText = m.content;
        if (m.role == Role::Assistant && !m.content.empty() && assistantText.empty()) assistantText = m.content;
    }
    if (userText.empty()) return;
    ProviderConfig provider;
    std::string model, error;
    const GeneralSettings& g = AppSettings().general;
    if (!g.titleProviderId.empty() && AppSettings().FindProvider(g.titleProviderId) && !g.titleModel.empty()) {
        provider = *AppSettings().FindProvider(g.titleProviderId);
        model = g.titleModel;
    } else if (!ResolveModel(provider, model, error)) {
        return;
    }
    if (m_conv.type == ConversationType::Code && !m_conv.folder.empty()) {
        userText = "(Working in " + m_conv.folder + ") " + userText;
    }
    struct Result {
        std::string title, error;
    };
    int64_t id = m_conv.id;
    RunInBackground<Result>(
        this,
        [provider, model, userText, assistantText]() {
            Result r;
            r.title = GenerateTitle(provider, model, userText, assistantText, r.error);
            return r;
        },
        [this, id](Result r) {
            if (r.title.empty() || !m_conv.title.empty() || id != m_conv.id) return;
            Conversation fresh;
            if (AppDatabase().GetConversation(id, fresh)) {
                fresh.title = r.title;
                AppDatabase().UpdateConversation(fresh);
            }
            m_conv.title = r.title;
            UpdateTitle();
            m_main->ConversationChanged(id);
            NotifyEvent(EventId::TitleGenerated, MakeContext(), &m_conv.settings);
        });
}

void ConversationFrame::RefreshCredits() {
    ProviderConfig provider;
    std::string model, error;
    if (!ResolveModel(provider, model, error)) return;
    if (provider.kind != "openrouter" && provider.kind != "deepseek") return;
    if (NowSeconds() - m_lastCreditsCheck < 30) return;
    m_lastCreditsCheck = NowSeconds();
    RunInBackground<AccountInfo>(
        this,
        [provider]() {
            auto p = CreateProvider(provider);
            AccountInfo info = p->GetAccountInfo(nullptr);
            AppProviderCache().SetAccount(provider.id, info);
            return info;
        },
        [this](AccountInfo) {
            UpdateStatusBar();
            if (MainFrame* main = GetMainFrame()) main->UpdateStatus();
        });
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void ConversationFrame::OnCommand(wxCommandEvent& e) {
    int index = SelectedMessage();
    switch (e.GetId()) {
        case ID_STOP: Stop(); break;
        case ID_CHANGE_MODEL: ChangeModel(); break;
        case ID_CHAT_SETTINGS: EditSettings(); break;
        case ID_GLOBAL_SETTINGS:
            if (ShowSettingsDialog(this)) SettingsChanged();
            break;
        case ID_REGENERATE: Regenerate(); break;
        case ID_SPEAK_LAST: SpeakLastResponse(); break;
        case ID_STOP_SPEECH: AppSpeech().Stop(); break;
        case ID_COPY_LAST: CopyLastResponse(); break;
        case ID_RENAME: Rename(); break;
        case ID_INFO: ShowInfo(); break;
        case ID_EXPORT: Export(); break;
        case ID_CLEAR: ClearMessages(); break;
        case ID_OPEN_FOLDER:
            if (!m_conv.folder.empty()) OpenWithSystem(m_conv.folder);
            break;
        case ID_ACCOUNT: {
            ProviderConfig provider;
            std::string model, error;
            if (ResolveModel(provider, model, error)) {
                ShowAccountInfo(this, provider);
                UpdateStatusBar();
            }
            break;
        }
        case ID_CLOSE: Close(); break;
        case ID_FOCUS_LIST:
            if (m_list->HasFocus()) m_input->SetFocus();
            else m_list->SetFocus();
            break;
        case ID_MSG_VIEW: if (index >= 0) ViewMessage(index); break;
        case ID_MSG_COPY: if (index >= 0) CopyMessage(index); break;
        case ID_MSG_COPY_CODE: if (index >= 0) CopyCodeBlocks(index); break;
        case ID_MSG_SPEAK: if (index >= 0) SpeakMessage(index); break;
        case ID_MSG_THINKING: if (index >= 0) ViewThinking(index); break;
        case ID_MSG_DETAILS: if (index >= 0) MessageDetails(index); break;
        case ID_MSG_EDIT: if (index >= 0) EditMessage(index); break;
        case ID_MSG_REGENERATE: if (index >= 0) RegenerateFrom(index); break;
        case ID_MSG_DELETE: if (index >= 0) DeleteMessage(index, false); break;
        case ID_MSG_DELETE_AFTER: if (index >= 0) DeleteMessage(index, true); break;
        default: e.Skip(); break;
    }
}

void ConversationFrame::OnCharHook(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && !e.HasAnyModifiers()) {
        if (m_running) Stop();
        else Close();
        return;
    }
    e.Skip();
}

void ConversationFrame::ShowActions() {
    wxMenu menu;
    if (m_running) menu.Append(ID_STOP, "S&top responding\tEscape");
    menu.Append(ID_CHANGE_MODEL, "Change &model...\tCtrl+M");
    menu.Append(ID_CHAT_SETTINGS, "Conversation &settings...\tCtrl+Shift+,");
    menu.Append(ID_REGENERATE, "&Regenerate the last response\tCtrl+R");
    menu.AppendSeparator();
    menu.Append(ID_SPEAK_LAST, "S&peak the last response\tCtrl+Shift+R");
    menu.Append(ID_STOP_SPEECH, "Stop spea&king\tCtrl+Shift+S");
    menu.Append(ID_COPY_LAST, "&Copy the last response\tCtrl+Shift+C");
    menu.AppendSeparator();
    menu.Append(ID_RENAME, "Re&name...\tF2");
    menu.Append(ID_INFO, "Conversation &information\tCtrl+I");
    menu.Append(ID_ACCOUNT, "&Account and credits...\tCtrl+Shift+A");
    menu.Append(ID_EXPORT, "&Export...\tCtrl+E");
    if (!m_conv.folder.empty()) menu.Append(ID_OPEN_FOLDER, "Open &folder");
    menu.Append(ID_CLEAR, "C&lear all messages...");
    menu.AppendSeparator();
    menu.Append(ID_GLOBAL_SETTINGS, "FastChat setti&ngs...\tCtrl+,");
    menu.Append(ID_CLOSE, "Cl&ose\tCtrl+W");
    PopupMenu(&menu, m_actions->GetPosition() + wxPoint(0, m_actions->GetSize().y));
}

void ConversationFrame::ShowMessageMenu() {
    int index = SelectedMessage();
    if (index < 0) return;
    const Message& m = m_messages[index];
    wxMenu menu;
    menu.Append(ID_MSG_VIEW, "&View\tEnter");
    menu.Append(ID_MSG_COPY, "&Copy\tCtrl+C");
    if (!ExtractCodeBlocks(m.content).empty()) menu.Append(ID_MSG_COPY_CODE, "Copy c&ode blocks");
    menu.Append(ID_MSG_SPEAK, "&Speak");
    const Message* thinker = m.role == Role::Tool ? CallerOf(index) : &m;
    if (thinker && thinker->role == Role::Assistant && !thinker->reasoning.empty()) {
        menu.Append(ID_MSG_THINKING, "Show &thinking");
    }
    menu.Append(ID_MSG_DETAILS, "D&etails");
    if (m.role == Role::User) menu.Append(ID_MSG_EDIT, "&Edit and resend");
    if (m.role == Role::Assistant) menu.Append(ID_MSG_REGENERATE, "&Regenerate from here");
    menu.AppendSeparator();
    menu.Append(ID_MSG_DELETE, "&Delete\tDel");
    menu.Append(ID_MSG_DELETE_AFTER, "Delete this and &everything after\tShift+Del");
    wxRect rect;
    wxPoint pos(0, 0);
    if (m_list->GetSelection() >= 0) {
        // Below the selected row, roughly.
        int itemHeight = m_list->GetCharHeight() + 4;
        pos = wxPoint(10, std::min((m_list->GetSelection() - m_list->GetTopItem() + 1) * itemHeight, m_list->GetSize().y));
    }
    m_list->PopupMenu(&menu, pos);
}

void ConversationFrame::ViewMessage(int index) {
    const Message& m = m_messages[index];
    std::string text;
    wxString title;
    switch (m.role) {
        case Role::User:
            title = "Your message";
            text = m.content;
            break;
        case Role::System:
            title = "System message";
            text = m.content;
            break;
        case Role::Assistant:
            title = U(AssistantName(m));
            text = m.content;
            if (!m.toolCalls.empty()) {
                text += text.empty() ? "" : "\n\n";
                text += "Tool calls:\n";
                for (const auto& c : m.toolCalls) text += "- " + DescribeToolCall(c) + "\n";
            }
            if (text.empty() && !m.reasoning.empty()) text = "Thinking:\n" + m.reasoning;
            break;
        case Role::Tool: {
            std::string command = IsCommandToolName(m.toolName) ? CommandOf(index) : std::string();
            if (!command.empty()) {
                title = "Command";
                text = "Command: " + command + "\n\n" + m.content;
            } else {
                title = "Tool: " + U(m.toolName);
                text = m.reasoning + "\n\n" + m.content;
            }
            if (!m.images.empty()) text += "\n\n(A screenshot went to the model with this result.)";
            break;
        }
    }
    ShowTextDialog(this, title, U(text));
}

void ConversationFrame::ViewThinking(int index) {
    const Message* caller = m_messages[index].role == Role::Tool ? CallerOf(index) : &m_messages[index];
    if (!caller) return;
    const Message& m = *caller;
    if (m.reasoning.empty()) {
        SetStatus("This message has no thinking");
        return;
    }
    ShowTextDialog(this, "Thinking: " + U(AssistantName(m)), U(m.reasoning));
}

void ConversationFrame::MessageDetails(int index) {
    const Message& m = m_messages[index];
    std::string t;
    t += "Sent: " + FormatLocalTime(m.createdAt, true) + "\n";
    if (m.role == Role::Assistant) {
        const ProviderConfig* p = AppSettings().FindProvider(m.providerId);
        t += "Model: " + m.model + "\n";
        if (p) t += "Provider: " + p->name + "\n";
        if (m.durationMs) t += "Took: " + std::to_string(m.durationMs / 1000) + "." + std::to_string((m.durationMs / 100) % 10) + " seconds\n";
        if (m.usage.HasTokens()) {
            t += "Input tokens: " + FormatThousands(m.usage.inputTokens);
            if (m.usage.cachedInputTokens) t += " (" + FormatThousands(m.usage.cachedInputTokens) + " from cache)";
            if (m.usage.cacheWriteTokens) t += " (" + FormatThousands(m.usage.cacheWriteTokens) + " written to cache)";
            t += "\nOutput tokens: " + FormatThousands(m.usage.outputTokens);
            if (m.usage.reasoningTokens) t += " (" + FormatThousands(m.usage.reasoningTokens) + " thinking)";
            t += "\n";
            if (m.durationMs > 0 && m.usage.outputTokens > 0) {
                char speed[64];
                std::snprintf(speed, sizeof(speed), "%.1f", m.usage.outputTokens * 1000.0 / m.durationMs);
                t += std::string("Speed: ") + speed + " tokens a second\n";
            }
        }
        if (m.usage.cost >= 0) t += "Cost: " + FormatMoney(m.usage.cost) + "\n";
        if (!m.stopReason.empty()) t += "Stop reason: " + m.stopReason + "\n";
        if (!m.toolCalls.empty()) {
            t += "\nTool calls:\n";
            for (const auto& c : m.toolCalls) t += "- " + c.name + " " + c.arguments + "\n";
        }
    }
    if (m.role == Role::Tool) t += "Tool: " + m.toolName + (m.toolError ? " (failed)" : "") + "\n";
    t += "Characters: " + FormatThousands(static_cast<int64_t>(Utf8ToWide(m.content).size())) + "\n";
    ShowTextDialog(this, "Message details", U(t));
}

void ConversationFrame::CopyMessage(int index) {
    const Message& m = m_messages[index];
    std::string text = m.content.empty() && m.role == Role::Assistant ? m.reasoning : m.content;
    if (CopyToClipboard(text)) SetStatus("Copied");
}

void ConversationFrame::CopyCodeBlocks(int index) {
    auto blocks = ExtractCodeBlocks(m_messages[index].content);
    std::string all;
    for (const auto& b : blocks) all += (all.empty() ? "" : "\n") + b;
    if (!blocks.empty() && CopyToClipboard(all)) {
        SetStatus(wxString::Format("Copied %zu code block%s", blocks.size(), blocks.size() == 1 ? "" : "s"));
    }
}

void ConversationFrame::SpeakMessage(int index) {
    const Message& m = m_messages[index];
    SpeakOnRequest(m.role == Role::Tool ? m.reasoning + ". " + m.content : m.content, &m_conv.settings);
}

void ConversationFrame::DeleteMessage(int index, bool andAfter) {
    if (m_running) {
        SetStatus("Stop the response before deleting messages");
        wxBell();
        return;
    }
    // A tool call and its results go together: providers refuse one without the other.
    int first = index, last = index;
    if (m_messages[index].role == Role::Tool) {
        while (first > 0 && m_messages[first].role == Role::Tool) first--;
    }
    if (m_messages[first].role == Role::Assistant && !m_messages[first].toolCalls.empty()) {
        last = first;
        while (last + 1 < static_cast<int>(m_messages.size()) && m_messages[last + 1].role == Role::Tool) last++;
    }
    if (andAfter) last = static_cast<int>(m_messages.size()) - 1;
    int count = last - first + 1;
    if (AppSettings().general.confirmDeletes) {
        wxString question = count == 1 ? wxString("Delete this message?")
                                       : wxString::Format("Delete these %d messages?", count);
        if (!AskYesNo(this, question, "Delete")) return;
    }
    for (int i = first; i <= last; i++) AppDatabase().DeleteMessage(m_messages[i].id);
    m_messages.erase(m_messages.begin() + first, m_messages.begin() + last + 1);
    FillList(std::min(first, static_cast<int>(m_messages.size()) - 1));
    NotifyEvent(EventId::MessageDeleted, MakeContext(), &m_conv.settings);
    UpdateStatusBar();
    m_main->ConversationChanged(m_conv.id);
}

void ConversationFrame::EditMessage(int index) {
    if (m_running) return;
    const Message m = m_messages[index];
    if (index + 1 < static_cast<int>(m_messages.size()) && AppSettings().general.confirmDeletes &&
        !AskYesNo(this, "Editing this message removes it and everything after it. Continue?", "Edit and resend")) {
        return;
    }
    for (size_t i = index; i < m_messages.size(); i++) AppDatabase().DeleteMessage(m_messages[i].id);
    m_messages.erase(m_messages.begin() + index, m_messages.end());
    FillList(static_cast<int>(m_messages.size()) - 1);
    m_input->SetValue(U(m.content));
    m_input->SetInsertionPointEnd();
    m_input->SetFocus();
    UpdateStatusBar();
}

void ConversationFrame::RegenerateFrom(int index) {
    if (m_running) return;
    // Back to the message that asked for this response.
    while (index > 0 && m_messages[index].role != Role::Assistant) index--;
    int start = index;
    while (start > 0 && m_messages[start - 1].role != Role::User) start--;
    if (start <= 0) start = index;
    for (size_t i = start; i < m_messages.size(); i++) AppDatabase().DeleteMessage(m_messages[i].id);
    m_messages.erase(m_messages.begin() + start, m_messages.end());
    FillList(static_cast<int>(m_messages.size()) - 1);
    if (m_messages.empty() || m_messages.back().role != Role::User) {
        SetStatus("There is no message to respond to");
        return;
    }
    StartTurn();
}

void ConversationFrame::Regenerate() {
    for (int i = static_cast<int>(m_messages.size()) - 1; i >= 0; i--) {
        if (m_messages[i].role == Role::User) {
            if (i + 1 < static_cast<int>(m_messages.size())) {
                RegenerateFrom(i + 1);
            } else if (!m_running) {
                StartTurn();
            }
            return;
        }
    }
    SetStatus("There is nothing to regenerate");
}

void ConversationFrame::ChangeModel() {
    std::string p = m_conv.settings.providerId, m = m_conv.settings.model;
    if (!PickModel(this, p, m, true)) return;
    m_conv.settings.providerId = p;
    m_conv.settings.model = m;
    SaveConversation();
    UpdateStatusBar();
    ProviderConfig provider;
    std::string model, error;
    if (ResolveModel(provider, model, error)) SetStatus("Using " + U(ModelDisplayName(provider.id, model)));
    m_main->ConversationChanged(m_conv.id);
}

void ConversationFrame::EditSettings() {
    Raise();
    Conversation copy = m_conv;
    if (!EditConversationSettings(this, copy)) return;
    m_conv = copy;
    SaveConversation();
    UpdateTitle();
    UpdateStatusBar();
    m_main->ConversationChanged(m_conv.id);
}

void ConversationFrame::SaveConversation() {
    Conversation fresh;
    if (AppDatabase().GetConversation(m_conv.id, fresh)) {
        fresh.title = m_conv.title;
        fresh.folder = m_conv.folder;
        fresh.settings = m_conv.settings;
        AppDatabase().UpdateConversation(fresh);
    }
}

void ConversationFrame::Rename() {
    wxTextEntryDialog dlg(this, "Name:", "Rename conversation", U(m_conv.title));
    if (dlg.ShowModal() != wxID_OK) return;
    m_conv.title = Trim(S(dlg.GetValue()));
    SaveConversation();
    UpdateTitle();
    m_main->ConversationChanged(m_conv.id);
}

void ConversationFrame::ShowInfo() {
    std::string t = (m_conv.title.empty() ? std::string("Untitled") : m_conv.title) + "\n";
    t += std::string(ConversationTypeName(m_conv.type)) + " conversation, created " + FormatLocalTime(m_conv.createdAt) + "\n";
    if (!m_conv.folder.empty()) t += "Folder: " + m_conv.folder + "\n";
    ProviderConfig provider;
    std::string model, error;
    if (ResolveModel(provider, model, error)) {
        ModelInfo info;
        AppProviderCache().FindModel(provider.id, model, info);
        t += "\nModel: " + DescribeModel(info) + "\nModel ID: " + model + "\nProvider: " + provider.name + "\n";
        if (info.maxOutput) t += "Longest response: " + FormatThousands(info.maxOutput) + " tokens\n";
    } else {
        t += "\n" + error + "\n";
    }
    int users = 0, replies = 0, tools = 0;
    int64_t in = 0, out = 0, cached = 0, reasoning = 0;
    double cost = 0;
    bool anyCost = false;
    for (const auto& m : m_messages) {
        if (m.role == Role::User) users++;
        if (m.role == Role::Tool) tools++;
        if (m.role != Role::Assistant) continue;
        replies++;
        in += m.usage.inputTokens;
        out += m.usage.outputTokens;
        cached += m.usage.cachedInputTokens;
        reasoning += m.usage.reasoningTokens;
        if (m.usage.cost >= 0) {
            cost += m.usage.cost;
            anyCost = true;
        }
    }
    t += "\nMessages: " + std::to_string(users) + " from you, " + std::to_string(replies) + " responses";
    if (tools) t += ", " + std::to_string(tools) + " tool results";
    t += "\nTokens read: " + FormatThousands(in) + " (" + FormatThousands(cached) + " from cache)\n";
    t += "Tokens written: " + FormatThousands(out) + " (" + FormatThousands(reasoning) + " thinking)\n";
    if (anyCost) t += "Cost: " + FormatMoney(cost) + "\n";
    t += "\nStatus bar: " + m_statusText + "\n";
    for (int i = 1; i < 5; i++) {
        std::string field = S(GetStatusBar()->GetStatusText(i));
        if (!field.empty()) t += field + "\n";
    }
    ShowTextDialog(this, "Conversation information", U(t));
}

void ConversationFrame::Export() {
    wxString name = U(m_conv.title.empty() ? std::string("conversation") : m_conv.title);
    for (const char* bad : {"/", "\\", ":", "*", "?", "\"", "<", ">", "|"}) name.Replace(bad, "-");
    wxFileDialog dlg(this, "Export conversation", "", name + ".txt", "Text files (*.txt)|*.txt|Markdown (*.md)|*.md",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    if (WriteFileText(S(dlg.GetPath()), ConversationToText(m_conv, m_messages))) SetStatus("Exported");
    else ShowError(this, "The file could not be written.");
}

void ConversationFrame::ClearMessages() {
    if (m_running || m_messages.empty()) return;
    if (!AskYesNo(this, "Delete every message in this conversation?", "Clear")) return;
    AppDatabase().DeleteAllMessages(m_conv.id);
    m_messages.clear();
    FillList();
    UpdateStatusBar();
    m_main->ConversationChanged(m_conv.id);
}

void ConversationFrame::Stop() {
    if (!m_running) return;
    m_runner->Stop();
    SetStatus("Stopping");
}

void ConversationFrame::SpeakLastResponse() {
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if (it->role == Role::Assistant && !it->content.empty()) {
            SpeakOnRequest(it->content, &m_conv.settings);
            return;
        }
    }
    if (m_running && !m_streamText.empty()) SpeakOnRequest(m_streamText, &m_conv.settings);
}

void ConversationFrame::CopyLastResponse() {
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if (it->role == Role::Assistant && !it->content.empty()) {
            if (CopyToClipboard(it->content)) SetStatus("Copied the last response");
            return;
        }
    }
}

void ConversationFrame::FocusInput() {
    m_input->SetFocus();
}

void ConversationFrame::ConversationUpdated(const Conversation& c) {
    m_conv.title = c.title;
    m_conv.pinned = c.pinned;
    UpdateTitle();
}

void ConversationFrame::SettingsChanged() {
    UpdateStatusBar();
}

void ConversationFrame::CloseSilently() {
    ReleaseStopHotkey();
    m_silentClose = true;
    m_runner->SetListener(nullptr);
    m_runner->Stop();
    Destroy();
}

void ConversationFrame::OnClose(wxCloseEvent& e) {
    if (m_running && e.CanVeto()) {
        if (!AskYesNo(this, "A response is still arriving. Stop it and close?", "Close")) {
            e.Veto();
            return;
        }
    }
    m_runner->SetListener(nullptr);
    m_runner->Stop();
    m_timer.Stop();
    ReleaseStopHotkey();
    if (!IsMaximized() && !IsIconized()) {
        AppSettings().windowState["conversation"] = {{"w", GetSize().x}, {"h", GetSize().y}};
    }
    if (!m_silentClose && GetMainFrame()) GetMainFrame()->ConversationClosed(m_conv.id);
    Destroy();
}
