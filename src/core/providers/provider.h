#pragma once

// AI providers: one interface (chat with streaming and tools, model lists,
// account details) over each service's own API.

#include "core/conversation.h"
#include "core/platform.h"
#include "core/settings.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Kinds of provider
// ---------------------------------------------------------------------------

enum class ApiStyle {
    OpenAI,
    Anthropic,
    Gemini,
    Ollama,
    // Coding agents run as programs; they bring their own tools.
    ClaudeCode,
    Codex,
};

struct ProviderKindInfo {
    const char* key;
    const char* displayName;
    ApiStyle style;
    const char* defaultBaseUrl;
    bool needsApiKey;
    bool hasAdminKey;          // an optional second key for spend reports
    const char* adminKeyLabel;
    const char* keyPageUrl;    // where to get a key
    const char* defaultModel;
    const char* description;
};

const std::vector<ProviderKindInfo>& ProviderKinds();
const ProviderKindInfo* FindProviderKind(const std::string& key);

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

struct ModelInfo {
    std::string id;
    std::string name;            // display name, or the id
    std::string description;
    int64_t contextLength = 0;   // 0: unknown
    int64_t maxOutput = 0;
    // Dollars per million tokens; negative: unknown.
    double inputPrice = -1;
    double outputPrice = -1;
    double cacheReadPrice = -1;
    double cacheWritePrice = -1;
    bool tools = true;
    bool vision = false;
    bool reasoning = false;      // thinks before answering
    bool adaptiveThinking = false;  // Anthropic: thinking {type: adaptive}
    std::vector<std::string> effortLevels;  // reasoning effort values it accepts
    int64_t created = 0;
    bool free = false;
};

json ModelInfoToJson(const ModelInfo& m);
ModelInfo ModelInfoFromJson(const json& j);

// "Claude Opus 5.5 - 1M context, $4/$20"
std::string DescribeModel(const ModelInfo& m);

// ---------------------------------------------------------------------------
// Account details (credits, spend, limits)
// ---------------------------------------------------------------------------

struct AccountInfo {
    bool ok = false;
    std::string error;
    std::vector<std::pair<std::string, std::string>> lines;  // label, value
    double creditsRemaining = -1;  // dollars, when the provider reports a balance
    std::string summary;           // one line for the status bar ("Plan 50% used this week")
    int64_t fetchedAt = 0;
    void Add(const std::string& label, const std::string& value) { lines.emplace_back(label, value); }
};

// ---------------------------------------------------------------------------
// Chat
// ---------------------------------------------------------------------------

struct ToolDef {
    std::string name;
    std::string description;
    json parameters;  // JSON Schema object
};

struct ChatRequest {
    std::string model;
    std::string systemPrompt;
    std::vector<Message> messages;  // user, assistant and tool messages in order
    std::vector<ToolDef> tools;
    std::optional<double> temperature;
    int maxTokens = 0;               // 0: the provider/model default
    std::string reasoningEffort;     // empty: the model's default
    bool stream = true;
};

struct StreamCallbacks {
    std::function<void(const std::string& delta)> onText;
    std::function<void(const std::string& delta)> onReasoning;
    std::function<void(const std::string& toolName)> onToolCallStarted;
};

struct ChatResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    int httpStatus = 0;
    std::string text;
    std::string reasoning;
    std::vector<ToolCall> toolCalls;
    Usage usage;
    std::string stopReason;
    std::string model;          // the model that actually answered
    std::string responseId;
    json providerData;          // stored on the message; see Message::providerData
    std::vector<std::pair<std::string, std::string>> rateLimits;  // header name, value
};

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

// A whole turn by a coding agent (Claude Code, Codex): it runs its own tools
// until it is done, and FastChat shows and stores what it did.
struct AgentTurn {
    ChatRequest request;          // the history; the last message is the new one
    std::string folder;           // where it works (code conversations)
    bool tools = false;           // false: a plain chat, no tools
    std::string approval = "ask"; // "ask", "auto", "deny"
    std::string resumeSession;    // continue this session instead of sending the history
};

struct AgentCallbacks : StreamCallbacks {
    // A finished assistant message or tool result, in order.
    std::function<void(Message&)> onMessage;
    // A tool is about to run.
    std::function<void(const ToolCall& call, const std::string& description)> onToolRunning;
    // Whether to allow a tool the agent asks permission for (blocks).
    std::function<bool(const std::string& description, bool isCommand)> approve;
    // Plan usage or rate limits the agent reported.
    std::function<void(const std::vector<std::pair<std::string, std::string>>& limits, const std::string& summary)> onLimits;
};

class Provider {
public:
    explicit Provider(ProviderConfig config);
    virtual ~Provider() = default;

    const ProviderConfig& Config() const { return m_config; }
    const ProviderKindInfo& Kind() const { return *m_kind; }
    std::string DefaultModel() const;

    // Blocking; run on a worker thread. Callbacks arrive on that thread too.
    virtual ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                            const std::atomic<bool>* cancel) = 0;
    virtual bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) = 0;
    // Credits, spend, limits; whatever the service reports.
    virtual AccountInfo GetAccountInfo(const std::atomic<bool>* cancel);

    // Coding agents: RunAgent runs a whole turn. The result carries the totals
    // (usage, cost) and, in providerData, the session to resume next time.
    virtual bool IsAgent() const { return false; }
    virtual ChatResult RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks, const std::atomic<bool>* cancel);

protected:
    std::string BaseUrl() const;   // without a trailing slash
    HttpResponse Send(const std::string& method, const std::string& url,
                      std::vector<std::pair<std::string, std::string>> headers, const std::string& body,
                      const std::atomic<bool>* cancel, std::function<bool(const char*, size_t)> onData = nullptr);
    // The service's error message from an error response, or a description of
    // the HTTP status.
    static std::string ErrorFromResponse(const HttpResponse& response);
    static void CollectRateLimits(const HttpResponse& response, ChatResult& result);

    ProviderConfig m_config;
    const ProviderKindInfo* m_kind;
};

std::unique_ptr<Provider> CreateProvider(const ProviderConfig& config);

// ---------------------------------------------------------------------------
// Shared state: cached model lists, the last account details and rate limits.
// Thread-safe.
// ---------------------------------------------------------------------------

class ProviderCache {
public:
    void Load();
    void Save();

    bool GetModels(const std::string& providerId, std::vector<ModelInfo>& out, int64_t* fetchedAt = nullptr);
    void SetModels(const std::string& providerId, const std::vector<ModelInfo>& models);
    // The model's details, from the provider's list or the built-in table.
    bool FindModel(const std::string& providerId, const std::string& modelId, ModelInfo& out);

    void SetAccount(const std::string& providerId, const AccountInfo& info);
    // The account's one-line summary, from what a response reported.
    void SetSummary(const std::string& providerId, const std::string& summary);
    bool GetAccount(const std::string& providerId, AccountInfo& out);

    void SetRateLimits(const std::string& providerId, const std::vector<std::pair<std::string, std::string>>& limits);
    std::vector<std::pair<std::string, std::string>> GetRateLimits(const std::string& providerId, int64_t* when = nullptr);

private:
    std::mutex m_mutex;
    struct Entry {
        std::vector<ModelInfo> models;
        int64_t modelsFetchedAt = 0;
        AccountInfo account;
        std::vector<std::pair<std::string, std::string>> rateLimits;
        int64_t rateLimitsAt = 0;
    };
    std::map<std::string, Entry> m_entries;
};

ProviderCache& AppProviderCache();

// Prices and limits for well-known models, for services whose model list does
// not include them.
bool LookupKnownModel(const std::string& modelId, ModelInfo& out);

// Fill in usage.cost from the model's prices when the provider did not report it.
void ApplyPricing(const ModelInfo& model, Usage& usage);

// Parse JSON leniently: the value, or an empty object.
json ParseJsonOrEmpty(const std::string& text);

// A tool call's arguments as a JSON object (an empty object if they do not parse).
json ArgumentsToObject(const std::string& arguments);
