// Google's Gemini API (generativelanguage.googleapis.com): streamed
// generateContent with function calling, thinking (summaries shown), and
// thought signatures kept for the next request.

#include "core/providers/provider_impl.h"
#include "core/sse.h"
#include "core/util.h"

namespace {

class GeminiProvider : public Provider {
public:
    using Provider::Provider;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                    const std::atomic<bool>* cancel) override;
    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) override;
    AccountInfo GetAccountInfo(const std::atomic<bool>* cancel) override;

private:
    std::vector<std::pair<std::string, std::string>> Headers() const { return {{"x-goog-api-key", m_config.apiKey}}; }
};

// Gemini takes an OpenAPI subset of JSON Schema; drop what it rejects.
json CleanSchema(const json& schema) {
    if (schema.is_object()) {
        json out = json::object();
        for (auto& [key, value] : schema.items()) {
            if (key == "additionalProperties" || key == "$schema" || key == "default") continue;
            out[key] = CleanSchema(value);
        }
        return out;
    }
    if (schema.is_array()) {
        json out = json::array();
        for (const auto& v : schema) out.push_back(CleanSchema(v));
        return out;
    }
    return schema;
}

json BuildContents(const ChatRequest& request) {
    json contents = json::array();
    auto append = [&](const std::string& role, const json& parts) {
        if (!contents.empty() && contents.back()["role"] == role) {
            for (const auto& p : parts) contents.back()["parts"].push_back(p);
        } else {
            contents.push_back({{"role", role}, {"parts", parts}});
        }
    };
    for (const auto& m : request.messages) {
        switch (m.role) {
            case Role::System:
            case Role::User: {
                json parts = json::array();
                // Images and PDFs (inline data, which Gemini takes as either); a
                // message of attachments alone has no empty text part.
                for (const auto& img : m.images) parts.push_back({{"inlineData", {{"mimeType", img.mediaType}, {"data", img.base64}}}});
                if (!m.content.empty() || parts.empty()) parts.push_back({{"text", m.role == Role::System ? "[System] " + m.content : m.content}});
                append("user", parts);
                break;
            }
            case Role::Assistant: {
                if (auto pd = m.providerData.find("gemini"); pd != m.providerData.end() && pd->is_object()) {
                    if (auto parts = pd->find("parts"); parts != pd->end() && parts->is_array() && !parts->empty()) {
                        append("model", *parts);
                        break;
                    }
                }
                json parts = json::array();
                if (!m.content.empty()) parts.push_back({{"text", m.content}});
                for (const auto& c : m.toolCalls) {
                    parts.push_back({{"functionCall", {{"name", c.name}, {"args", ArgumentsToObject(c.arguments)}}}});
                }
                if (parts.empty()) parts.push_back({{"text", " "}});
                append("model", parts);
                break;
            }
            case Role::Tool: {
                json response = {{"result", m.content}};
                if (m.toolError) response = {{"error", m.content}};
                json fr = {{"name", m.toolName}, {"response", response}};
                if (!m.toolCallId.empty() && !StartsWith(m.toolCallId, "gemini_")) fr["id"] = m.toolCallId;
                json parts = json::array({{{"functionResponse", fr}}});
                for (const auto& img : m.images) parts.push_back({{"inlineData", {{"mimeType", img.mediaType}, {"data", img.base64}}}});
                append("user", parts);
                break;
            }
        }
    }
    return contents;
}

ChatResult GeminiProvider::Chat(const ChatRequest& request, const StreamCallbacks& callbacks,
                                const std::atomic<bool>* cancel) {
    ChatResult result;
    json body = {{"contents", BuildContents(request)}};
    if (!request.systemPrompt.empty()) {
        body["systemInstruction"] = {{"parts", json::array({{{"text", request.systemPrompt}}})}};
    }
    if (!request.tools.empty()) {
        json decls = json::array();
        for (const auto& t : request.tools) {
            decls.push_back({{"name", t.name}, {"description", t.description}, {"parameters", CleanSchema(t.parameters)}});
        }
        body["tools"] = json::array({{{"functionDeclarations", decls}}});
    }
    json gen = json::object();
    if (request.temperature) gen["temperature"] = *request.temperature;
    if (request.maxTokens > 0) gen["maxOutputTokens"] = request.maxTokens;
    json thinking = {{"includeThoughts", true}};
    const std::string& effort = request.reasoningEffort;
    bool budgetStyle = request.model.find("gemini-2") != std::string::npos;
    if (!effort.empty()) {
        if (budgetStyle) {
            thinking["thinkingBudget"] = effort == "none" ? 0 : effort == "low" ? 1024 : effort == "medium" ? 8192 : 24576;
            if (effort == "none") thinking["includeThoughts"] = false;
        } else {
            thinking["thinkingLevel"] = (effort == "none" || effort == "low") ? "low" : effort == "medium" ? "medium" : "high";
        }
    }
    gen["thinkingConfig"] = thinking;
    body["generationConfig"] = gen;

    json modelParts = json::array();  // every part the model sent, for the next request
    std::string streamError;
    int callCount = 0;

    auto handleChunk = [&](const json& chunk) {
        if (auto e = chunk.find("error"); e != chunk.end() && e->is_object()) {
            streamError = e->value("message", std::string("Error"));
            return;
        }
        if (auto v = chunk.find("modelVersion"); v != chunk.end() && v->is_string()) result.model = v->get<std::string>();
        if (auto id = chunk.find("responseId"); id != chunk.end() && id->is_string()) result.responseId = id->get<std::string>();
        if (auto u = chunk.find("usageMetadata"); u != chunk.end() && u->is_object()) {
            result.usage.inputTokens = u->value("promptTokenCount", int64_t{0}) + u->value("toolUsePromptTokenCount", int64_t{0});
            result.usage.reasoningTokens = u->value("thoughtsTokenCount", int64_t{0});
            result.usage.outputTokens = u->value("candidatesTokenCount", int64_t{0}) + result.usage.reasoningTokens;
            result.usage.cachedInputTokens = u->value("cachedContentTokenCount", int64_t{0});
        }
        if (auto pf = chunk.find("promptFeedback"); pf != chunk.end() && pf->is_object()) {
            if (auto br = pf->find("blockReason"); br != pf->end() && br->is_string()) {
                streamError = "The prompt was blocked (" + br->get<std::string>() + ")";
            }
        }
        auto cands = chunk.find("candidates");
        if (cands == chunk.end() || !cands->is_array() || cands->empty()) return;
        const json& cand = (*cands)[0];
        if (auto fr = cand.find("finishReason"); fr != cand.end() && fr->is_string()) result.stopReason = fr->get<std::string>();
        auto content = cand.find("content");
        if (content == cand.end() || !content->is_object()) return;
        for (const auto& part : content->value("parts", json::array())) {
            if (!part.is_object()) continue;
            bool thought = part.value("thought", false);
            if (auto t = part.find("text"); t != part.end() && t->is_string()) {
                std::string text = t->get<std::string>();
                if (thought) {
                    result.reasoning += text;
                    if (callbacks.onReasoning) callbacks.onReasoning(text);
                } else {
                    result.text += text;
                    if (callbacks.onText) callbacks.onText(text);
                }
                // Merge streamed text pieces of the same kind into one part.
                if (!modelParts.empty() && modelParts.back().contains("text") &&
                    modelParts.back().value("thought", false) == thought && !part.contains("thoughtSignature") &&
                    !modelParts.back().contains("thoughtSignature")) {
                    modelParts.back()["text"] = modelParts.back()["text"].get<std::string>() + text;
                } else {
                    modelParts.push_back(part);
                }
                continue;
            }
            if (auto fc = part.find("functionCall"); fc != part.end() && fc->is_object()) {
                ToolCall call;
                call.name = fc->value("name", std::string());
                call.id = fc->value("id", std::string());
                if (call.id.empty()) call.id = "gemini_" + std::to_string(callCount);
                callCount++;
                call.arguments = JsonDump(fc->value("args", json::object()));
                if (callbacks.onToolCallStarted) callbacks.onToolCallStarted(call.name);
                result.toolCalls.push_back(call);
            }
            modelParts.push_back(part);
        }
    };

    SseParser sse([&](const std::string&, const std::string& data) {
        json chunk = json::parse(data, nullptr, false);
        if (!chunk.is_discarded() && chunk.is_object()) handleChunk(chunk);
        return true;
    });

    std::string url = BaseUrl() + "/models/" + request.model +
                      (request.stream ? ":streamGenerateContent?alt=sse" : ":generateContent");
    HttpResponse response = Send("POST", url, Headers(), JsonDump(body), cancel,
                                 request.stream ? std::function<bool(const char*, size_t)>(
                                                      [&](const char* d, size_t n) { return sse.Feed(d, n); })
                                                : nullptr);
    result.httpStatus = response.status;
    CollectRateLimits(response, result);
    if (response.cancelled || (cancel && cancel->load())) {
        result.cancelled = true;
    } else if (!response.Ok()) {
        SetHttpError(result, response, url);
        return result;
    }
    if (request.stream) sse.Finish();
    else handleChunk(ParseJsonOrEmpty(response.body));

    if (!modelParts.empty() && !result.cancelled) result.providerData["gemini"] = {{"parts", modelParts}};
    if (!streamError.empty()) {
        result.error = streamError;
        result.errorDetails = "Provider: " + m_config.name + "\nThe service reported this in the middle of its reply: " + streamError + "\n";
        return result;
    }
    if (result.stopReason == "SAFETY" || result.stopReason == "PROHIBITED_CONTENT" || result.stopReason == "BLOCKLIST") {
        result.error = "The response was blocked (" + result.stopReason + ")";
        if (result.text.empty()) return result;
    }
    if (result.model.empty()) result.model = request.model;
    result.ok = !result.cancelled;
    return result;
}

bool GeminiProvider::ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>* cancel) {
    out.clear();
    std::string token;
    for (int page = 0; page < 20; page++) {
        std::string url = BaseUrl() + "/models?pageSize=1000";
        if (!token.empty()) url += "&pageToken=" + UrlEncode(token);
        HttpResponse r = Send("GET", url, Headers(), std::string(), cancel);
        if (!r.Ok()) {
            error = ErrorFromResponse(r);
            return false;
        }
        json root = ParseJsonOrEmpty(r.body);
        for (const auto& m : root.value("models", json::array())) {
            bool generates = false;
            for (const auto& method : m.value("supportedGenerationMethods", json::array())) {
                if (method == "generateContent") generates = true;
            }
            if (!generates) continue;
            ModelInfo info;
            info.id = m.value("name", std::string());
            if (StartsWith(info.id, "models/")) info.id = info.id.substr(7);
            if (info.id.empty()) continue;
            // Image, speech and embedding models share the list.
            std::string lower = ToLower(info.id);
            if (lower.find("embedding") != std::string::npos || lower.find("-tts") != std::string::npos ||
                lower.find("imagen") != std::string::npos || lower.find("image-generation") != std::string::npos) {
                continue;
            }
            info.name = m.value("displayName", info.id);
            info.description = m.value("description", std::string());
            info.contextLength = m.value("inputTokenLimit", int64_t{0});
            info.maxOutput = m.value("outputTokenLimit", int64_t{0});
            info.reasoning = m.value("thinking", false);
            info.vision = true;
            out.push_back(std::move(info));
        }
        token = root.value("nextPageToken", std::string());
        if (token.empty()) break;
    }
    return true;
}

AccountInfo GeminiProvider::GetAccountInfo(const std::atomic<bool>*) {
    AccountInfo info;
    info.ok = true;
    info.fetchedAt = NowSeconds();
    info.Add("Billing", "The Gemini API does not report credits or spend; see Google AI Studio or Cloud Billing");
    return info;
}

}  // namespace

std::unique_ptr<Provider> CreateGeminiProvider(const ProviderConfig& config) {
    return std::make_unique<GeminiProvider>(config);
}
