#pragma once

// Conversations and their messages, independent of any provider's wire format.

#include "core/events.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

// Serialize JSON without ever throwing: bytes that are not UTF-8 become U+FFFD.
inline std::string JsonDump(const json& j, int indent = -1) {
    return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

enum class Role { System, User, Assistant, Tool };
const char* RoleKey(Role role);
Role RoleFromKey(const std::string& key);

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON text, as the model wrote it
};

struct Usage {
    int64_t inputTokens = 0;        // everything the model read, cached or not
    int64_t outputTokens = 0;       // everything it wrote, reasoning included
    int64_t cachedInputTokens = 0;  // of inputTokens, read from the provider's cache
    int64_t cacheWriteTokens = 0;
    int64_t reasoningTokens = 0;    // of outputTokens
    double cost = -1;               // dollars; negative when unknown
    bool HasTokens() const { return inputTokens > 0 || outputTokens > 0; }
};

json UsageToJson(const Usage& u);
Usage UsageFromJson(const json& j);

struct Message {
    int64_t id = 0;
    int64_t conversationId = 0;
    Role role = Role::User;
    std::string content;
    std::string reasoning;              // the model's visible thinking, if any
    std::vector<ToolCall> toolCalls;    // assistant messages that call tools
    std::string toolCallId;             // tool results: which call they answer
    std::string toolName;
    bool toolError = false;
    std::string providerId;             // which provider/model wrote it (assistant)
    std::string model;
    Usage usage;
    int64_t createdAt = 0;
    int64_t durationMs = 0;             // how long the response took
    std::string stopReason;
    // Provider-specific content that must go back unchanged on the next request
    // (Anthropic thinking blocks with signatures, Gemini thought signatures).
    // Keyed by provider kind; ignored by other providers.
    json providerData;
};

enum class ConversationType { Chat, Code };

// Per conversation speech and sound settings. Every field left empty follows
// the global setting.
struct SpeechOverrides {
    std::optional<bool> enabled;           // speech for this conversation at all
    std::optional<int> speakResponses;     // SpeakResponses value
    std::optional<bool> speakReasoning;
    std::optional<bool> speakToolCalls;
    std::optional<bool> speakInBackground;
    std::optional<bool> interrupt;
    std::optional<std::string> voice;      // SAPI / Prism voice name
    std::optional<int> rate;               // 0-100
};

struct EventOverride {
    std::optional<bool> soundEnabled;
    std::optional<std::string> soundFile;  // "" = the built-in tone
    std::optional<bool> speakEnabled;
    std::optional<std::string> speechText;
    bool Empty() const { return !soundEnabled && !soundFile && !speakEnabled && !speechText; }
};

struct ConversationSettings {
    std::string providerId;                // empty: the default provider
    std::string model;                     // empty: the provider's default model
    std::optional<std::string> systemPrompt;
    std::optional<double> temperature;
    std::optional<int> maxTokens;
    std::string reasoningEffort;           // empty: default; "none", "low", "medium", "high", "xhigh", "max"
    std::string commandApproval;           // empty: global; "ask", "auto", "deny"
    std::optional<bool> soundsEnabled;
    SpeechOverrides speech;
    EventOverride events[static_cast<int>(EventId::Count)];
};

json ConversationSettingsToJson(const ConversationSettings& s);
ConversationSettings ConversationSettingsFromJson(const json& j);

struct Conversation {
    int64_t id = 0;
    std::string title;
    ConversationType type = ConversationType::Chat;
    std::string folder;        // code conversations: the project folder
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
    bool pinned = false;
    ConversationSettings settings;
    // Totals across the conversation (kept up to date by the database).
    int64_t messageCount = 0;
    double totalCost = 0;
    int64_t totalInputTokens = 0;
    int64_t totalOutputTokens = 0;
};

// "Chat" / "Code"
const char* ConversationTypeName(ConversationType t);
