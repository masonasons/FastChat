#pragma once

// FastChat's dialogs.

#include "core/conversation.h"
#include "core/settings.h"

#include <wx/wx.h>

#include <string>

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
// 2 speech, 3 sounds). Returns true if the settings changed.
bool ShowSettingsDialog(wxWindow* parent, int page = 0);

// Add or edit a provider. Returns false if cancelled.
bool EditProvider(wxWindow* parent, ProviderConfig& config, bool adding);

// Fetch and show a provider's account details (credits, spend, limits).
void ShowAccountInfo(wxWindow* parent, const ProviderConfig& config);

// The conversation's own settings. Returns false if cancelled.
bool EditConversationSettings(wxWindow* parent, Conversation& conversation);
