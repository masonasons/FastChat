#include "core/providers/provider.h"
#include "core/providers/provider_impl.h"
#include "core/util.h"

#include <chrono>
#include <cstdio>
#include <filesystem>

// ---------------------------------------------------------------------------
// Coding agent helpers
// ---------------------------------------------------------------------------

std::string AgentPrompt(const AgentTurn& turn) {
    const auto& messages = turn.request.messages;
    // The new message: everything after the last reply.
    size_t firstNew = messages.size();
    while (firstNew > 0 && messages[firstNew - 1].role == Role::User) firstNew--;
    std::string latest;
    for (size_t i = firstNew; i < messages.size(); i++) {
        if (!latest.empty()) latest += "\n\n";
        latest += messages[i].content;
    }
    if (!turn.resumeSession.empty() || firstNew == 0) return latest;
    // A new session for a conversation that already has history (another
    // model answered before, or messages were edited): send it written out.
    std::string text = "Here is our conversation so far.\n\n";
    for (size_t i = 0; i < firstNew; i++) {
        const Message& m = messages[i];
        switch (m.role) {
            case Role::User: text += "User: " + m.content + "\n\n"; break;
            case Role::System: text += "System: " + m.content + "\n\n"; break;
            case Role::Assistant:
                if (!m.content.empty()) text += "Assistant: " + m.content + "\n\n";
                for (const auto& c : m.toolCalls) text += "(Assistant used " + c.name + " " + OneLine(c.arguments, 300) + ")\n";
                break;
            case Role::Tool: text += "(Result: " + OneLine(m.content, 500) + ")\n\n"; break;
        }
    }
    return text + "My new message:\n\n" + latest;
}

std::vector<ImageData> AgentAttachments(const AgentTurn& turn) {
    const auto& messages = turn.request.messages;
    size_t firstNew = messages.size();
    while (firstNew > 0 && messages[firstNew - 1].role == Role::User) firstNew--;
    std::vector<ImageData> files;
    for (size_t i = firstNew; i < messages.size(); i++) {
        for (const auto& img : messages[i].images) files.push_back(img);
    }
    return files;
}

std::string AgentChatFolder() {
    std::string dir = JoinPath(GetDataDir(), "agent-chat");
    std::error_code ec;
    std::filesystem::create_directories(PathFromUtf8(dir), ec);
    return dir;
}

bool CaptureProgram(const std::vector<std::string>& argv, std::string& output, std::string& error, unsigned timeoutMs) {
    ChildProcess child;
    if (!child.Start(argv, std::string(), {}, error)) return false;
    child.CloseStdin();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string line;
    for (;;) {
        ChildProcess::Read r = child.ReadLine(line, 100);
        if (r == ChildProcess::Read::Line) {
            output += line + "\n";
        } else if (r == ChildProcess::Read::End) {
            break;
        } else if (std::chrono::steady_clock::now() > end) {
            error = "It did not answer in time";
            return false;
        }
    }
    int code = child.Wait(5000);
    output = SanitizeUtf8(output);
    if (code != 0 && Trim(output).empty()) {
        error = OneLine(SanitizeUtf8(child.Stderr()), 300);
        if (error.empty()) error = "It exited with code " + std::to_string(code);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

const std::vector<ProviderKindInfo>& ProviderKinds() {
    static const std::vector<ProviderKindInfo> kinds = {
        {"openai", "OpenAI", ApiStyle::OpenAI, "https://api.openai.com/v1", true, true,
         "Admin key (optional, for spend this month)", "https://platform.openai.com/api-keys", "",
         "GPT models from OpenAI."},
        {"anthropic", "Anthropic (Claude)", ApiStyle::Anthropic, "https://api.anthropic.com/v1", true, true,
         "Admin key (optional, for spend this month)", "https://platform.claude.com/settings/keys", "claude-opus-5-5",
         "Claude models from Anthropic."},
        {"gemini", "Google Gemini", ApiStyle::Gemini, "https://generativelanguage.googleapis.com/v1beta", true, false, "",
         "https://aistudio.google.com/apikey", "", "Gemini models through the Gemini API (Google AI Studio)."},
        {"openrouter", "OpenRouter", ApiStyle::OpenAI, "https://openrouter.ai/api/v1", true, false, "",
         "https://openrouter.ai/settings/keys", "", "Hundreds of models from every provider, one key, one balance."},
        {"ollama_cloud", "Ollama Cloud", ApiStyle::Ollama, "https://ollama.com", true, false, "",
         "https://ollama.com/settings/keys", "", "Open models hosted by Ollama."},
        {"ollama", "Ollama (local)", ApiStyle::Ollama, "http://localhost:11434", false, false, "", "", "",
         "Models running on this computer, or another on your network, through Ollama."},
        {"deepseek", "DeepSeek", ApiStyle::OpenAI, "https://api.deepseek.com/v1", true, false, "",
         "https://platform.deepseek.com/api_keys", "deepseek-chat", "DeepSeek's own API."},
        {"groq", "Groq", ApiStyle::OpenAI, "https://api.groq.com/openai/v1", true, false, "",
         "https://console.groq.com/keys", "", "Fast inference of open models."},
        {"mistral", "Mistral", ApiStyle::OpenAI, "https://api.mistral.ai/v1", true, false, "",
         "https://console.mistral.ai/api-keys", "", "Mistral AI's models."},
        {"xai", "xAI (Grok)", ApiStyle::OpenAI, "https://api.x.ai/v1", true, false, "", "https://console.x.ai", "",
         "Grok models from xAI."},
        {"claude_code", "Claude Code", ApiStyle::ClaudeCode, "", false, false, "", "https://claude.com/claude-code",
         "default",
         "Anthropic's coding agent, run on this computer with your Claude login or an API key. In code conversations it "
         "uses its own tools; FastChat asks you before it changes anything."},
        {"codex", "Codex CLI", ApiStyle::Codex, "", false, false, "", "https://github.com/openai/codex", "",
         "OpenAI's coding agent, run on this computer with your ChatGPT login or an API key. In code conversations it "
         "may edit files in the folder but has no network access."},
        {"openai_compatible", "Other OpenAI-compatible service", ApiStyle::OpenAI, "http://localhost:1234/v1", false,
         false, "", "", "", "Any service with an OpenAI-style chat completions API: LM Studio, vLLM, llama.cpp, LiteLLM..."},
    };
    return kinds;
}

const ProviderKindInfo* FindProviderKind(const std::string& key) {
    for (const auto& k : ProviderKinds()) {
        if (key == k.key) return &k;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

json ModelInfoToJson(const ModelInfo& m) {
    json j = {{"id", m.id}, {"name", m.name}};
    if (!m.description.empty()) j["description"] = m.description;
    if (m.contextLength) j["context"] = m.contextLength;
    if (m.maxOutput) j["maxOutput"] = m.maxOutput;
    if (m.inputPrice >= 0) j["inPrice"] = m.inputPrice;
    if (m.outputPrice >= 0) j["outPrice"] = m.outputPrice;
    if (m.cacheReadPrice >= 0) j["cacheReadPrice"] = m.cacheReadPrice;
    if (m.cacheWritePrice >= 0) j["cacheWritePrice"] = m.cacheWritePrice;
    j["tools"] = m.tools;
    if (m.vision) j["vision"] = true;
    if (m.reasoning) j["reasoning"] = true;
    if (m.adaptiveThinking) j["adaptive"] = true;
    if (!m.effortLevels.empty()) j["effort"] = m.effortLevels;
    if (m.created) j["created"] = m.created;
    if (m.free) j["free"] = true;
    return j;
}

ModelInfo ModelInfoFromJson(const json& j) {
    ModelInfo m;
    m.id = j.value("id", std::string());
    m.name = j.value("name", m.id);
    m.description = j.value("description", std::string());
    m.contextLength = j.value("context", int64_t{0});
    m.maxOutput = j.value("maxOutput", int64_t{0});
    m.inputPrice = j.value("inPrice", -1.0);
    m.outputPrice = j.value("outPrice", -1.0);
    m.cacheReadPrice = j.value("cacheReadPrice", -1.0);
    m.cacheWritePrice = j.value("cacheWritePrice", -1.0);
    m.tools = j.value("tools", true);
    m.vision = j.value("vision", false);
    m.reasoning = j.value("reasoning", false);
    m.adaptiveThinking = j.value("adaptive", false);
    if (auto it = j.find("effort"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (e.is_string()) m.effortLevels.push_back(e.get<std::string>());
        }
    }
    m.created = j.value("created", int64_t{0});
    m.free = j.value("free", false);
    return m;
}

static std::string Price(double perMillion) {
    char buf[32];
    if (perMillion == 0) return "free";
    if (perMillion < 0.1) std::snprintf(buf, sizeof(buf), "$%.3f", perMillion);
    else std::snprintf(buf, sizeof(buf), "$%.2f", perMillion);
    std::string s = buf;
    // $4.00 -> $4
    if (EndsWith(s, ".00")) s.resize(s.size() - 3);
    return s;
}

std::string DescribeModel(const ModelInfo& m) {
    std::string s = m.name.empty() ? m.id : m.name;
    std::vector<std::string> parts;
    if (m.contextLength) parts.push_back(FormatTokensShort(m.contextLength) + " context");
    if (m.free || (m.inputPrice == 0 && m.outputPrice == 0)) {
        parts.push_back("free");
    } else if (m.inputPrice >= 0 && m.outputPrice >= 0) {
        parts.push_back(Price(m.inputPrice) + " in, " + Price(m.outputPrice) + " out per million");
    }
    if (m.reasoning) parts.push_back("reasoning");
    if (m.vision) parts.push_back("vision");
    if (!m.tools) parts.push_back("no tools");
    if (!parts.empty()) {
        s += " - ";
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) s += ", ";
            s += parts[i];
        }
    }
    return s;
}

bool LookupKnownModel(const std::string& modelId, ModelInfo& out) {
    struct Known {
        const char* id;
        const char* name;
        int64_t context, maxOutput;
        double in, out, cacheRead;
    };
    // Anthropic's list prices (dollars per million tokens). The Models API reports
    // limits and capabilities but not prices.
    static const Known kKnown[] = {
        {"claude-fable-5-2", "Claude Fable 5.2", 1000000, 128000, 8, 40, 0.20},
        {"claude-mythos-5-2", "Claude Mythos 5.2", 1000000, 128000, 8, 40, 0.20},
        {"claude-fable-5-1", "Claude Fable 5.1", 1000000, 128000, 10, 50, 0.25},
        {"claude-mythos-5-1", "Claude Mythos 5.1", 1000000, 128000, 10, 50, 0.25},
        {"claude-fable-5", "Claude Fable 5", 1000000, 128000, 10, 50, 1.0},
        {"claude-opus-5-5", "Claude Opus 5.5", 1000000, 128000, 4, 20, 0.20},
        {"claude-opus-5", "Claude Opus 5", 1000000, 128000, 5, 25, 0.50},
        {"claude-opus-4-8", "Claude Opus 4.8", 1000000, 128000, 5, 25, 0.50},
        {"claude-opus-4-7", "Claude Opus 4.7", 1000000, 128000, 5, 25, 0.50},
        {"claude-opus-4-6", "Claude Opus 4.6", 1000000, 128000, 5, 25, 0.50},
        {"claude-sonnet-5-5", "Claude Sonnet 5.5", 1000000, 128000, 2, 10, 0.10},
        {"claude-sonnet-5", "Claude Sonnet 5", 1000000, 128000, 2, 10, 0.20},
        {"claude-sonnet-4-6", "Claude Sonnet 4.6", 1000000, 128000, 3, 15, 0.30},
        {"claude-haiku-5-5", "Claude Haiku 5.5", 1000000, 128000, 0.10, 0.50, 0.01},
        {"claude-haiku-4-5", "Claude Haiku 4.5", 200000, 64000, 1, 5, 0.10},
    };
    for (const auto& k : kKnown) {
        // Exact, or a dated snapshot of it ("claude-haiku-4-5-20251001").
        if (modelId == k.id || StartsWith(modelId, std::string(k.id) + "-2")) {
            out.id = modelId;
            if (out.name.empty() || out.name == out.id) out.name = k.name;
            if (!out.contextLength) out.contextLength = k.context;
            if (!out.maxOutput) out.maxOutput = k.maxOutput;
            out.inputPrice = k.in;
            out.outputPrice = k.out;
            out.cacheReadPrice = k.cacheRead;
            out.cacheWritePrice = k.in * 1.25;
            return true;
        }
    }
    return false;
}

void ApplyPricing(const ModelInfo& model, Usage& usage) {
    if (usage.cost >= 0) return;
    if (model.free) {
        usage.cost = 0;
        return;
    }
    if (model.inputPrice < 0 || model.outputPrice < 0) return;
    double cacheRead = model.cacheReadPrice >= 0 ? model.cacheReadPrice : model.inputPrice;
    double cacheWrite = model.cacheWritePrice >= 0 ? model.cacheWritePrice : model.inputPrice;
    int64_t plainInput = usage.inputTokens - usage.cachedInputTokens - usage.cacheWriteTokens;
    if (plainInput < 0) plainInput = 0;
    usage.cost = (plainInput * model.inputPrice + usage.cachedInputTokens * cacheRead +
                  usage.cacheWriteTokens * cacheWrite + usage.outputTokens * model.outputPrice) / 1e6;
}

json ParseJsonOrEmpty(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return json::object();
    return j;
}

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

Provider::Provider(ProviderConfig config) : m_config(std::move(config)) {
    m_kind = FindProviderKind(m_config.kind);
    if (!m_kind) m_kind = FindProviderKind("openai_compatible");
}

std::string Provider::DefaultModel() const {
    if (!m_config.defaultModel.empty()) return m_config.defaultModel;
    if (m_kind->defaultModel && *m_kind->defaultModel) return m_kind->defaultModel;
    std::vector<ModelInfo> models;
    if (AppProviderCache().GetModels(m_config.id, models) && !models.empty()) return models.front().id;
    return std::string();
}

std::string Provider::BaseUrl() const {
    std::string url = m_config.baseUrl.empty() ? m_kind->defaultBaseUrl : m_config.baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

HttpResponse Provider::Send(const std::string& method, const std::string& url,
                            std::vector<std::pair<std::string, std::string>> headers, const std::string& body,
                            const std::atomic<bool>* cancel, std::function<bool(const char*, size_t)> onData) {
    HttpRequest request;
    request.method = method;
    request.url = url;
    request.headers = std::move(headers);
    if (!body.empty()) request.headers.emplace_back("Content-Type", "application/json");
    // Extra headers configured for this provider ({"headers": {"Name": "value"}}).
    if (auto it = m_config.extra.find("headers"); it != m_config.extra.end() && it->is_object()) {
        for (auto& [name, value] : it->items()) {
            if (value.is_string()) request.headers.emplace_back(name, value.get<std::string>());
        }
    }
    request.body = body;
    request.cancel = cancel;
    request.onData = std::move(onData);
    return HttpSend(request);
}

std::string Provider::ErrorFromResponse(const HttpResponse& response) {
    if (!response.completed) return response.error.empty() ? "No response" : response.error;
    std::string message;
    json j = json::parse(response.body, nullptr, false);
    if (!j.is_discarded()) {
        // {"error": {"message": ...}}, {"error": "..."}, {"message": ...}, [{"error": {...}}] (Gemini)
        if (j.is_array() && !j.empty()) j = j[0];
        if (j.is_object()) {
            if (auto e = j.find("error"); e != j.end()) {
                if (e->is_string()) message = e->get<std::string>();
                else if (e->is_object()) {
                    message = e->value("message", std::string());
                    // OpenRouter puts the upstream provider's error in metadata.raw.
                    if (auto md = e->find("metadata"); md != e->end() && md->is_object()) {
                        if (auto raw = md->find("raw"); raw != md->end() && raw->is_string()) {
                            message += " (" + OneLine(raw->get<std::string>(), 300) + ")";
                        }
                    }
                }
            } else if (auto m = j.find("message"); m != j.end() && m->is_string()) {
                message = m->get<std::string>();
            } else if (auto d = j.find("detail"); d != j.end() && d->is_string()) {
                message = d->get<std::string>();
            }
        }
    }
    if (message.empty()) message = OneLine(response.body, 300);
    message = SanitizeUtf8(message);
    std::string status;
    switch (response.status) {
        case 400: status = "Bad request"; break;
        case 401: status = "The API key was not accepted"; break;
        case 402: status = "Payment required (out of credits?)"; break;
        case 403: status = "Not allowed"; break;
        case 404: status = "Not found"; break;
        case 408: status = "Timed out"; break;
        case 413: status = "Request too large"; break;
        case 429: status = "Rate limited"; break;
        case 500: status = "Server error"; break;
        case 502: status = "Bad gateway"; break;
        case 503: status = "Service unavailable"; break;
        case 504: status = "Gateway timed out"; break;
        case 529: status = "Overloaded"; break;
        default: status = "Unexpected response"; break;
    }
    status += " (HTTP " + std::to_string(response.status) + ")";
    if (message.empty()) return status + "; the server sent no explanation";
    return status + ": " + message;
}

void Provider::SetHttpError(ChatResult& result, const HttpResponse& response, const std::string& url) const {
    result.error = ErrorFromResponse(response);
    std::string d = "Provider: " + m_config.name + " (" + m_kind->displayName + ")\n";
    // The address without its query (which may carry a key).
    d += "Address: " + url.substr(0, url.find('?')) + "\n";
    if (response.completed) {
        d += "HTTP status: " + std::to_string(response.status) + "\n";
        for (const char* h : {"request-id", "x-request-id", "cf-ray", "openai-processing-ms", "retry-after"}) {
            std::string v = response.Header(h);
            if (!v.empty()) d += std::string(h) + ": " + v + "\n";
        }
        d += "Response: " + (Trim(response.body).empty() ? std::string("(empty)") : TruncateUtf8(SanitizeUtf8(response.body), 4000)) + "\n";
    } else {
        d += "No response arrived. " + (response.error.empty() ? std::string("No reason was given.") : response.error) + "\n";
    }
    result.errorDetails = d;
}

bool ChatResult::Retriable() const {
    if (ok || cancelled) return false;
    if (httpStatus == 0) return true;  // the connection failed or dropped
    return httpStatus == 408 || httpStatus == 429 || httpStatus >= 500;
}

void Provider::CollectRateLimits(const HttpResponse& response, ChatResult& result) {
    for (const auto& [name, value] : response.headers) {
        if (StartsWith(name, "x-ratelimit-") || StartsWith(name, "anthropic-ratelimit-") || name == "retry-after") {
            result.rateLimits.emplace_back(name, value);
        }
    }
}

ChatResult Provider::RunAgent(const AgentTurn&, const AgentCallbacks&, const std::atomic<bool>*) {
    ChatResult r;
    r.error = "This provider is not a coding agent";
    return r;
}

std::vector<ModelInfo> KnownClaudeModels() {
    std::vector<ModelInfo> list;
    // Aliases follow the newest model of each line; their limits and prices are that model's.
    auto alias = [&](const char* id, const char* name, const char* target) {
        ModelInfo m;
        if (target) LookupKnownModel(target, m);
        m.id = id;
        m.name = name;
        list.push_back(m);
    };
    alias("default", "Default (your plan's default model)", nullptr);
    alias("opus", "Opus (the latest)", "claude-opus-5-5");
    alias("sonnet", "Sonnet (the latest)", "claude-sonnet-5-5");
    alias("haiku", "Haiku (the latest)", "claude-haiku-5-5");
    for (const char* id : {"claude-fable-5-2", "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5",
                           "claude-fable-5-1", "claude-opus-5", "claude-sonnet-5", "claude-opus-4-8",
                           "claude-sonnet-4-6", "claude-haiku-4-5"}) {
        ModelInfo m;
        m.id = id;
        LookupKnownModel(id, m);
        list.push_back(m);
    }
    for (auto& m : list) m.reasoning = true;
    return list;
}

AccountInfo Provider::GetAccountInfo(const std::atomic<bool>*) {
    AccountInfo info;
    info.ok = true;
    info.fetchedAt = NowSeconds();
    return info;
}

std::unique_ptr<Provider> CreateProvider(const ProviderConfig& config) {
    const ProviderKindInfo* kind = FindProviderKind(config.kind);
    ApiStyle style = kind ? kind->style : ApiStyle::OpenAI;
    switch (style) {
        case ApiStyle::Anthropic: return CreateAnthropicProvider(config);
        case ApiStyle::Gemini: return CreateGeminiProvider(config);
        case ApiStyle::Ollama: return CreateOllamaProvider(config);
        case ApiStyle::ClaudeCode: return CreateClaudeCodeProvider(config);
        case ApiStyle::Codex: return CreateCodexProvider(config);
        case ApiStyle::OpenAI: break;
    }
    return CreateOpenAIProvider(config);
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

ProviderCache& AppProviderCache() {
    static ProviderCache cache;
    return cache;
}

static std::string CachePath() {
    return JoinPath(GetDataDir(), "models-cache.json");
}

void ProviderCache::Load() {
    std::string text;
    if (!ReadFileText(CachePath(), text)) return;
    json root = json::parse(text, nullptr, false);
    if (!root.is_object()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, entry] : root.items()) {
        if (!entry.is_object()) continue;
        Entry& e = m_entries[id];
        e.modelsFetchedAt = entry.value("fetchedAt", int64_t{0});
        if (auto it = entry.find("models"); it != entry.end() && it->is_array()) {
            for (const auto& m : *it) e.models.push_back(ModelInfoFromJson(m));
        }
    }
}

void ProviderCache::Save() {
    json root = json::object();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, e] : m_entries) {
            if (e.models.empty()) continue;
            json list = json::array();
            for (const auto& m : e.models) list.push_back(ModelInfoToJson(m));
            root[id] = {{"fetchedAt", e.modelsFetchedAt}, {"models", list}};
        }
    }
    WriteFileText(CachePath(), JsonDump(root));
}

bool ProviderCache::GetModels(const std::string& providerId, std::vector<ModelInfo>& out, int64_t* fetchedAt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(providerId);
    if (it == m_entries.end() || it->second.models.empty()) return false;
    out = it->second.models;
    if (fetchedAt) *fetchedAt = it->second.modelsFetchedAt;
    return true;
}

void ProviderCache::SetModels(const std::string& providerId, const std::vector<ModelInfo>& models) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry& e = m_entries[providerId];
        e.models = models;
        e.modelsFetchedAt = NowSeconds();
    }
    Save();
}

bool ProviderCache::FindModel(const std::string& providerId, const std::string& modelId, ModelInfo& out) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(providerId);
        if (it != m_entries.end()) {
            for (const auto& m : it->second.models) {
                if (m.id == modelId) {
                    out = m;
                    if (out.inputPrice < 0) LookupKnownModel(modelId, out);
                    return true;
                }
            }
        }
    }
    out = ModelInfo();
    out.id = modelId;
    out.name = modelId;
    return LookupKnownModel(modelId, out);
}

void ProviderCache::SetAccount(const std::string& providerId, const AccountInfo& info) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[providerId].account = info;
}

void ProviderCache::SetSummary(const std::string& providerId, const std::string& summary) {
    std::lock_guard<std::mutex> lock(m_mutex);
    AccountInfo& account = m_entries[providerId].account;
    account.summary = summary;
    if (!account.fetchedAt) account.fetchedAt = NowSeconds();
}

bool ProviderCache::GetAccount(const std::string& providerId, AccountInfo& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(providerId);
    if (it == m_entries.end() || !it->second.account.fetchedAt) return false;
    out = it->second.account;
    return true;
}

void ProviderCache::SetRateLimits(const std::string& providerId,
                                  const std::vector<std::pair<std::string, std::string>>& limits) {
    if (limits.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& e = m_entries[providerId];
    e.rateLimits = limits;
    e.rateLimitsAt = NowSeconds();
}

std::vector<std::pair<std::string, std::string>> ProviderCache::GetRateLimits(const std::string& providerId,
                                                                              int64_t* when) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(providerId);
    if (it == m_entries.end()) return {};
    if (when) *when = it->second.rateLimitsAt;
    return it->second.rateLimits;
}

json ArgumentsToObject(const std::string& arguments) {
    if (Trim(arguments).empty()) return json::object();
    json j = json::parse(arguments, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return json::object();
    return j;
}
