// Ollama's own API (/api/chat, /api/tags, /api/show), for Ollama Cloud and
// Ollama running locally.

#include "core/providers/provider_impl.h"
#include "core/sse.h"
#include "core/util.h"

#include <algorithm>

namespace {

class OllamaProvider : public Provider {
public:
    using Provider::Provider;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                    const std::atomic<bool>* cancel) override;
    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) override;
    AccountInfo GetAccountInfo(const std::atomic<bool>* cancel) override;

private:
    std::vector<std::pair<std::string, std::string>> Headers() const {
        std::vector<std::pair<std::string, std::string>> h;
        if (!m_config.apiKey.empty()) h.emplace_back("Authorization", "Bearer " + m_config.apiKey);
        return h;
    }
    bool ShowModel(const std::string& name, ModelInfo& info, const std::atomic<bool>* cancel);
};

json BuildMessages(const ChatRequest& request) {
    json messages = json::array();
    if (!request.systemPrompt.empty()) messages.push_back({{"role", "system"}, {"content", request.systemPrompt}});
    for (const auto& m : request.messages) {
        switch (m.role) {
            case Role::System: messages.push_back({{"role", "system"}, {"content", m.content}}); break;
            case Role::User: messages.push_back({{"role", "user"}, {"content", m.content}}); break;
            case Role::Assistant: {
                json msg = {{"role", "assistant"}, {"content", m.content}};
                if (!m.reasoning.empty() && !m.toolCalls.empty()) msg["thinking"] = m.reasoning;
                if (!m.toolCalls.empty()) {
                    json calls = json::array();
                    for (const auto& c : m.toolCalls) {
                        calls.push_back({{"function", {{"name", c.name}, {"arguments", ArgumentsToObject(c.arguments)}}}});
                    }
                    msg["tool_calls"] = calls;
                }
                messages.push_back(msg);
                break;
            }
            case Role::Tool:
                messages.push_back({{"role", "tool"}, {"content", m.content}, {"tool_name", m.toolName}});
                break;
        }
    }
    return messages;
}

ChatResult OllamaProvider::Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                                const std::atomic<bool>* cancel) {
    ChatResult result;
    ModelInfo model;
    AppProviderCache().FindModel(m_config.id, request.model, model);

    json body = {{"model", request.model}, {"messages", BuildMessages(request)}, {"stream", request.stream}};
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            tools.push_back({{"type", "function"},
                             {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
        }
        body["tools"] = tools;
    }
    const std::string& effort = request.reasoningEffort;
    bool levels = ToLower(request.model).find("gpt-oss") != std::string::npos;
    if (effort == "none") {
        if (!levels) body["think"] = false;
    } else if (!effort.empty()) {
        if (levels) body["think"] = effort == "medium" || effort == "low" ? effort : std::string("high");
        else body["think"] = true;
    } else if (model.reasoning) {
        body["think"] = true;
    }
    json options = json::object();
    if (request.temperature) options["temperature"] = *request.temperature;
    if (request.maxTokens > 0) options["num_predict"] = request.maxTokens;
    if (auto ctx = m_config.extra.find("numCtx"); ctx != m_config.extra.end() && ctx->is_number()) {
        options["num_ctx"] = ctx->get<int64_t>();
    }
    if (!options.empty()) body["options"] = options;

    std::string streamError;
    auto handleLine = [&](const json& chunk) {
        if (auto e = chunk.find("error"); e != chunk.end()) {
            streamError = e->is_string() ? e->get<std::string>() : JsonDump(*e);
            return;
        }
        if (auto m = chunk.find("model"); m != chunk.end() && m->is_string()) result.model = m->get<std::string>();
        if (auto msg = chunk.find("message"); msg != chunk.end() && msg->is_object()) {
            if (auto t = msg->find("thinking"); t != msg->end() && t->is_string() && !t->get<std::string>().empty()) {
                result.reasoning += t->get<std::string>();
                if (callbacks.onReasoning) callbacks.onReasoning(t->get<std::string>());
            }
            if (auto c = msg->find("content"); c != msg->end() && c->is_string() && !c->get<std::string>().empty()) {
                result.text += c->get<std::string>();
                if (callbacks.onText) callbacks.onText(c->get<std::string>());
            }
            if (auto calls = msg->find("tool_calls"); calls != msg->end() && calls->is_array()) {
                for (const auto& c : *calls) {
                    const json& fn = c.value("function", json::object());
                    ToolCall call;
                    call.id = c.value("id", std::string());
                    if (call.id.empty()) call.id = "call_" + NewId().substr(0, 16);
                    call.name = fn.value("name", std::string());
                    const json& args = fn.value("arguments", json::object());
                    call.arguments = args.is_string() ? args.get<std::string>() : JsonDump(args);
                    if (callbacks.onToolCallStarted) callbacks.onToolCallStarted(call.name);
                    result.toolCalls.push_back(call);
                }
            }
        }
        if (chunk.value("done", false)) {
            result.stopReason = chunk.value("done_reason", std::string("stop"));
            result.usage.inputTokens = chunk.value("prompt_eval_count", int64_t{0});
            result.usage.outputTokens = chunk.value("eval_count", int64_t{0});
        }
    };

    NdjsonParser parser([&](const std::string& line) {
        json chunk = json::parse(line, nullptr, false);
        if (!chunk.is_discarded() && chunk.is_object()) handleLine(chunk);
        return true;
    });

    HttpResponse response = Send("POST", BaseUrl() + "/api/chat", Headers(), JsonDump(body), cancel,
                                 request.stream ? std::function<bool(const char*, size_t)>(
                                                      [&](const char* d, size_t n) { return parser.Feed(d, n); })
                                                : nullptr);
    result.httpStatus = response.status;
    CollectRateLimits(response, result);
    if (response.cancelled || (cancel && cancel->load())) {
        result.cancelled = true;
    } else if (!response.Ok()) {
        result.error = ErrorFromResponse(response);
        return result;
    }
    if (request.stream) parser.Finish();
    else handleLine(ParseJsonOrEmpty(response.body));
    if (!streamError.empty()) {
        result.error = streamError;
        return result;
    }
    if (result.model.empty()) result.model = request.model;
    result.usage.cost = m_config.kind == "ollama" ? 0 : -1;  // local models cost nothing
    result.ok = !result.cancelled;
    return result;
}

bool OllamaProvider::ShowModel(const std::string& name, ModelInfo& info, const std::atomic<bool>* cancel) {
    HttpResponse r = Send("POST", BaseUrl() + "/api/show", Headers(), JsonDump(json({{"model", name}})), cancel);
    if (!r.Ok()) return false;
    json root = ParseJsonOrEmpty(r.body);
    if (auto mi = root.find("model_info"); mi != root.end() && mi->is_object()) {
        for (auto& [key, value] : mi->items()) {
            if (EndsWith(key, ".context_length") && value.is_number()) info.contextLength = value.get<int64_t>();
        }
    }
    if (auto caps = root.find("capabilities"); caps != root.end() && caps->is_array()) {
        info.tools = false;
        for (const auto& c : *caps) {
            if (c == "tools") info.tools = true;
            if (c == "thinking") info.reasoning = true;
            if (c == "vision") info.vision = true;
        }
    }
    return true;
}

bool OllamaProvider::ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) {
    HttpResponse r = Send("GET", BaseUrl() + "/api/tags", Headers(), std::string(), cancel);
    if (!r.Ok()) {
        error = ErrorFromResponse(r);
        return false;
    }
    out.clear();
    json root = ParseJsonOrEmpty(r.body);
    int shown = 0;
    for (const auto& m : root.value("models", json::array())) {
        ModelInfo info;
        info.id = m.value("model", m.value("name", std::string()));
        if (info.id.empty()) continue;
        info.name = m.value("name", info.id);
        std::vector<std::string> bits;
        if (auto d = m.find("details"); d != m.end() && d->is_object()) {
            std::string ps = d->value("parameter_size", std::string());
            std::string family = d->value("family", std::string());
            std::string q = d->value("quantization_level", std::string());
            if (!ps.empty()) bits.push_back(ps);
            if (!family.empty()) bits.push_back(family);
            if (!q.empty()) bits.push_back(q);
        }
        if (auto size = m.find("size"); size != m.end() && size->is_number() && size->get<uint64_t>() > 0) {
            bits.push_back(FormatBytes(size->get<uint64_t>()));
        }
        for (size_t i = 0; i < bits.size(); i++) info.description += (i ? ", " : "") + bits[i];
        if (m_config.kind == "ollama") info.free = true;
        if (cancel && cancel->load()) return false;
        if (shown++ < 100) ShowModel(info.id, info, cancel);
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
    return true;
}

AccountInfo OllamaProvider::GetAccountInfo(const std::atomic<bool>* cancel) {
    AccountInfo info;
    info.fetchedAt = NowSeconds();
    HttpResponse v = Send("GET", BaseUrl() + "/api/version", Headers(), std::string(), cancel);
    if (v.Ok()) {
        info.ok = true;
        std::string version = ParseJsonOrEmpty(v.body).value("version", std::string());
        if (!version.empty()) info.Add("Ollama version", version);
    } else if (m_config.kind == "ollama") {
        info.error = "Ollama is not answering at " + BaseUrl() + ": " + ErrorFromResponse(v);
        return info;
    }
    info.ok = true;
    if (m_config.kind == "ollama") {
        HttpResponse ps = Send("GET", BaseUrl() + "/api/ps", Headers(), std::string(), cancel);
        if (ps.Ok()) {
            std::string loaded;
            for (const auto& m : ParseJsonOrEmpty(ps.body).value("models", json::array())) {
                if (!loaded.empty()) loaded += ", ";
                loaded += m.value("name", std::string());
            }
            info.Add("Models in memory", loaded.empty() ? "none" : loaded);
        }
    } else {
        info.Add("Usage", "Ollama Cloud shows plan usage at ollama.com/settings; its API does not report it");
    }
    return info;
}

}  // namespace

std::unique_ptr<Provider> CreateOllamaProvider(const ProviderConfig& config) {
    return std::make_unique<OllamaProvider>(config);
}
