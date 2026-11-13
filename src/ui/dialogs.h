#pragma once

// FastChat's dialogs.

#include "core/conversation.h"
#include "core/settings.h"

#include <wx/wx.h>

#include <string>
#include <vector>

struct AgentQuestion;
struct PlanDecision;

// Read-only text in a multiline box, for reading a message line by line,
// account details, conversation information.
void ShowTextDialog(wxWindow* parent, const wxString& title, const wxString& text);

// Choose a provider and model. `allowDefault` adds "Use the default" (both
// results empty). Returns false if cancelled.
bool PickModel(wxWindow* parent, std::string& providerId, std::string& modelId, bool allowDefault,
               const wxString& title = "Choose a model");

// New conversation: returns false if cancelled.
struct NewConversationChoice {
    std::string title;
    ConversationType type = ConversationType::Chat;
    std::string folder;
    std::string providerId;  // empty: default
    std::string model;
};
bool AskNewConversation(wxWindow* parent, NewConversationChoice& out);

// Settings > (Ctrl+,). `page` selects the starting tab (0 general, 1 providers,
// 2 speech, 3 sounds, 4 updates). Returns true if the settings changed.
bool ShowSettingsDialog(wxWindow* parent, int page = 0);

// Add or edit a provider. Returns false if cancelled.
bool EditProvider(wxWindow* parent, ProviderConfig& config, bool adding);

// Fetch and show a provider's account details (credits, spend, limits).
void ShowAccountInfo(wxWindow* parent, const ProviderConfig& config);

// The conversation's own settings. Returns false if cancelled.
bool EditConversationSettings(wxWindow* parent, Conversation& conversation);

// A coding agent's questions with choices (one group of options per question,
// and an answer in the user's own words). False if they chose not to answer;
// otherwise one answer per question.
bool AskAgentQuestions(wxWindow* parent, const wxString& asker, const std::vector<AgentQuestion>& questions,
                       std::vector<std::string>& answers);

// Asking before a command or another action runs: the question, then what it
// is in a read-only box (focused, so it is read first). `allowAll`: the user
// also chose to allow everything for the rest of the conversation.
struct ApprovalRequest {
    wxString title;       // "Run command"
    wxString question;    // "Run this command in the project folder?"
    wxString boxLabel;    // "&Command:"
    std::string what;     // the command, or what the action does
    wxString yesLabel;    // "&Run"
    wxString noLabel;     // "&Don't run"
};
bool AskToAllow(wxWindow* parent, const ApprovalRequest& request, bool& allowAll);

// A coding agent's plan (Claude Code's plan mode): start on it accepting
// edits, start asking before edits, or keep planning (with what to change).
PlanDecision AskPlanApproval(wxWindow* parent, const wxString& asker, const std::string& plan);

