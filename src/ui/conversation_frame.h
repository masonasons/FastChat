#pragma once

// A conversation's window: the messages, the box to write in, an Actions
// button, and a status bar with the model, context size and cost.

#include "core/attachments.h"
#include "core/chat_engine.h"
#include "core/conversation.h"
#include "core/notify.h"
#include "ui/activity_window.h"

#include <wx/weakref.h>

#include <wx/wx.h>

#include <memory>
#include <string>
#include <vector>

class MainFrame;

class ConversationFrame : public wxFrame, public ChatListener {
public:
    ConversationFrame(MainFrame* main, const Conversation& conversation);
    ~ConversationFrame() override;

    void FocusInput();
    // Closing from outside (deleting it, closing FastChat): stops any response.
    void CloseSilently();
    // The conversation was changed elsewhere (renamed in the list).
    void ConversationUpdated(const Conversation& c);
    void SettingsChanged();
    void EditSettings();
    // Attach these files to the next message (also what dropping files does).
    void AttachPaths(const wxArrayString& paths);

    // ChatListener
    void OnReasoningDelta(const std::string& text) override;
    void OnTextDelta(const std::string& text) override;
    void OnToolCallStarted(const std::string& toolName) override;
    void OnMessage(Message& message) override;
    void OnToolRunning(const ToolCall& call, const std::string& description) override;
    void OnApproveCommand(const std::string& what, bool isCommand, std::function<void(bool)> answer) override;
    void OnAskQuestions(const std::vector<AgentQuestion>& questions,
                        std::function<void(bool answered, std::vector<std::string> answers)> answer) override;
    void OnFinished(Outcome outcome, const std::string& error) override;
    void OnStatus(const std::string& text) override;
    void OnProblem(const std::string& error, const std::string& details) override;
    void OnQueuedDelivered(Message& message) override;
    void OnActivity(const ActivityEvent& event) override;

private:
    void BuildMenuIds();
    void FillList(int select = -1);
    wxString RowText(int index) const;
    bool ShowsRow(int index) const;
    // Whether a message is from before the latest compaction (shown only in
    // the full history).
    bool BeforeCompaction(int index) const;
    const Message* CallerOf(int index) const;
    std::string CommandOf(int index) const;
    int RowForMessage(int index) const;
    // Buffers: views of the message list (Left and Right).
    void SwitchBuffer(int to);
    bool PendingShown() const;
    bool QueuedShown() const;
    int QueuedStart() const;
    void AppendRow(int index);
    std::string AssistantName(const Message& m) const;
    void SetPendingRow(bool force);
    void RemovePendingRow();
    int SelectedMessage() const;

    void Send();
    bool StartTurn(bool compactOnly = false);
    // Files to send with the next message (Attach files, pasting, dropping).
    void ChooseAttachments();
    void AddAttachment(const Attachment& a);
    bool PasteAttachments();  // false: nothing on the clipboard to attach
    void RemoveAttachment(int index);
    void UpdateAttachments();
    void Compact();
    void ShowActivity();
    bool TakeBackQueued(int row);
    // Messages queued too late for a turn: sent next, or given back after a stop.
    void HandleLeftover(Outcome outcome, std::vector<Message>& leftover);
    int64_t ContextUsed(bool& exact) const;
    bool ResolveModel(ProviderConfig& provider, std::string& model, std::string& error) const;
    std::string SystemPrompt() const;
    void SaveConversation();
    void UpdateTitle();
    void UpdateStatusBar();
    void SetStatus(const wxString& text);
    void MaybeGenerateTitle();
    void RefreshCredits();
    bool WindowActive() const;
    EventContext MakeContext() const;

    // Speech while a response streams: whole sentences as they complete.
    void StreamSpeech(const std::string& text, bool reasoning);
    void FlushStreamSpeech(bool reasoning);

    // Commands
    void ShowActions();
    void ShowMessageMenu();
    void ViewMessage(int index);
    void ViewThinking(int index);
    void MessageDetails(int index);
    void CopyMessage(int index);
    void CopyCodeBlocks(int index);
    void SpeakMessage(int index);
    void DeleteMessage(int index, bool andAfter);
    void EditMessage(int index);
    void RegenerateFrom(int index);
    void Regenerate();
    void ChangeModel();
    void Rename();
    void ShowInfo();
    void Export();
    void ClearMessages();
    void Stop();
    void ReleaseStopHotkey();
    void SpeakLastResponse();
    void CopyLastResponse();

    void OnCommand(wxCommandEvent& e);
    void OnCharHook(wxKeyEvent& e);
    void OnClose(wxCloseEvent& e);

    MainFrame* m_main;
    Conversation m_conv;
    std::vector<Message> m_messages;
    std::vector<int> m_rows;  // list row -> message index (turns that only call tools have no row)
    std::shared_ptr<ChatRunner> m_runner;

    wxPanel* m_panel;
    wxListBox* m_list;
    wxStaticText* m_listLabel = nullptr;
    int m_buffer = 0;
    std::vector<bool> m_beforeCompaction;  // per message, as of the last FillList
    wxTextCtrl* m_input;
    wxStaticText* m_attachLabel = nullptr;
    wxListBox* m_attachList = nullptr;
    std::vector<Attachment> m_attachments;
    int m_pasted = 0;  // pasted images so far, for their names
    wxButton* m_send;
    wxButton* m_attach;
    wxButton* m_actions;
    wxTimer m_timer;

    // The response being received
    bool m_running = false;
    bool m_pendingRow = false;
    bool m_gotText = false;
    bool m_gotReasoning = false;
    std::string m_streamText;
    std::string m_streamReasoning;
    std::string m_speechBuffer;
    std::string m_reasoningSpeechBuffer;
    bool m_spokeFirstSentence = false;
    std::string m_streamModel;
    std::string m_runningTool;  // the pending row while a tool runs
    std::string m_statusText;
    int64_t m_turnStartMs = 0;
    bool m_autoApprove = false;
    bool m_compacting = false;
    std::vector<std::string> m_queued;  // messages sent while it works, not yet taken
    std::vector<Activity> m_activities; // subagents, shells and tools, this session
    wxWeakRef<ActivityWindow> m_activityWindow;
    bool m_stopHotkey = false;
    bool m_silentClose = false;
    int64_t m_lastCreditsCheck = 0;
};
