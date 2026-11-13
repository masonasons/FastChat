// Claude Code as a provider: `claude -p` with stream-json in and out. Its
// approval requests (control_request can_use_tool) become FastChat's approval
// dialog, its session id is kept so the next message resumes the session, and
// its rate_limit_event reports the plan's usage.

#include "core/providers/provider_impl.h"
#include "core/util.h"

#include <chrono>
#include <map>
#include <optional>
#include <set>

namespace {

bool IsCommandTool(const std::string& name) {
    return name == "Bash" || name == "PowerShell";
}

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A path shown relative to the folder it is in, when it is.
std::string Short(const std::string& path, const std::string& folder) {
    if (!folder.empty() && path.size() > folder.size() + 1 && ToLower(path.substr(0, folder.size())) == ToLower(folder)) {
        return path.substr(folder.size() + 1);
    }
    return path;
}

std::string Describe(const std::string& name, const json& input, const std::string& folder) {
    if (IsCommandTool(name)) return "Run: " + OneLine(Str(input, "command"), 200);
    if (name == "Write") return "Write " + Short(Str(input, "file_path"), folder);
    if (name == "Edit" || name == "MultiEdit" || name == "NotebookEdit") {
        std::string path = Str(input, "file_path");
        if (path.empty()) path = Str(input, "notebook_path");
        return "Edit " + Short(path, folder);
    }
    if (name == "Read") return "Read " + Short(Str(input, "file_path"), folder);
    if (name == "Glob") return "Find " + Str(input, "pattern");
    if (name == "Grep") return "Search for " + Str(input, "pattern");
    if (name == "WebFetch") return "Fetch " + Str(input, "url");
    if (name == "WebSearch") return "Search the web for " + Str(input, "query");
    if (name == "Task" || name == "Agent") return "Agent: " + Str(input, "description");
    if (name == "TodoWrite") return "Update the to-do list";
    if (name == "AskUserQuestion") {
        std::string asked;
        for (const auto& q : input.value("questions", json::array())) {
            if (q.is_object() && !Str(q, "question").empty()) asked += (asked.empty() ? "" : " / ") + Str(q, "question");
        }
        return "Ask: " + (asked.empty() ? std::string("a question") : asked);
    }
    return name + " " + OneLine(JsonDump(input), 100);
}

// An id for a message written to Claude Code, in a UUID's form, which it
// echoes back once it has taken the message in.
std::string MessageUuid() {
    std::string id = NewId();
    return id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" + id.substr(16, 4) + "-" + id.substr(20, 12);
}

std::string ResultText(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string text;
    if (content.is_array()) {
        for (const auto& block : content) {
            if (block.is_object() && block.value("type", "") == "text") {
                if (!text.empty()) text += "\n";
                text += block.value("text", "");
            } else if (block.is_object() && block.value("type", "") == "image") {
                text += "[image]";
            }
        }
    }
    return text;
}

Usage ReadUsage(const json& u) {
    Usage usage;
    if (!u.is_object()) return usage;
    usage.cachedInputTokens = u.value("cache_read_input_tokens", int64_t{0});
    usage.cacheWriteTokens = u.value("cache_creation_input_tokens", int64_t{0});
    usage.inputTokens = u.value("input_tokens", int64_t{0}) + usage.cachedInputTokens + usage.cacheWriteTokens;
    usage.outputTokens = u.value("output_tokens", int64_t{0});
    if (auto d = u.find("output_tokens_details"); d != u.end() && d->is_object()) {
        usage.reasoningTokens = d->value("thinking_tokens", int64_t{0});
    }
    return usage;
}

std::string FormatReset(int64_t when) {
    return when > 0 ? FormatRelativeTime(when) : std::string();
}

// AskUserQuestion's questions, from its input.
std::vector<AgentQuestion> ReadQuestions(const json& input) {
    std::vector<AgentQuestion> questions;
    for (const auto& q : input.value("questions", json::array())) {
        if (!q.is_object() || Str(q, "question").empty()) continue;
        AgentQuestion one;
        one.header = Str(q, "header");
        one.question = Str(q, "question");
        one.multiSelect = q.value("multiSelect", false);
        for (const auto& o : q.value("options", json::array())) {
            if (o.is_object() && !Str(o, "label").empty()) one.options.push_back({Str(o, "label"), Str(o, "description")});
        }
        questions.push_back(one);
    }
    return questions;
}

// A user message's content: its text alone, or with its images and PDFs
// first as content blocks (as the Messages API takes them).
json UserContent(const std::string& text, const std::vector<ImageData>& files) {
    if (files.empty()) return text;
    json content = json::array();
    for (const auto& f : files) {
        content.push_back({{"type", f.IsPdf() ? "document" : "image"},
                           {"source", {{"type", "base64"}, {"media_type", f.mediaType}, {"data", f.base64}}}});
    }
    content.push_back({{"type", "text"}, {"text", text.empty() ? std::string(" ") : text}});
    return content;
}

// After a compaction, Claude Code sends its summary as a user message that
// starts like this, and carries on from it.
bool IsCompactSummary(const std::string& text) {
    return StartsWith(text, "This session is being continued from a previous conversation");
}

// The summary itself: from its "Summary:" line, without the reminders
// Claude Code puts in for the model.
std::string CompactSummaryText(const std::string& text) {
    std::string s = text;
    for (size_t open; (open = s.find("<system-reminder>")) != std::string::npos;) {
        size_t close = s.find("</system-reminder>", open);
        s.erase(open, close == std::string::npos ? std::string::npos : close + 18 - open);
    }
    size_t at = s.find("Summary:");
    if (at != std::string::npos) s = s.substr(at + 8);
    return Trim(s);
}

class ClaudeCodeProvider : public Provider {
public:
    using Provider::Provider;

    bool IsAgent() const override { return true; }

    std::string Program() const {
        return m_config.baseUrl.empty() ? FindProgram("claude") : m_config.baseUrl;
    }

    ChatResult RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks, const std::atomic<bool>* cancel) override;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks, const std::atomic<bool>* cancel) override {
        AgentTurn turn;
        turn.request = request;
        turn.tools = false;
        AgentCallbacks cb;
        static_cast<StreamCallbacks&>(cb) = callbacks;
        return RunAgent(turn, cb, cancel);
    }

    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>*) override {
        if (Program().empty()) {
            error = "Claude Code was not found. Install it, or set its program in the provider's settings.";
            return false;
        }
        out = KnownClaudeModels();
        return true;
    }

    AccountInfo GetAccountInfo(const std::atomic<bool>*) override {
        AccountInfo info;
        info.fetchedAt = NowSeconds();
        std::string program = Program();
        if (program.empty()) {
            info.error = "Claude Code was not found.";
            return info;
        }
        info.Add("Program", program);
        std::string output, error;
        if (!CaptureProgram({program, "auth", "status"}, output, error, 20000)) {
            info.error = error;
            return info;
        }
        json status = json::parse(output, nullptr, false);
        if (status.is_object()) {
            info.Add("Logged in", status.value("loggedIn", false) ? "yes" : "no");
            if (status.contains("authMethod")) info.Add("Signed in with", Str(status, "authMethod"));
            if (status.contains("subscriptionType")) info.Add("Subscription", Str(status, "subscriptionType"));
            if (status.contains("planTier")) info.Add("Plan", Str(status, "planTier"));
            if (status.contains("orgName")) info.Add("Organization", Str(status, "orgName"));
            if (status.contains("loginExpiresAt")) info.Add("Login expires", Str(status, "loginExpiresAt"));
        } else {
            info.Add("Status", OneLine(output, 300));
        }
        if (!m_config.apiKey.empty()) info.Add("API key", "set: Claude Code bills this key instead of your plan");
        AccountInfo cached;
        if (AppProviderCache().GetAccount(m_config.id, cached) && !cached.summary.empty()) info.summary = cached.summary;
        info.ok = true;
        return info;
    }
};

ChatResult ClaudeCodeProvider::RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks,
                                        const std::atomic<bool>* cancel) {
    ChatResult result;
    std::string program = Program();
    if (program.empty()) {
        result.error = "Claude Code was not found. Install it, or set its program in the provider's settings.";
        return result;
    }
    std::vector<std::string> argv = {program, "-p", "--output-format", "stream-json", "--input-format", "stream-json",
                                     "--verbose", "--include-partial-messages", "--permission-prompt-tool", "stdio",
                                     "--replay-user-messages"};
    const ChatRequest& req = turn.request;
    if (!req.model.empty() && req.model != "default") {
        argv.push_back("--model");
        argv.push_back(req.model);
    }
    if (!req.reasoningEffort.empty()) {
        argv.push_back("--effort");
        argv.push_back(req.reasoningEffort == "none" ? "low" : req.reasoningEffort);
    }
    if (!Trim(req.systemPrompt).empty()) {
        argv.push_back("--append-system-prompt");
        argv.push_back(req.systemPrompt);
    }
    if (!turn.tools) {
        argv.push_back("--tools");
        argv.push_back("");
    } else {
        // "Ask" means FastChat's dialog decides, whatever mode the user's own
        // Claude Code settings start in (auto mode would approve on its own).
        // Their allow rules, CLAUDE.md and MCP servers still apply.
        argv.push_back("--permission-mode");
        argv.push_back(turn.approval == "auto" ? "acceptEdits" : "default");
    }
    if (!turn.resumeSession.empty()) {
        argv.push_back("--resume");
        argv.push_back(turn.resumeSession);
    }
    std::vector<std::pair<std::string, std::string>> env;
    if (!m_config.apiKey.empty()) env.emplace_back("ANTHROPIC_API_KEY", m_config.apiKey);
    std::string folder = turn.tools && !turn.folder.empty() ? turn.folder : AgentChatFolder();

    ChildProcess child;
    std::string error;
    if (!child.Start(argv, folder, env, error)) {
        result.error = error;
        return result;
    }
    // Messages written but not yet taken in: Claude Code echoes each one back
    // (--replay-user-messages) when it takes it in, during a turn or as a turn
    // of its own after the last one.
    std::set<std::string> waiting;
    auto send = [&](const std::string& text, const std::vector<ImageData>& files) {
        std::string uuid = MessageUuid();
        json msg = {{"type", "user"}, {"uuid", uuid}, {"message", {{"role", "user"}, {"content", UserContent(text, files)}}}};
        child.Write(JsonDump(msg) + "\n");
        waiting.insert(uuid);
    };
    send(AgentPrompt(turn), AgentAttachments(turn));

    std::string session = turn.resumeSession;
    Message pending;
    std::string pendingId;
    bool hasPending = false;
    std::map<std::string, std::pair<std::string, json>> calls;  // tool_use id -> name, input
    // Subagents: the tool call that started each (its id is their messages'
    // parent_tool_use_id), what it was described as, and which agent each of
    // their own tool calls belongs to.
    std::map<std::string, std::string> agentTask;   // starting tool_use id -> task id
    std::map<std::string, std::string> agentName;   // starting tool_use id -> description
    std::map<std::string, std::string> subCallOwner;  // a subagent's tool_use id -> starting tool_use id
    std::map<std::string, std::string> subCallName;   // a subagent's tool_use id -> description of the call
    auto activity = [&](ActivityEvent e) {
        if (callbacks.onActivity) callbacks.onActivity(e);
    };
    auto agentLine = [&](const std::string& parent, const std::string& line) {
        ActivityEvent e;
        auto task = agentTask.find(parent);
        e.id = task != agentTask.end() ? task->second : parent;
        e.toolUseId = parent;
        e.line = line;
        activity(e);
    };
    bool gotResult = false;
    std::optional<Message> compaction;  // a compaction's note, until its summary arrives
    auto flushCompaction = [&]() {
        if (!compaction) return;
        if (callbacks.onNote) callbacks.onNote(*compaction);
        compaction.reset();
    };

    auto flush = [&]() {
        if (!hasPending) return;
        hasPending = false;
        pending.providerData["claude_code"] = {{"session_id", session}};
        Message m = pending;
        if (callbacks.onMessage) callbacks.onMessage(m);
        for (const auto& call : pending.toolCalls) {
            if (callbacks.onToolRunning) callbacks.onToolRunning(call, Describe(call.name, ArgumentsToObject(call.arguments), folder));
        }
        pending = Message();
        pendingId.clear();
    };

    auto respond = [&](const std::string& requestId, const json& response) {
        json r = {{"type", "control_response"},
                  {"response", {{"subtype", "success"}, {"request_id", requestId}, {"response", response}}}};
        child.Write(JsonDump(r) + "\n");
    };

    // A message still waiting when a turn ends is run as another turn (Claude
    // Code's "dequeue"). Standard input must stay open for that turn: its
    // approvals come back that way, and fail ("Stream closed") once it is
    // closed. Its first output can take a while (a long context, or thinking
    // first), so wait long for it; a minute is only a fallback in case no turn
    // comes.
    bool stdinOpen = true;
    std::chrono::steady_clock::time_point closeAt{};
    // Background tasks (shells, agents) still running. When one ends after the
    // reply, Claude Code reports it in a turn of its own, whose approvals also
    // need standard input; so it stays open while any runs.
    std::set<std::string> running;
    bool betweenTurns = false;  // a reply has ended and no new one has started
    double totalCost = 0;
    bool anyCost = false;

    std::string line;
    for (;;) {
        if (cancel && cancel->load()) {
            child.Kill();
            result.cancelled = true;
            break;
        }
        if (stdinOpen && callbacks.takeQueued) {
            for (auto& m : callbacks.takeQueued()) {
                send(m.content, m.images);
                if (callbacks.onQueuedDelivered) callbacks.onQueuedDelivered(m);
                closeAt = {};
            }
        }
        ChildProcess::Read r = child.ReadLine(line, 100);
        if (r == ChildProcess::Read::Timeout) {
            if (stdinOpen && closeAt != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() >= closeAt) {
                child.CloseStdin();
                stdinOpen = false;
            }
            continue;
        }
        if (r == ChildProcess::Read::End) break;
        closeAt = {};
        json ev = json::parse(line, nullptr, false);
        if (!ev.is_object()) continue;
        std::string type = Str(ev, "type");
        bool topLevel = !ev.contains("parent_tool_use_id") || ev["parent_tool_use_id"].is_null();
        // A compaction's note goes out with the summary after it, or before
        // whatever else comes instead.
        bool summary = false;
        if (compaction && type == "user" && topLevel) {
            const json& content = ev.value("message", json::object()).value("content", json());
            summary = content.is_string() && IsCompactSummary(content.get<std::string>());
        }
        if (type == "user" && (ev.value("isReplay", false) || waiting.count(Str(ev, "uuid")))) {
            // One of ours, taken in; it is already in the conversation.
            waiting.erase(Str(ev, "uuid"));
            continue;
        }
        if (compaction && !summary) flushCompaction();
        if (type == "stream_event" || type == "assistant" || type == "user" || type == "control_request") betweenTurns = false;

        if (type == "system" && Str(ev, "subtype") == "init") {
            session = Str(ev, "session_id");
            result.model = Str(ev, "model");
        } else if (type == "system" && (Str(ev, "subtype") == "task_started" || Str(ev, "subtype") == "task_progress" ||
                                        Str(ev, "subtype") == "task_updated" || Str(ev, "subtype") == "task_notification")) {
            // Subagents and background shells.
            std::string subtype = Str(ev, "subtype");
            ActivityEvent e;
            e.id = Str(ev, "task_id");
            e.toolUseId = Str(ev, "tool_use_id");
            if (subtype == "task_started") {
                std::string taskType = Str(ev, "task_type");
                e.kind = taskType == "local_agent" ? "agent" : taskType == "local_bash" ? "shell" : taskType;
                e.description = Str(ev, "description");
                e.agentType = Str(ev, "subagent_type");
                e.prompt = Str(ev, "prompt");
                e.background = ev.value("is_backgrounded", false);
                e.status = "running";
                if (!e.toolUseId.empty()) {
                    agentTask[e.toolUseId] = e.id;
                    agentName[e.toolUseId] = e.description;
                }
            } else if (subtype == "task_progress") {
                e.progress = Str(ev, "description");
                e.lastTool = Str(ev, "last_tool_name");
                const json& u = ev.value("usage", json::object());
                e.tokens = u.value("total_tokens", int64_t{-1});
                e.toolUses = u.value("tool_uses", int64_t{-1});
                e.durationMs = u.value("duration_ms", int64_t{-1});
            } else if (subtype == "task_updated") {
                const json& patch = ev.value("patch", json::object());
                e.status = Str(patch, "status");
            } else {
                e.status = Str(ev, "status");
                e.outputFile = Str(ev, "output_file");
                e.summary = Str(ev, "summary");
            }
            if (subtype == "task_started") {
                running.insert(e.id);
            } else if (!e.status.empty() && e.status != "running" && e.status != "pending") {
                running.erase(e.id);
                // The last one ended between turns: its report turn should come soon.
                if (running.empty() && betweenTurns && waiting.empty() && stdinOpen) {
                    closeAt = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                }
            }
            activity(e);
        } else if (type == "system" && Str(ev, "subtype") == "status") {
            if (Str(ev, "status") == "compacting" && callbacks.onStatus) callbacks.onStatus("Compacting the conversation");
        } else if (type == "system" && Str(ev, "subtype") == "compact_boundary") {
            // Claude Code compacted its context (asked with /compact, or by itself).
            const json& meta = ev.value("compact_metadata", json::object());
            std::string text = std::string(Str(meta, "trigger") == "manual" ? "Compacted" : "Claude Code compacted the conversation");
            int64_t pre = meta.value("pre_tokens", int64_t{0}), post = meta.value("post_tokens", int64_t{0});
            if (pre) text += ": " + FormatTokensShort(pre) + " tokens down to " + FormatTokensShort(post);
            // Its summary comes next, as a user message; the note waits for it.
            flushCompaction();
            compaction = MakeNote(NoteKind::Note, text, {{"agentCompaction", true}, {"trigger", Str(meta, "trigger")}});
        } else if (summary) {
            compaction->providerData["fastchat"]["summary"] = CompactSummaryText(ev["message"]["content"].get<std::string>());
            flushCompaction();
        } else if (type == "stream_event" && topLevel) {
            const json& e = ev.value("event", json::object());
            std::string etype = Str(e, "type");
            if (etype == "content_block_start") {
                const json& block = e.value("content_block", json::object());
                if (Str(block, "type") == "tool_use" && callbacks.onToolCallStarted) callbacks.onToolCallStarted(Str(block, "name"));
            } else if (etype == "content_block_delta") {
                const json& d = e.value("delta", json::object());
                std::string dtype = Str(d, "type");
                if (dtype == "text_delta" && callbacks.onText) callbacks.onText(Str(d, "text"));
                if (dtype == "thinking_delta" && callbacks.onReasoning) callbacks.onReasoning(Str(d, "thinking"));
            }
        } else if (type == "assistant" && !topLevel) {
            std::string parent = Str(ev, "parent_tool_use_id");
            for (const auto& block : ev.value("message", json::object()).value("content", json::array())) {
                std::string btype = Str(block, "type");
                if (btype == "tool_use") {
                    std::string what = Describe(Str(block, "name"), block.value("input", json::object()), folder);
                    subCallOwner[Str(block, "id")] = parent;
                    subCallName[Str(block, "id")] = what;
                    agentLine(parent, what);
                } else if (btype == "text" && !Trim(Str(block, "text")).empty()) {
                    agentLine(parent, "Says: " + Str(block, "text"));
                }
            }
        } else if (type == "user" && !topLevel) {
            std::string parent = Str(ev, "parent_tool_use_id");
            const json& content = ev.value("message", json::object()).value("content", json());
            if (content.is_array()) {
                for (const auto& block : content) {
                    if (Str(block, "type") != "tool_result") continue;
                    std::string text = OneLine(ResultText(block.value("content", json())), 300);
                    agentLine(parent, std::string(block.value("is_error", false) ? "Failed: " : "Result: ") + text);
                }
            }
        } else if (type == "assistant" && topLevel) {
            const json& msg = ev.value("message", json::object());
            std::string id = Str(msg, "id");
            // One event per content block; blocks with the same id are one message.
            if (hasPending && id != pendingId) flush();
            if (!hasPending) {
                hasPending = true;
                pendingId = id;
                pending.role = Role::Assistant;
                pending.model = Str(msg, "model");
            }
            pending.usage = ReadUsage(msg.value("usage", json::object()));
            for (const auto& block : msg.value("content", json::array())) {
                std::string btype = Str(block, "type");
                if (btype == "text") {
                    if (!pending.content.empty()) pending.content += "\n\n";
                    pending.content += Str(block, "text");
                } else if (btype == "thinking") {
                    if (!pending.reasoning.empty()) pending.reasoning += "\n\n";
                    pending.reasoning += Str(block, "thinking");
                } else if (btype == "tool_use") {
                    ToolCall call{Str(block, "id"), Str(block, "name"), JsonDump(block.value("input", json::object()))};
                    calls[call.id] = {call.name, block.value("input", json::object())};
                    pending.toolCalls.push_back(call);
                }
            }
        } else if (type == "user" && topLevel) {
            flush();
            const json& msg = ev.value("message", json::object());
            const json& content = msg.value("content", json::array());
            if (!content.is_array()) continue;
            for (const auto& block : content) {
                if (Str(block, "type") != "tool_result") continue;
                Message tool;
                tool.role = Role::Tool;
                tool.toolCallId = Str(block, "tool_use_id");
                auto call = calls.find(tool.toolCallId);
                tool.toolName = call != calls.end() ? call->second.first : std::string("tool");
                tool.content = ResultText(block.value("content", json()));
                tool.toolError = block.value("is_error", false);
                std::string description = call != calls.end() ? Describe(call->second.first, call->second.second, folder)
                                                              : tool.toolName;
                // A command sent to the background: where its output goes.
                if (const json& extra = ev.value("tool_use_result", json()); extra.is_object() && extra.contains("backgroundTaskId")) {
                    ActivityEvent e;
                    e.id = Str(extra, "backgroundTaskId");
                    size_t at = tool.content.find("Output is being written to: ");
                    if (at != std::string::npos) {
                        std::string path = tool.content.substr(at + 28);
                        e.outputFile = Trim(path.substr(0, path.find_first_of("\r\n")));
                    }
                    if (call != calls.end()) e.prompt = Str(call->second.second, "command");
                    activity(e);
                }
                tool.reasoning = description + (tool.toolError ? " (failed)" : "");
                if (callbacks.onMessage) callbacks.onMessage(tool);
            }
        } else if (type == "control_request") {
            flush();
            const json& request = ev.value("request", json::object());
            std::string requestId = Str(ev, "request_id");
            if (!stdinOpen) {
                // Nothing can answer it; Claude Code has already given up on it.
            } else if (Str(request, "subtype") == "can_use_tool" && Str(request, "tool_name") == "AskUserQuestion") {
                // Questions for the user, whatever the approval setting: the
                // answers go back in the tool's input, keyed by question.
                json input = request.value("input", json::object());
                std::vector<AgentQuestion> questions = ReadQuestions(input);
                std::vector<std::string> answers;
                if (!questions.empty() && callbacks.askQuestions && callbacks.askQuestions(questions, answers)) {
                    json byQuestion = json::object();
                    for (size_t i = 0; i < questions.size(); i++) byQuestion[questions[i].question] = answers[i];
                    input["answers"] = byQuestion;
                    respond(requestId, {{"behavior", "allow"}, {"updatedInput", input}});
                } else {
                    respond(requestId, {{"behavior", "deny"}, {"message", "The user chose not to answer these questions. Ask them how to proceed."}});
                }
            } else if (Str(request, "subtype") == "can_use_tool") {
                std::string tool = Str(request, "tool_name");
                json input = request.value("input", json::object());
                bool allow = false;
                std::string what = IsCommandTool(tool) ? Str(input, "command") : Describe(tool, input, folder);
                // A subagent's request says which agent asks.
                auto owner = subCallOwner.find(Str(request, "tool_use_id"));
                if (owner != subCallOwner.end()) {
                    auto name = agentName.find(owner->second);
                    std::string agent = name != agentName.end() ? name->second : std::string("a subagent");
                    what = "Agent '" + agent + "' wants to: " + Describe(tool, input, folder);
                }
                if (turn.approval == "auto") allow = true;
                else if (turn.approval != "deny" && callbacks.approve) {
                    allow = callbacks.approve(what, IsCommandTool(tool) && owner == subCallOwner.end());
                }
                if (allow) respond(requestId, {{"behavior", "allow"}, {"updatedInput", input}});
                else respond(requestId, {{"behavior", "deny"}, {"message", "The user declined this. Ask them how to proceed."}});
            } else {
                json r = {{"type", "control_response"},
                          {"response", {{"subtype", "error"}, {"request_id", requestId}, {"error", "Not supported by FastChat"}}}};
                child.Write(JsonDump(r) + "\n");
            }
        } else if (type == "rate_limit_event") {
            const json& info = ev.value("rate_limit_info", json::object());
            std::vector<std::pair<std::string, std::string>> limits;
            std::string summary;
            limits.emplace_back("Plan status", Str(info, "status"));
            if (auto windows = info.find("unifiedWindows"); windows != info.end() && windows->is_object()) {
                for (auto& [name, w] : windows->items()) {
                    std::string label = ReplaceAll(name, "_", " ");
                    double used = w.value("utilization", -1.0);
                    std::string text = used >= 0 ? FormatPercent(used) + " used" : std::string("?");
                    int64_t resets = w.value("resetsAt", int64_t{0});
                    if (resets) text += ", resets " + FormatReset(resets);
                    limits.emplace_back("Plan usage, " + label, text);
                    if (summary.empty() && used >= 0) summary = "Plan " + FormatPercent(used) + " used (" + label + ")";
                }
            }
            if (info.value("isUsingOverage", false)) limits.emplace_back("Using extra usage", "yes");
            if (callbacks.onLimits) callbacks.onLimits(limits, summary);
            result.rateLimits = limits;
        } else if (type == "result") {
            gotResult = true;
            session = Str(ev, "session_id").empty() ? session : Str(ev, "session_id");
            if (hasPending) {
                // The whole run's cost goes on its last message.
                if (auto cost = ev.find("total_cost_usd"); cost != ev.end() && cost->is_number()) pending.usage.cost = cost->get<double>();
                flush();
            }
            result.usage = ReadUsage(ev.value("usage", json::object()));
            if (auto cost = ev.find("total_cost_usd"); cost != ev.end() && cost->is_number()) {
                totalCost += cost->get<double>();
                anyCost = true;
            }
            if (anyCost) result.usage.cost = totalCost;
            result.stopReason = Str(ev, "stop_reason");
            result.text = Str(ev, "result");
            if (ev.value("is_error", false) || StartsWith(Str(ev, "subtype"), "error")) {
                std::string why = Str(ev, "result");
                if (why.empty()) {
                    for (const auto& e : ev.value("errors", json::array())) {
                        if (e.is_string()) why += (why.empty() ? "" : "; ") + e.get<std::string>();
                    }
                }
                result.error = why.empty() ? "Claude Code stopped: " + Str(ev, "subtype") : why;
                result.errorDetails = "Claude Code reported: " + OneLine(line, 3000) + "\n";
            }
            betweenTurns = true;
            if (result.error.empty() && (!waiting.empty() || !running.empty())) {
                // A message of the user's, or a background task's report, is still
                // to come as a turn of its own. With tasks running, input stays
                // open until they end (Claude Code waits for them anyway).
                if (running.empty()) closeAt = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            } else if (stdinOpen) {
                child.CloseStdin();
                stdinOpen = false;
            }
        }
    }
    flushCompaction();
    flush();
    if (!result.cancelled && child.Wait(5000) < 0) child.Kill();
    result.providerData["claude_code"] = {{"session_id", session}};
    if (result.cancelled) return result;
    if (!gotResult && result.error.empty()) {
        std::string err = Trim(SanitizeUtf8(child.Stderr()));
        result.error = err.empty() ? std::string("Claude Code ended without finishing") : "Claude Code: " + OneLine(err, 400);
        result.errorDetails = "Program: " + program + "\nWhat it wrote to its error output:\n" +
                              (err.empty() ? std::string("(nothing)") : TruncateUtf8(err, 4000)) + "\n";
    }
    if (result.model.empty()) result.model = req.model;
    result.ok = result.error.empty();
    return result;
}

}  // namespace

std::unique_ptr<Provider> CreateClaudeCodeProvider(const ProviderConfig& config) {
    return std::make_unique<ClaudeCodeProvider>(config);
}
