// OpenAI's chat completions API, and the many services that copy it: OpenRouter,
// DeepSeek, Groq, Mistral, xAI, LM Studio, vLLM...

#include "core/providers/provider_impl.h"
#include "core/sse.h"
#include "core/util.h"

#include <algorithm>

#include <map>

namespace {

class OpenAIProvider : public Provider {
public:
    using Provider::Provider;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                    const std::atomic<bool>* cancel) override;
    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) override;
    AccountInfo GetAccountInfo(const std::atomic<bool>* cancel) override;

private:
    bool IsKind(const char* key) const { return m_config.kind == key; }
    std::vector<std::pair<std::string, std::string>> AuthHeaders() const;
    json BuildMessages(const ChatRequest& request) const;
    void AccountOpenRouter(AccountInfo& info, const std::atomic<bool>* cancel);
    void AccountOpenAI(AccountInfo& info, const std::atomic<bool>* cancel);
    void AccountDeepSeek(AccountInfo& info, const std::atomic<bool>* cancel);
};

std::vector<std::pair<std::string, std::string>> OpenAIProvider::AuthHeaders() const {
    std::vector<std::pair<std::string, std::string>> h;
    if (!m_config.apiKey.empty()) h.emplace_back("Authorization", "Bearer " + m_config.apiKey);
    if (IsKind("openrouter")) {
        // OpenRouter's app attribution
        h.emplace_back("HTTP-Referer", "https://github.com/fastchat-app");
        h.emplace_back("X-Title", "FastChat");
    }
    if (IsKind("openai")) {
        if (auto org = m_config.extra.find("organization"); org != m_config.extra.end() && org->is_string() &&
                                                            !org->get<std::string>().empty()) {
            h.emplace_back("OpenAI-Organization", org->get<std::string>());
        }
    }
    return h;
}

json OpenAIProvider::BuildMessages(const ChatRequest& request) const {
    json messages = json::array();
    if (!request.systemPrompt.empty()) messages.push_back({{"role", "system"}, {"content", request.systemPrompt}});
    // Tool messages take only text: screenshots from tool results follow, once
    // the run of tool results is over, as a user message.
    json pendingImages = json::array();
    auto flushImages = [&]() {
        if (pendingImages.empty()) return;
        json content = json::array({{{"type", "text"}, {"text", "Screenshot from the tool results above."}}});
        for (auto& part : pendingImages) content.push_back(part);
        messages.push_back({{"role", "user"}, {"content", content}});
        pendingImages = json::array();
    };
    auto imagePart = [](const ImageData& img) {
        return json{{"type", "image_url"}, {"image_url", {{"url", "data:" + img.mediaType + ";base64," + img.base64}}}};
    };
    for (const auto& m : request.messages) {
        if (m.role != Role::Tool) flushImages();
        switch (m.role) {
            case Role::System:
                messages.push_back({{"role", "system"}, {"content", m.content}});
                break;
            case Role::User:
                if (m.images.empty()) {
                    messages.push_back({{"role", "user"}, {"content", m.content}});
                } else {
                    json content = json::array({{{"type", "text"}, {"text", m.content}}});
                    for (const auto& img : m.images) content.push_back(imagePart(img));
                    messages.push_back({{"role", "user"}, {"content", content}});
                }
                break;
            case Role::Assistant: {
                json msg = {{"role", "assistant"}};
                msg["content"] = m.content.empty() && !m.toolCalls.empty() ? json(nullptr) : json(m.content);
                if (!m.toolCalls.empty()) {
                    json calls = json::array();
                    for (const auto& c : m.toolCalls) {
                        calls.push_back({{"id", c.id},
                                         {"type", "function"},
                                         {"function", {{"name", c.name}, {"arguments", c.arguments.empty() ? "{}" : c.arguments}}}});
                    }
                    msg["tool_calls"] = calls;
                }
                // Reasoning some services need back within a tool loop.
                if (IsKind("openrouter")) {
                    if (auto pd = m.providerData.find("openrouter"); pd != m.providerData.end() && pd->is_object()) {
                        if (auto rd = pd->find("reasoning_details"); rd != pd->end() && rd->is_array() && !rd->empty()) {
                            msg["reasoning_details"] = *rd;
                        }
                    }
                } else if (IsKind("deepseek") && !m.toolCalls.empty() && !m.reasoning.empty()) {
                    msg["reasoning_content"] = m.reasoning;
                }
                messages.push_back(msg);
                break;
            }
            case Role::Tool:
                messages.push_back({{"role", "tool"}, {"tool_call_id", m.toolCallId}, {"content", m.content}});
                for (const auto& img : m.images) pendingImages.push_back(imagePart(img));
                break;
        }
    }
    flushImages();
    return messages;
}

// Streamed tool calls arrive in pieces, keyed by index.
struct PartialCall {
    std::string id;
    std::string name;
    std::string arguments;
};

// OpenRouter streams reasoning_details in pieces; pieces with the same index
// and type are one detail.
void MergeReasoningDetail(json& details, const json& piece) {
    if (!piece.is_object()) return;
    if (piece.contains("index")) {
        for (auto& d : details) {
            if (d.value("index", -1) == piece.value("index", -2) && d.value("type", "") == piece.value("type", "")) {
                for (auto& [key, value] : piece.items()) {
                    if ((key == "text" || key == "summary" || key == "data") && value.is_string() && d.contains(key) &&
                        d[key].is_string()) {
                        d[key] = d[key].get<std::string>() + value.get<std::string>();
                    } else if (!value.is_null()) {
                        d[key] = value;
                    }
                }
                return;
            }
        }
    }
    details.push_back(piece);
}

ChatResult OpenAIProvider::Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                                const std::atomic<bool>* cancel) {
    ChatResult result;
    json body = {{"model", request.model}, {"messages", BuildMessages(request)}, {"stream", request.stream}};
    if (request.stream && !IsKind("mistral")) body["stream_options"] = {{"include_usage", true}};
    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& t : request.tools) {
            tools.push_back({{"type", "function"},
                             {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
        }
        body["tools"] = tools;
    }
    if (request.temperature) body["temperature"] = *request.temperature;
    if (request.maxTokens > 0) {
        body[IsKind("openai") ? "max_completion_tokens" : "max_tokens"] = request.maxTokens;
    }
    if (!request.reasoningEffort.empty()) {
        if (IsKind("openrouter")) {
            if (request.reasoningEffort == "none") body["reasoning"] = {{"enabled", false}};
            else body["reasoning"] = {{"effort", request.reasoningEffort == "max" ? "xhigh" : request.reasoningEffort}};
        } else {
            body["reasoning_effort"] = request.reasoningEffort;
        }
    }
    if (IsKind("openrouter")) body["usage"] = {{"include", true}};

    std::map<int, PartialCall> calls;
    json reasoningDetails = json::array();
    std::string streamError;
    bool announcedReasoning = false;

    auto handleChunk = [&](const json& chunk) {
        if (auto e = chunk.find("error"); e != chunk.end()) {
            streamError = e->is_object() ? e->value("message", JsonDump(*e)) : JsonDump(*e);
            return;
        }
        if (auto id = chunk.find("id"); id != chunk.end() && id->is_string()) result.responseId = id->get<std::string>();
        if (auto model = chunk.find("model"); model != chunk.end() && model->is_string()) {
            result.model = model->get<std::string>();
        }
        if (auto usage = chunk.find("usage"); usage != chunk.end() && usage->is_object()) {
            const json& u = *usage;
            result.usage.inputTokens = u.value("prompt_tokens", int64_t{0});
            result.usage.outputTokens = u.value("completion_tokens", int64_t{0});
            if (auto d = u.find("prompt_tokens_details"); d != u.end() && d->is_object()) {
                result.usage.cachedInputTokens = d->value("cached_tokens", int64_t{0});
                result.usage.cacheWriteTokens = d->value("cache_write_tokens", int64_t{0});
            }
            if (auto d = u.find("completion_tokens_details"); d != u.end() && d->is_object()) {
                result.usage.reasoningTokens = d->value("reasoning_tokens", int64_t{0});
            }
            // DeepSeek reports its cache this way
            if (u.contains("prompt_cache_hit_tokens")) {
                result.usage.cachedInputTokens = u.value("prompt_cache_hit_tokens", int64_t{0});
            }
            if (auto cost = u.find("cost"); cost != u.end() && cost->is_number()) result.usage.cost = cost->get<double>();
        }
        auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array() || choices->empty()) return;
        const json& choice = (*choices)[0];
        if (auto fr = choice.find("finish_reason"); fr != choice.end() && fr->is_string()) {
            result.stopReason = fr->get<std::string>();
        }
        // Streaming uses "delta"; a whole response uses "message".
        const json* delta = nullptr;
        if (auto d = choice.find("delta"); d != choice.end() && d->is_object()) delta = &*d;
        else if (auto m = choice.find("message"); m != choice.end() && m->is_object()) delta = &*m;
        if (!delta) return;
        for (const char* key : {"reasoning_content", "reasoning"}) {
            if (auto r = delta->find(key); r != delta->end() && r->is_string() && !r->get<std::string>().empty()) {
                std::string piece = r->get<std::string>();
                result.reasoning += piece;
                if (!announcedReasoning) announcedReasoning = true;
                if (callbacks.onReasoning) callbacks.onReasoning(piece);
                break;  // OpenRouter sends both with the same text
            }
        }
        if (auto rd = delta->find("reasoning_details"); rd != delta->end() && rd->is_array()) {
            for (const auto& piece : *rd) MergeReasoningDetail(reasoningDetails, piece);
        }
        if (auto c = delta->find("content"); c != delta->end() && c->is_string()) {
            std::string piece = c->get<std::string>();
            if (!piece.empty()) {
                result.text += piece;
                if (callbacks.onText) callbacks.onText(piece);
            }
        }
        if (auto tc = delta->find("tool_calls"); tc != delta->end() && tc->is_array()) {
            for (const auto& call : *tc) {
                int index = call.value("index", static_cast<int>(calls.size()));
                PartialCall& p = calls[index];
                if (auto id = call.find("id"); id != call.end() && id->is_string() && !id->get<std::string>().empty()) {
                    p.id = id->get<std::string>();
                }
                if (auto fn = call.find("function"); fn != call.end() && fn->is_object()) {
                    if (auto n = fn->find("name"); n != fn->end() && n->is_string() && !n->get<std::string>().empty()) {
                        bool first = p.name.empty();
                        p.name += n->get<std::string>();
                        if (first && callbacks.onToolCallStarted) callbacks.onToolCallStarted(p.name);
                    }
                    if (auto a = fn->find("arguments"); a != fn->end()) {
                        if (a->is_string()) p.arguments += a->get<std::string>();
                        else if (a->is_object()) p.arguments = JsonDump(*a);
                    }
                }
            }
        }
    };

    SseParser sse([&](const std::string&, const std::string& data) {
        if (data == "[DONE]") return true;
        json chunk = json::parse(data, nullptr, false);
        if (!chunk.is_discarded() && chunk.is_object()) handleChunk(chunk);
        return true;
    });

    auto headers = AuthHeaders();
    headers.emplace_back("Accept", request.stream ? "text/event-stream" : "application/json");
    HttpResponse response = Send("POST", BaseUrl() + "/chat/completions", headers, JsonDump(body), cancel,
                                 request.stream ? std::function<bool(const char*, size_t)>(
                                                      [&](const char* d, size_t n) { return sse.Feed(d, n); })
                                                : nullptr);
    result.httpStatus = response.status;
    CollectRateLimits(response, result);
    if (response.cancelled || (cancel && cancel->load())) {
        result.cancelled = true;
    } else if (!response.Ok()) {
        SetHttpError(result, response, BaseUrl() + "/chat/completions");
        return result;
    }
    if (request.stream) sse.Finish();
    else handleChunk(ParseJsonOrEmpty(response.body));

    for (auto& [index, p] : calls) {
        if (p.id.empty()) p.id = "call_" + NewId().substr(0, 24);
        result.toolCalls.push_back({p.id, p.name, p.arguments});
    }
    if (!reasoningDetails.empty()) result.providerData["openrouter"] = {{"reasoning_details", reasoningDetails}};
    if (!streamError.empty()) {
        result.error = streamError;
        result.errorDetails = "Provider: " + m_config.name + "\nThe service reported this in the middle of its reply: " + streamError + "\n";
        return result;
    }
    if (result.model.empty()) result.model = request.model;
    result.ok = !result.cancelled;
    return result;
}

bool IsChatModelId(const std::string& id) {
    static const char* kSkip[] = {"embedding", "whisper", "tts", "dall-e", "moderation", "davinci", "babbage",
                                  "transcribe", "realtime", "image", "audio", "search-preview", "computer-use",
                                  "sora", "omni-moderation"};
    std::string lower = ToLower(id);
    for (const char* s : kSkip) {
        if (lower.find(s) != std::string::npos) return false;
    }
    return true;
}

double PerMillion(const json& v) {
    try {
        if (v.is_string()) return std::stod(v.get<std::string>()) * 1e6;
        if (v.is_number()) return v.get<double>() * 1e6;
    } catch (...) {
    }
    return -1;
}

bool OpenAIProvider::ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) {
    std::string url = BaseUrl() + "/models";
    HttpResponse response = Send("GET", url, AuthHeaders(), std::string(), cancel);
    if (!response.Ok()) {
        error = ErrorFromResponse(response);
        return false;
    }
    json root = ParseJsonOrEmpty(response.body);
    const json* data = nullptr;
    if (root.is_object()) {
        if (auto d = root.find("data"); d != root.end() && d->is_array()) data = &*d;
    } else if (root.is_array()) {
        data = &root;
    }
    if (!data) {
        error = "The model list was not understood";
        return false;
    }
    out.clear();
    for (const auto& m : *data) {
        if (!m.is_object()) continue;
        ModelInfo info;
        info.id = m.value("id", std::string());
        if (info.id.empty()) continue;
        info.name = m.value("name", info.id);
        info.description = m.value("description", std::string());
        info.created = m.value("created", int64_t{0});
        if (IsKind("openai") && !IsChatModelId(info.id)) continue;
        // OpenRouter
        info.contextLength = m.value("context_length", int64_t{0});
        if (auto p = m.find("pricing"); p != m.end() && p->is_object()) {
            if (auto v = p->find("prompt"); v != p->end()) info.inputPrice = PerMillion(*v);
            if (auto v = p->find("completion"); v != p->end()) info.outputPrice = PerMillion(*v);
            if (auto v = p->find("input_cache_read"); v != p->end()) info.cacheReadPrice = PerMillion(*v);
            if (auto v = p->find("input_cache_write"); v != p->end()) info.cacheWritePrice = PerMillion(*v);
            info.free = info.inputPrice == 0 && info.outputPrice == 0;
        }
        if (auto tp = m.find("top_provider"); tp != m.end() && tp->is_object()) {
            if (auto v = tp->find("max_completion_tokens"); v != tp->end() && v->is_number()) {
                info.maxOutput = v->get<int64_t>();
            }
        }
        if (auto sp = m.find("supported_parameters"); sp != m.end() && sp->is_array()) {
            info.tools = false;
            for (const auto& p : *sp) {
                if (!p.is_string()) continue;
                std::string s = p.get<std::string>();
                if (s == "tools") info.tools = true;
                if (s == "reasoning" || s == "include_reasoning") info.reasoning = true;
            }
        }
        if (auto arch = m.find("architecture"); arch != m.end() && arch->is_object()) {
            if (auto im = arch->find("input_modalities"); im != arch->end() && im->is_array()) {
                for (const auto& x : *im) {
                    if (x == "image") info.vision = true;
                }
            }
        }
        // Groq, Mistral and others
        if (!info.contextLength) info.contextLength = m.value("context_window", int64_t{0});
        if (!info.contextLength) info.contextLength = m.value("max_context_length", int64_t{0});
        if (auto caps = m.find("capabilities"); caps != m.end() && caps->is_object()) {
            info.tools = caps->value("function_calling", info.tools);
            info.vision = caps->value("vision", info.vision);
        }
        out.push_back(std::move(info));
    }
    // Newest first when the service says when models were made, else by name.
    std::sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) {
        if (a.created != b.created) return a.created > b.created;
        return a.id < b.id;
    });
    return true;
}

void OpenAIProvider::AccountOpenRouter(AccountInfo& info, const std::atomic<bool>* cancel) {
    std::string base = BaseUrl();
    HttpResponse key = Send("GET", base + "/key", AuthHeaders(), std::string(), cancel);
    if (key.Ok()) {
        json d = ParseJsonOrEmpty(key.body).value("data", json::object());
        if (d.contains("label") && d["label"].is_string()) info.Add("Key", d["label"].get<std::string>());
        auto money = [&](const char* field, const char* label) {
            if (auto v = d.find(field); v != d.end() && v->is_number()) info.Add(label, FormatMoney(v->get<double>()));
        };
        if (auto limit = d.find("limit"); limit != d.end()) {
            if (limit->is_number()) info.Add("Key spending limit", FormatMoney(limit->get<double>()));
            else info.Add("Key spending limit", "none");
        }
        money("limit_remaining", "Left under the key's limit");
        if (auto reset = d.find("limit_reset"); reset != d.end() && reset->is_string()) {
            info.Add("Limit resets", reset->get<std::string>());
        }
        money("usage", "Key usage, all time");
        money("usage_daily", "Key usage today");
        money("usage_weekly", "Key usage this week");
        money("usage_monthly", "Key usage this month");
        money("byok_usage", "Bring-your-own-key usage");
        if (auto free = d.find("is_free_tier"); free != d.end() && free->is_boolean()) {
            info.Add("Free tier", free->get<bool>() ? "yes" : "no");
        }
        if (auto fm = d.find("free_model_daily_requests"); fm != d.end() && fm->is_object()) {
            std::string used = fm->contains("used") ? JsonDump(fm->at("used")) : "?";
            std::string limit = fm->contains("limit") ? JsonDump(fm->at("limit")) : "?";
            info.Add("Free model requests today", used + " of " + limit);
        }
        info.ok = true;
    } else {
        info.error = ErrorFromResponse(key);
    }
    HttpResponse credits = Send("GET", base + "/credits", AuthHeaders(), std::string(), cancel);
    if (credits.Ok()) {
        json d = ParseJsonOrEmpty(credits.body).value("data", json::object());
        double total = d.value("total_credits", -1.0);
        double used = d.value("total_usage", -1.0);
        if (total >= 0 && used >= 0) {
            info.creditsRemaining = total - used;
            info.lines.insert(info.lines.begin(), {"Credits remaining", FormatMoney(total - used)});
            info.lines.insert(info.lines.begin() + 1, {"Credits purchased", FormatMoney(total)});
            info.lines.insert(info.lines.begin() + 2, {"Credits used", FormatMoney(used)});
            info.ok = true;
        }
    }
}

void OpenAIProvider::AccountOpenAI(AccountInfo& info, const std::atomic<bool>* cancel) {
    info.ok = true;
    if (m_config.adminKey.empty()) {
        info.Add("Spend", "Add an admin key to see this month's spend");
        return;
    }
    std::vector<std::pair<std::string, std::string>> headers = {{"Authorization", "Bearer " + m_config.adminKey}};
    int64_t start = StartOfMonthUtc();
    std::string url = BaseUrl() + "/organization/costs?start_time=" + std::to_string(start) + "&bucket_width=1d&limit=31";
    double total = 0;
    std::string currency = "usd";
    for (int page = 0; page < 10; page++) {
        HttpResponse r = Send("GET", url, headers, std::string(), cancel);
        if (!r.Ok()) {
            info.Add("Spend this month", "unavailable: " + ErrorFromResponse(r));
            return;
        }
        json root = ParseJsonOrEmpty(r.body);
        for (const auto& bucket : root.value("data", json::array())) {
            for (const auto& res : bucket.value("results", json::array())) {
                if (auto amount = res.find("amount"); amount != res.end() && amount->is_object()) {
                    total += amount->value("value", 0.0);
                    currency = amount->value("currency", currency);
                }
            }
        }
        if (!root.value("has_more", false) || !root.contains("next_page") || !root["next_page"].is_string()) break;
        url = BaseUrl() + "/organization/costs?start_time=" + std::to_string(start) +
              "&bucket_width=1d&limit=31&page=" + UrlEncode(root["next_page"].get<std::string>());
    }
    info.Add("Spend this month", currency == "usd" ? FormatMoney(total) : std::to_string(total) + " " + currency);

    // Tokens this month, from the usage report
    std::string usageUrl = BaseUrl() + "/organization/usage/completions?start_time=" + std::to_string(start) +
                           "&bucket_width=1d&limit=31";
    HttpResponse u = Send("GET", usageUrl, headers, std::string(), cancel);
    if (u.Ok()) {
        int64_t in = 0, out = 0, cached = 0, requests = 0;
        for (const auto& bucket : ParseJsonOrEmpty(u.body).value("data", json::array())) {
            for (const auto& res : bucket.value("results", json::array())) {
                in += res.value("input_tokens", int64_t{0});
                out += res.value("output_tokens", int64_t{0});
                cached += res.value("input_cached_tokens", int64_t{0});
                requests += res.value("num_model_requests", int64_t{0});
            }
        }
        info.Add("Requests this month", FormatThousands(requests));
        info.Add("Input tokens this month", FormatThousands(in) + " (" + FormatThousands(cached) + " cached)");
        info.Add("Output tokens this month", FormatThousands(out));
    }
}

void OpenAIProvider::AccountDeepSeek(AccountInfo& info, const std::atomic<bool>* cancel) {
    std::string base = BaseUrl();
    if (EndsWith(base, "/v1")) base.resize(base.size() - 3);
    HttpResponse r = Send("GET", base + "/user/balance", AuthHeaders(), std::string(), cancel);
    if (!r.Ok()) {
        info.error = ErrorFromResponse(r);
        return;
    }
    json root = ParseJsonOrEmpty(r.body);
    info.ok = true;
    if (root.contains("is_available")) info.Add("Account usable", root.value("is_available", false) ? "yes" : "no");
    for (const auto& b : root.value("balance_infos", json::array())) {
        std::string currency = b.value("currency", std::string());
        std::string total = b.value("total_balance", std::string());
        info.Add("Balance (" + currency + ")", total);
        info.Add("Granted (" + currency + ")", b.value("granted_balance", std::string()));
        info.Add("Topped up (" + currency + ")", b.value("topped_up_balance", std::string()));
        if (currency == "USD") {
            try {
                info.creditsRemaining = std::stod(total);
            } catch (...) {
            }
        }
    }
}

AccountInfo OpenAIProvider::GetAccountInfo(const std::atomic<bool>* cancel) {
    AccountInfo info;
    info.fetchedAt = NowSeconds();
    if (IsKind("openrouter")) AccountOpenRouter(info, cancel);
    else if (IsKind("openai")) AccountOpenAI(info, cancel);
    else if (IsKind("deepseek")) AccountDeepSeek(info, cancel);
    else info.ok = true;
    return info;
}

}  // namespace

std::unique_ptr<Provider> CreateOpenAIProvider(const ProviderConfig& config) {
    return std::make_unique<OpenAIProvider>(config);
}
