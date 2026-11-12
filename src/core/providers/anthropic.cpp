// Anthropic's Messages API: streaming, tools, adaptive thinking with effort,
// prompt caching, rate-limit headers, the Models API, and (with an admin key)
// the organization's cost report.

#include "core/providers/provider_impl.h"
#include "core/sse.h"
#include "core/util.h"

#include <algorithm>
#include <map>

namespace {

const char* kApiVersion = "2026-10-21";

class AnthropicProvider : public Provider {
public:
    using Provider::Provider;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                    const std::atomic<bool>* cancel) override;
    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) override;
    AccountInfo GetAccountInfo(const std::atomic<bool>* cancel) override;

private:
    std::vector<std::pair<std::string, std::string>> Headers(const std::string& key) const {
        return {{"x-api-key", key}, {"anthropic-version", kApiVersion}};
    }
};

// Models from before adaptive thinking take a token budget instead.
bool GuessAdaptive(const std::string& model) {
    static const char* kOld[] = {"claude-3", "claude-opus-4-0", "claude-opus-4-1", "claude-opus-4-5",
                                 "claude-sonnet-4-0", "claude-sonnet-4-5", "claude-haiku-4-5", "claude-opus-4-2",
                                 "claude-sonnet-4-2", "claude-opus-4-20", "claude-sonnet-4-20"};
    for (const char* old : kOld) {
        if (StartsWith(model, old)) return false;
    }
    return StartsWith(model, "claude-");
}

json ImageBlock(const ImageData& img) {
    return {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", img.mediaType}, {"data", img.base64}}}};
}

json UserContent(const Message& m) {
    json content = json::array();
    for (const auto& img : m.images) content.push_back(ImageBlock(img));
    content.push_back({{"type", "text"}, {"text", m.content.empty() ? std::string(" ") : m.content}});
    return content;
}

json AssistantContent(const Message& m) {
    // The blocks exactly as the API sent them (thinking blocks with their
    // signatures must go back unchanged).
    if (auto pd = m.providerData.find("anthropic"); pd != m.providerData.end() && pd->is_object()) {
        if (auto content = pd->find("content"); content != pd->end() && content->is_array() && !content->empty()) {
            return *content;
        }
    }
    json content = json::array();
    if (!m.content.empty()) content.push_back({{"type", "text"}, {"text", m.content}});
    for (const auto& c : m.toolCalls) {
        content.push_back({{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", ArgumentsToObject(c.arguments)}});
    }
    if (content.empty()) content.push_back({{"type", "text"}, {"text", " "}});
    return content;
}

json BuildMessages(const ChatRequest& request) {
    json messages = json::array();
    auto append = [&](const std::string& role, const json& blocks) {
        // Consecutive turns of the same role become one (tool results after a
        // multi-tool call, a user message after tool results).
        if (!messages.empty() && messages.back()["role"] == role) {
            for (const auto& b : blocks) messages.back()["content"].push_back(b);
        } else {
            messages.push_back({{"role", role}, {"content", blocks}});
        }
    };
    for (const auto& m : request.messages) {
        switch (m.role) {
            case Role::System:
                // Mid-conversation instructions; sent as user text, which every model accepts.
                append("user", json::array({{{"type", "text"}, {"text", "[System] " + m.content}}}));
                break;
            case Role::User:
                append("user", UserContent(m));
                break;
            case Role::Assistant:
                append("assistant", AssistantContent(m));
                break;
            case Role::Tool: {
                json block = {{"type", "tool_result"},
                              {"tool_use_id", m.toolCallId},
                              {"content", m.content.empty() ? std::string("(no output)") : m.content}};
                if (!m.images.empty()) {
                    json content = json::array({{{"type", "text"}, {"text", m.content.empty() ? std::string("(no output)") : m.content}}});
                    for (const auto& img : m.images) content.push_back(ImageBlock(img));
                    block["content"] = content;
                }
                if (m.toolError) block["is_error"] = true;
                append("user", json::array({block}));
                break;
            }
        }
    }
    return messages;
}

ChatResult AnthropicProvider::Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                                   const std::atomic<bool>* cancel) {
    ChatResult result;
    ModelInfo model;
    AppProviderCache().FindModel(m_config.id, request.model, model);

    int64_t maxTokens = request.maxTokens > 0 ? request.maxTokens : 64000;
    if (model.maxOutput > 0) maxTokens = std::min<int64_t>(maxTokens, model.maxOutput);

    json body = {{"model", request.model}, {"max_tokens", maxTokens}, {"messages", BuildMessages(request)},
                 {"stream", request.stream}};
    if (!request.systemPrompt.empty()) {
        body["system"] = json::array({{{"type", "text"}, {"text", request.systemPrompt}}});
    }
    // Cache the conversation so far; each turn reads the last one's prefix.
    body["cache_control"] = {{"type", "ephemeral"}};
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            tools.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.parameters}});
        }
        body["tools"] = tools;
    }
    // Thinking: adaptive, with its summary shown, depth set by effort.
    bool adaptive = model.adaptiveThinking || GuessAdaptive(request.model);
    std::string effort = request.reasoningEffort;
    if (adaptive) {
        if (effort == "none") {
            effort = "low";
        } else {
            body["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};
        }
        if (!effort.empty()) body["output_config"] = {{"effort", effort}};
    } else if (!effort.empty() && effort != "none") {
        // Older models: a thinking budget below max_tokens.
        int64_t budget = effort == "low" ? 4000 : effort == "medium" ? 12000 : 32000;
        budget = std::min<int64_t>(budget, maxTokens - 1024);
        if (budget >= 1024) body["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
    }
    // Sampling parameters are not accepted on this API version; temperature is ignored.

    struct Block {
        json data;
        std::string partialJson;
    };
    std::map<int, Block> blocks;
    std::string streamError;
    bool sawText = false;

    auto readUsage = [&](const json& u) {
        if (!u.is_object()) return;
        int64_t input = u.value("input_tokens", int64_t{-1});
        int64_t cacheRead = u.value("cache_read_input_tokens", int64_t{-1});
        int64_t cacheWrite = u.value("cache_creation_input_tokens", int64_t{-1});
        if (input >= 0 || cacheRead >= 0 || cacheWrite >= 0) {
            if (cacheRead >= 0) result.usage.cachedInputTokens = cacheRead;
            if (cacheWrite >= 0) result.usage.cacheWriteTokens = cacheWrite;
            int64_t plain = input >= 0 ? input : 0;
            result.usage.inputTokens = plain + result.usage.cachedInputTokens + result.usage.cacheWriteTokens;
        }
        if (auto o = u.find("output_tokens"); o != u.end() && o->is_number()) result.usage.outputTokens = o->get<int64_t>();
    };

    auto handleEvent = [&](const json& ev) {
        std::string type = ev.value("type", std::string());
        if (type == "message_start") {
            const json& msg = ev.value("message", json::object());
            result.responseId = msg.value("id", std::string());
            result.model = msg.value("model", request.model);
            readUsage(msg.value("usage", json::object()));
        } else if (type == "content_block_start") {
            int index = ev.value("index", 0);
            Block& b = blocks[index];
            b.data = ev.value("content_block", json::object());
            std::string btype = b.data.value("type", std::string());
            if (btype == "tool_use" && callbacks.onToolCallStarted) {
                callbacks.onToolCallStarted(b.data.value("name", std::string()));
            }
            if (btype == "text") {
                std::string t = b.data.value("text", std::string());
                if (!t.empty()) {
                    result.text += t;
                    if (callbacks.onText) callbacks.onText(t);
                }
            }
        } else if (type == "content_block_delta") {
            int index = ev.value("index", 0);
            Block& b = blocks[index];
            const json& d = ev.value("delta", json::object());
            std::string dtype = d.value("type", std::string());
            if (dtype == "text_delta") {
                std::string t = d.value("text", std::string());
                b.data["text"] = b.data.value("text", std::string()) + t;
                // Separate text blocks of one turn (before and after a tool call) with a blank line.
                if (sawText && b.data["text"].get<std::string>() == t && !result.text.empty() && result.text.back() != '\n') {
                    result.text += "\n\n";
                    if (callbacks.onText) callbacks.onText("\n\n");
                }
                sawText = true;
                result.text += t;
                if (callbacks.onText) callbacks.onText(t);
            } else if (dtype == "thinking_delta") {
                std::string t = d.value("thinking", std::string());
                b.data["thinking"] = b.data.value("thinking", std::string()) + t;
                result.reasoning += t;
                if (callbacks.onReasoning) callbacks.onReasoning(t);
            } else if (dtype == "signature_delta") {
                b.data["signature"] = b.data.value("signature", std::string()) + d.value("signature", std::string());
            } else if (dtype == "input_json_delta") {
                b.partialJson += d.value("partial_json", std::string());
            }
        } else if (type == "content_block_stop") {
            int index = ev.value("index", 0);
            Block& b = blocks[index];
            if (b.data.value("type", std::string()) == "tool_use" || b.data.value("type", std::string()) == "server_tool_use") {
                b.data["input"] = ArgumentsToObject(b.partialJson);
            }
        } else if (type == "message_delta") {
            const json& d = ev.value("delta", json::object());
            if (auto sr = d.find("stop_reason"); sr != d.end() && sr->is_string()) result.stopReason = sr->get<std::string>();
            readUsage(ev.value("usage", json::object()));
        } else if (type == "error") {
            const json& e = ev.value("error", json::object());
            streamError = e.value("message", std::string("Error")) + " (" + e.value("type", std::string()) + ")";
        }
    };

    SseParser sse([&](const std::string&, const std::string& data) {
        json ev = json::parse(data, nullptr, false);
        if (!ev.is_discarded() && ev.is_object()) handleEvent(ev);
        return true;
    });

    HttpResponse response = Send("POST", BaseUrl() + "/messages", Headers(m_config.apiKey), JsonDump(body), cancel,
                                 request.stream ? std::function<bool(const char*, size_t)>(
                                                      [&](const char* d, size_t n) { return sse.Feed(d, n); })
                                                : nullptr);
    result.httpStatus = response.status;
    CollectRateLimits(response, result);
    if (response.cancelled || (cancel && cancel->load())) {
        result.cancelled = true;
    } else if (!response.Ok()) {
        result.error = ErrorFromResponse(response);
        return result;
    }

    if (request.stream) {
        sse.Finish();
    } else {
        // A whole message: replay it as the events a stream would have sent.
        json msg = ParseJsonOrEmpty(response.body);
        handleEvent({{"type", "message_start"}, {"message", msg}});
        int i = 0;
        for (const auto& block : msg.value("content", json::array())) {
            blocks[i].data = block;
            if (block.value("type", "") == "text") result.text += block.value("text", "");
            if (block.value("type", "") == "thinking") result.reasoning += block.value("thinking", "");
            i++;
        }
        result.stopReason = msg.value("stop_reason", std::string());
    }

    json content = json::array();
    for (auto& [index, b] : blocks) {
        std::string btype = b.data.value("type", std::string());
        if (btype == "tool_use") {
            if (b.data.contains("input") && b.data["input"].is_object()) {
                result.toolCalls.push_back({b.data.value("id", std::string()), b.data.value("name", std::string()),
                                            JsonDump(b.data["input"])});
            } else {
                b.data["input"] = json::object();
                result.toolCalls.push_back({b.data.value("id", std::string()), b.data.value("name", std::string()), "{}"});
            }
        }
        content.push_back(b.data);
    }
    // Kept only when it holds something text alone cannot carry back.
    bool needsRaw = false;
    for (const auto& b : content) {
        std::string t = b.value("type", std::string());
        if (t != "text" && t != "tool_use") needsRaw = true;
    }
    if (needsRaw && !result.cancelled) result.providerData["anthropic"] = {{"content", content}};

    if (!streamError.empty()) {
        result.error = streamError;
        return result;
    }
    if (result.stopReason == "refusal") {
        result.error = "The model declined to continue (refusal).";
        if (result.text.empty()) return result;
    }
    if (result.model.empty()) result.model = request.model;
    result.ok = !result.cancelled;
    return result;
}

bool AnthropicProvider::ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) {
    out.clear();
    std::string after;
    for (int page = 0; page < 20; page++) {
        std::string url = BaseUrl() + "/models?limit=1000";
        if (!after.empty()) url += "&after_id=" + UrlEncode(after);
        HttpResponse r = Send("GET", url, Headers(m_config.apiKey), std::string(), cancel);
        if (!r.Ok()) {
            error = ErrorFromResponse(r);
            return false;
        }
        json root = ParseJsonOrEmpty(r.body);
        for (const auto& m : root.value("data", json::array())) {
            ModelInfo info;
            info.id = m.value("id", std::string());
            if (info.id.empty()) continue;
            info.name = m.value("display_name", info.id);
            info.contextLength = m.value("max_input_tokens", int64_t{0});
            info.maxOutput = m.value("max_tokens", int64_t{0});
            info.tools = true;
            if (auto caps = m.find("capabilities"); caps != m.end() && caps->is_object()) {
                const json& c = *caps;
                if (auto v = c.find("image_input"); v != c.end() && v->is_object()) info.vision = v->value("supported", false);
                if (auto t = c.find("thinking"); t != c.end() && t->is_object()) {
                    info.reasoning = t->value("supported", false);
                    if (auto types = t->find("types"); types != t->end() && types->is_object()) {
                        if (auto a = types->find("adaptive"); a != types->end() && a->is_object()) {
                            info.adaptiveThinking = a->value("supported", false);
                        }
                    }
                }
                if (auto e = c.find("effort"); e != c.end() && e->is_object() && e->value("supported", false)) {
                    for (const char* level : {"low", "medium", "high", "xhigh", "max"}) {
                        if (auto l = e->find(level); l != e->end() && l->is_object() && l->value("supported", false)) {
                            info.effortLevels.push_back(level);
                        }
                    }
                }
            } else {
                info.adaptiveThinking = GuessAdaptive(info.id);
            }
            LookupKnownModel(info.id, info);  // prices
            out.push_back(std::move(info));
        }
        if (!root.value("has_more", false)) break;
        after = root.value("last_id", std::string());
        if (after.empty()) break;
    }
    return true;
}

AccountInfo AnthropicProvider::GetAccountInfo(const std::atomic<bool>* cancel) {
    AccountInfo info;
    info.fetchedAt = NowSeconds();
    info.ok = true;
    if (m_config.adminKey.empty()) {
        info.Add("Spend", "Add an admin key (sk-ant-admin...) to see this month's spend");
        return info;
    }
    auto headers = Headers(m_config.adminKey);
    int64_t start = StartOfMonthUtc();
    std::string base = BaseUrl() + "/organizations/cost_report?starting_at=" + UrlEncode(FormatIsoUtc(start)) +
                       "&ending_at=" + UrlEncode(FormatIsoUtc(NowSeconds() + 86400)) + "&limit=31";
    std::string url = base;
    double cents = 0;
    for (int page = 0; page < 10; page++) {
        HttpResponse r = Send("GET", url, headers, std::string(), cancel);
        if (!r.Ok()) {
            info.Add("Spend this month", "unavailable: " + ErrorFromResponse(r));
            return info;
        }
        json root = ParseJsonOrEmpty(r.body);
        for (const auto& bucket : root.value("data", json::array())) {
            for (const auto& res : bucket.value("results", json::array())) {
                // Decimal strings in cents
                auto amount = res.find("amount");
                if (amount == res.end()) continue;
                try {
                    if (amount->is_string()) cents += std::stod(amount->get<std::string>());
                    else if (amount->is_number()) cents += amount->get<double>();
                } catch (...) {
                }
            }
        }
        if (!root.value("has_more", false) || !root.contains("next_page") || !root["next_page"].is_string()) break;
        url = base + "&page=" + UrlEncode(root["next_page"].get<std::string>());
    }
    info.Add("Spend this month", FormatMoney(cents / 100.0));

    // Tokens this month
    std::string usageUrl = BaseUrl() + "/organizations/usage_report/messages?starting_at=" +
                           UrlEncode(FormatIsoUtc(start)) + "&bucket_width=1d&limit=31";
    HttpResponse u = Send("GET", usageUrl, headers, std::string(), cancel);
    if (u.Ok()) {
        int64_t uncached = 0, cacheRead = 0, cacheWrite = 0, output = 0;
        for (const auto& bucket : ParseJsonOrEmpty(u.body).value("data", json::array())) {
            for (const auto& res : bucket.value("results", json::array())) {
                uncached += res.value("uncached_input_tokens", int64_t{0});
                cacheRead += res.value("cache_read_input_tokens", int64_t{0});
                output += res.value("output_tokens", int64_t{0});
                if (auto cc = res.find("cache_creation"); cc != res.end() && cc->is_object()) {
                    for (auto& [k, v] : cc->items()) {
                        if (v.is_number()) cacheWrite += v.get<int64_t>();
                    }
                }
            }
        }
        info.Add("Input tokens this month", FormatThousands(uncached + cacheRead + cacheWrite) + " (" +
                                                FormatThousands(cacheRead) + " read from cache)");
        info.Add("Output tokens this month", FormatThousands(output));
    }
    return info;
}

}  // namespace

std::unique_ptr<Provider> CreateAnthropicProvider(const ProviderConfig& config) {
    return std::make_unique<AnthropicProvider>(config);
}
