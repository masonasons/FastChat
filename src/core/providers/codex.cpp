// The Codex CLI as a provider: `codex exec --json`, prompt on standard input.
// Codex cannot ask for approval in this mode, so code conversations run in its
// workspace-write sandbox (edits inside the folder, no network) and chats in
// read-only. Commands it runs are stored as run_command calls with their
// output and exit code, like FastChat's own.

#include "core/providers/provider_impl.h"
#include "core/util.h"

#include <algorithm>
#include <map>
#include <set>

namespace {

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

class CodexProvider : public Provider {
public:
    using Provider::Provider;

    bool IsAgent() const override { return true; }

    std::string Program() const { return m_config.baseUrl.empty() ? FindProgram("codex") : m_config.baseUrl; }

    ChatResult RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks, const std::atomic<bool>* cancel) override;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks, const std::atomic<bool>* cancel) override {
        AgentTurn turn;
        turn.request = request;
        AgentCallbacks cb;
        static_cast<StreamCallbacks&>(cb) = callbacks;
        return RunAgent(turn, cb, cancel);
    }

    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>*) override {
        std::string program = Program();
        if (program.empty()) {
            error = "Codex was not found. Install it, or set its program in the provider's settings.";
            return false;
        }
        std::string output;
        if (!CaptureProgram({program, "debug", "models"}, output, error, 30000)) {
            error = "Codex did not list its models (" + error + "). Type the model's name instead.";
            return false;
        }
        size_t start = output.find_first_of("[{");
        json root = start == std::string::npos ? json() : json::parse(output.substr(start), nullptr, false);
        const json* list = nullptr;
        if (root.is_array()) list = &root;
        else if (root.is_object()) {
            for (const char* key : {"models", "data", "items"}) {
                if (auto it = root.find(key); it != root.end() && it->is_array()) list = &*it;
            }
        }
        out.clear();
        if (list) {
            for (const auto& m : *list) {
                ModelInfo info;
                if (m.is_string()) info.id = m.get<std::string>();
                else if (m.is_object()) {
                    for (const char* key : {"slug", "id", "model", "name"}) {
                        if (info.id.empty()) info.id = Str(m, key);
                    }
                    info.name = Str(m, "display_name");
                    info.description = Str(m, "description");
                    info.contextLength = m.value("context_window", int64_t{0});
                }
                if (info.id.empty()) continue;
                if (info.name.empty()) info.name = info.id;
                info.reasoning = true;
                out.push_back(info);
            }
        }
        if (out.empty()) {
            error = "Codex's model list was not understood. Type the model's name instead.";
            return false;
        }
        return true;
    }

    AccountInfo GetAccountInfo(const std::atomic<bool>*) override {
        AccountInfo info;
        info.fetchedAt = NowSeconds();
        std::string program = Program();
        if (program.empty()) {
            info.error = "Codex was not found.";
            return info;
        }
        info.Add("Program", program);
        std::string output, error;
        if (!CaptureProgram({program, "login", "status"}, output, error, 20000)) {
            info.Add("Login", "not logged in, or unknown: " + error);
        } else {
            for (const auto& line : SplitLines(output)) {
                if (!Trim(line).empty()) info.Add("Login", Trim(line));
            }
        }
        if (!m_config.apiKey.empty()) info.Add("API key", "set: Codex bills this key");
        info.Add("Plan usage", "Codex does not report plan limits in this mode");
        info.ok = true;
        return info;
    }
};

ChatResult CodexProvider::RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks, const std::atomic<bool>* cancel) {
    ChatResult result;
    std::string program = Program();
    if (program.empty()) {
        result.error = "Codex was not found. Install it, or set its program in the provider's settings.";
        return result;
    }
    std::string folder = turn.tools && !turn.folder.empty() ? turn.folder : AgentChatFolder();
    const ChatRequest& req = turn.request;
    std::vector<std::string> argv = {program, "exec", "--json", "--skip-git-repo-check", "-C", folder, "--sandbox",
                                     turn.tools && turn.approval != "deny" ? "workspace-write" : "read-only"};
    if (!req.model.empty() && req.model != "default") {
        argv.push_back("-m");
        argv.push_back(req.model);
    }
    if (!req.reasoningEffort.empty()) {
        std::string effort = req.reasoningEffort == "none" ? "minimal" : req.reasoningEffort == "max" ? "xhigh" : req.reasoningEffort;
        argv.push_back("-c");
        argv.push_back("model_reasoning_effort=" + effort);
    }
    if (!turn.resumeSession.empty()) {
        argv.push_back("resume");
        argv.push_back(turn.resumeSession);
    }
    argv.push_back("-");  // the prompt comes on standard input
    std::vector<std::pair<std::string, std::string>> env;
    if (!m_config.apiKey.empty()) env.emplace_back("CODEX_API_KEY", m_config.apiKey);

    ChildProcess child;
    std::string error;
    if (!child.Start(argv, folder, env, error)) {
        result.error = error;
        return result;
    }
    std::string prompt = AgentPrompt(turn);
    if (!Trim(req.systemPrompt).empty()) prompt = "Instructions: " + req.systemPrompt + "\n\n" + prompt;
    child.Write(prompt);
    child.CloseStdin();

    std::string session = turn.resumeSession;
    std::string reasoning;          // reasoning items go with the next message
    std::map<std::string, size_t> streamed;  // agent_message id -> characters already sent
    std::set<std::string> startedCommands;
    Message pending;
    bool hasPending = false;
    bool finished = false;

    auto post = [&](Message& m) {
        m.providerData["codex"] = {{"thread_id", session}};
        if (callbacks.onMessage) callbacks.onMessage(m);
    };
    auto flush = [&]() {
        if (!hasPending) return;
        hasPending = false;
        post(pending);
        pending = Message();
    };
    auto assistantWithCall = [&](const std::string& id, const std::string& name, const json& args) {
        flush();
        Message m;
        m.role = Role::Assistant;
        m.reasoning = reasoning;
        reasoning.clear();
        m.toolCalls.push_back({id, name, JsonDump(args)});
        post(m);
        return m.toolCalls.back();
    };
    auto toolResult = [&](const std::string& id, const std::string& name, const std::string& content, bool failed,
                          const std::string& summary) {
        Message t;
        t.role = Role::Tool;
        t.toolCallId = id;
        t.toolName = name;
        t.content = SanitizeUtf8(content);
        t.toolError = failed;
        t.reasoning = summary;
        post(t);
    };

    std::string line;
    for (;;) {
        if (cancel && cancel->load()) {
            child.Kill();
            result.cancelled = true;
            break;
        }
        ChildProcess::Read r = child.ReadLine(line, 100);
        if (r == ChildProcess::Read::Timeout) continue;
        if (r == ChildProcess::Read::End) break;
        json ev = json::parse(line, nullptr, false);
        if (!ev.is_object()) continue;
        std::string type = Str(ev, "type");
        if (type == "thread.started") {
            session = Str(ev, "thread_id");
        } else if (type == "item.started" || type == "item.updated" || type == "item.completed") {
            const json& item = ev.value("item", json::object());
            std::string itype = Str(item, "type");
            std::string id = Str(item, "id");
            bool done = type == "item.completed";
            if (itype == "agent_message") {
                std::string text = Str(item, "text");
                size_t& sent = streamed[id];
                if (text.size() > sent && callbacks.onText) callbacks.onText(text.substr(sent));
                sent = std::max(sent, text.size());
                if (done) {
                    flush();
                    pending = Message();
                    pending.role = Role::Assistant;
                    pending.content = text;
                    pending.reasoning = reasoning;
                    reasoning.clear();
                    hasPending = true;
                }
            } else if (itype == "reasoning") {
                if (done) {
                    std::string text = Str(item, "text");
                    if (callbacks.onReasoning) callbacks.onReasoning(text);
                    reasoning += (reasoning.empty() ? "" : "\n\n") + text;
                }
            } else if (itype == "command_execution") {
                std::string command = Str(item, "command");
                if (type == "item.started") {
                    startedCommands.insert(id);
                    ToolCall call = assistantWithCall(id, "run_command", {{"command", command}});
                    if (callbacks.onToolRunning) callbacks.onToolRunning(call, "Run: " + OneLine(command, 200));
                } else if (done) {
                    std::string status = Str(item, "status");
                    std::string output = Str(item, "aggregated_output");
                    int64_t code = item.contains("exit_code") && item["exit_code"].is_number() ? item["exit_code"].get<int64_t>() : -1;
                    if (status == "declined") output += "\n(Codex's sandbox declined this command.)";
                    if (code >= 0) output += "\nExit code: " + std::to_string(code);
                    bool failed = status == "failed" || status == "declined" || code > 0;
                    // A command Codex reports only when finished still needs its call.
                    if (!startedCommands.count(id)) assistantWithCall(id, "run_command", {{"command", command}});
                    toolResult(id, "run_command", output, failed,
                               "Ran " + OneLine(command, 80) + (code >= 0 ? ", exit code " + std::to_string(code) : ""));
                }
            } else if (itype == "file_change" && done) {
                std::string list, summary;
                int count = 0;
                for (const auto& c : item.value("changes", json::array())) {
                    list += Str(c, "kind") + ": " + Str(c, "path") + "\n";
                    summary += (count++ ? ", " : "") + PathToUtf8(PathFromUtf8(Str(c, "path")).filename());
                }
                assistantWithCall(id, "apply_patch", {{"changes", item.value("changes", json::array())}});
                toolResult(id, "apply_patch", list, Str(item, "status") == "failed", "Changed " + summary);
            } else if (itype == "mcp_tool_call" && done) {
                std::string name = Str(item, "server") + "." + Str(item, "tool");
                assistantWithCall(id, name, item.value("arguments", json::object()));
                std::string content = item.contains("result") && !item["result"].is_null() ? JsonDump(item["result"]) : std::string();
                if (item.contains("error") && item["error"].is_object()) content += Str(item["error"], "message");
                toolResult(id, name, content, Str(item, "status") == "failed", "Used " + name);
            } else if (itype == "web_search" && done) {
                std::string query = Str(item, "query");
                assistantWithCall(id, "web_search", {{"query", query}});
                toolResult(id, "web_search", item.contains("results") ? JsonDump(item["results"]) : std::string(), false,
                           "Searched the web for " + query);
            }
        } else if (type == "turn.completed") {
            const json& u = ev.value("usage", json::object());
            result.usage.cachedInputTokens = u.value("cached_input_tokens", int64_t{0});
            result.usage.cacheWriteTokens = u.value("cache_write_input_tokens", int64_t{0});
            result.usage.inputTokens = u.value("input_tokens", int64_t{0});
            result.usage.outputTokens = u.value("output_tokens", int64_t{0}) + u.value("reasoning_output_tokens", int64_t{0});
            result.usage.reasoningTokens = u.value("reasoning_output_tokens", int64_t{0});
            if (hasPending) pending.usage = result.usage;
            flush();
            finished = true;
        } else if (type == "turn.failed") {
            result.error = Str(ev.value("error", json::object()), "message");
            if (result.error.empty()) result.error = "Codex could not finish";
        } else if (type == "error") {
            result.error = Str(ev, "message");
        }
    }
    flush();
    if (!result.cancelled && child.Wait(5000) < 0) child.Kill();
    result.providerData["codex"] = {{"thread_id", session}};
    if (result.cancelled) return result;
    if (!finished && result.error.empty()) {
        std::string err = Trim(SanitizeUtf8(child.Stderr()));
        result.error = err.empty() ? std::string("Codex ended without finishing") : "Codex: " + OneLine(err, 400);
    }
    if (!result.error.empty()) {
        std::string err = Trim(SanitizeUtf8(child.Stderr()));
        result.errorDetails = "Program: " + program + "\nWhat it wrote to its error output:\n" +
                              (err.empty() ? std::string("(nothing)") : TruncateUtf8(err, 4000)) + "\n";
    }
    if (result.model.empty()) result.model = req.model;
    result.ok = result.error.empty();
    return result;
}

}  // namespace

std::unique_ptr<Provider> CreateCodexProvider(const ProviderConfig& config) {
    return std::make_unique<CodexProvider>(config);
}
