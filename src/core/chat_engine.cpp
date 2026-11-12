#include "core/chat_engine.h"
#include "core/util.h"

#include <chrono>

ChatRunner::ChatRunner(std::function<void(std::function<void()>)> post) : m_post(std::move(post)) {}

ChatRunner::~ChatRunner() {
    m_listener = nullptr;
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}

bool ChatRunner::Start(ChatTurn turn) {
    if (m_running) return false;
    if (m_thread.joinable()) m_thread.join();
    m_cancel = false;
    m_running = true;
    m_thread = std::thread([this, turn = std::move(turn)]() mutable { Run(std::move(turn)); });
    return true;
}

void ChatRunner::Stop() {
    m_cancel = true;
}

void ChatRunner::Post(std::function<void(ChatListener&)> fn) {
    std::weak_ptr<ChatRunner> weak = weak_from_this();
    m_post([weak, fn = std::move(fn)]() {
        if (auto self = weak.lock()) {
            if (self->m_listener) fn(*self->m_listener);
        }
    });
}

bool ChatRunner::AskApproval(const std::string& command, bool isCommand) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto answered = std::make_shared<std::atomic<bool>>(false);
    std::future<bool> future = promise->get_future();
    std::weak_ptr<ChatRunner> weak = weak_from_this();
    m_post([weak, command, isCommand, promise, answered]() {
        auto self = weak.lock();
        auto answer = [promise, answered](bool yes) {
            if (!answered->exchange(true)) promise->set_value(yes);
        };
        if (!self || !self->m_listener || self->m_cancel) {
            answer(false);
            return;
        }
        self->m_listener->OnApproveCommand(command, isCommand, answer);
    });
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (m_cancel) {
            if (!answered->exchange(true)) promise->set_value(false);
            break;
        }
    }
    return future.get();
}

void ChatRunner::Run(ChatTurn turn) {
    // Nothing that goes wrong here may end the program: report it instead.
    std::string problem;
    try {
        RunTurn(std::move(turn));
        return;
    } catch (const std::exception& e) {
        problem = e.what();
    } catch (...) {
        problem = "unknown exception";
    }
    AppendLog("Chat worker: " + problem);
    m_running = false;
    Post([problem](ChatListener& l) {
        l.OnFinished(ChatListener::Outcome::Failed, "Something went wrong in FastChat: " + problem);
    });
}

void ChatRunner::RunAgentTurn(ChatTurn turn, Provider& provider) {
    AgentTurn agent;
    agent.request = turn.request;
    agent.folder = turn.toolContext.root;
    agent.tools = turn.tools;
    agent.approval = turn.toolContext.commandApproval;
    // Continue the agent's own session when nothing but new messages from the
    // user came after its last reply; otherwise start afresh with the history.
    const auto& messages = turn.request.messages;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        const Message& m = messages[i];
        if (m.role == Role::User) continue;
        if (m.role == Role::Assistant && m.providerId == turn.provider.id) {
            for (const char* kind : {"claude_code", "codex"}) {
                if (auto pd = m.providerData.find(kind); pd != m.providerData.end() && pd->is_object()) {
                    for (const char* key : {"session_id", "thread_id"}) {
                        if (auto id = pd->find(key); id != pd->end() && id->is_string()) agent.resumeSession = id->get<std::string>();
                    }
                }
            }
        }
        break;
    }

    AgentCallbacks callbacks;
    callbacks.onText = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnTextDelta(t); }); };
    callbacks.onReasoning = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnReasoningDelta(t); }); };
    callbacks.onToolCallStarted = [this](const std::string& n) { Post([n](ChatListener& l) { l.OnToolCallStarted(n); }); };
    std::string providerId = turn.provider.id;
    std::string model = turn.request.model;
    callbacks.onMessage = [this, providerId, model](Message& m) {
        m.providerId = providerId;
        if (m.role == Role::Assistant && m.model.empty()) m.model = model;
        m.createdAt = NowSeconds();
        Message copy = m;
        Post([copy](ChatListener& l) mutable { l.OnMessage(copy); });
    };
    callbacks.onToolRunning = [this](const ToolCall& call, const std::string& description) {
        Post([call, description](ChatListener& l) { l.OnToolRunning(call, description); });
    };
    callbacks.approve = [this](const std::string& what, bool isCommand) { return AskApproval(what, isCommand); };
    callbacks.onLimits = [providerId](const std::vector<std::pair<std::string, std::string>>& limits, const std::string& summary) {
        AppProviderCache().SetRateLimits(providerId, limits);
        if (!summary.empty()) AppProviderCache().SetSummary(providerId, summary);
    };

    ChatResult result = provider.RunAgent(agent, callbacks, &m_cancel);
    m_running = false;
    ChatListener::Outcome outcome = result.cancelled || m_cancel ? ChatListener::Outcome::Stopped
                                    : result.ok                  ? ChatListener::Outcome::Done
                                                                 : ChatListener::Outcome::Failed;
    std::string error = outcome == ChatListener::Outcome::Failed ? result.error : std::string();
    Post([outcome, error](ChatListener& l) { l.OnFinished(outcome, error); });
}

void ChatRunner::RunTurn(ChatTurn turn) {
    std::unique_ptr<Provider> provider = CreateProvider(turn.provider);
    if (provider->IsAgent()) {
        RunAgentTurn(std::move(turn), *provider);
        return;
    }
    ModelInfo modelInfo;
    AppProviderCache().FindModel(turn.provider.id, turn.request.model, modelInfo);

    if (turn.tools) {
        turn.request.tools = CodeToolDefinitions();
        turn.toolContext.cancel = &m_cancel;
        turn.toolContext.approveCommand = [this](const std::string& command) { return AskApproval(command, true); };
    }

    StreamCallbacks callbacks;
    callbacks.onText = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnTextDelta(t); }); };
    callbacks.onReasoning = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnReasoningDelta(t); }); };
    callbacks.onToolCallStarted = [this](const std::string& n) { Post([n](ChatListener& l) { l.OnToolCallStarted(n); }); };

    auto finish = [this](ChatListener::Outcome outcome, const std::string& error) {
        m_running = false;
        Post([outcome, error](ChatListener& l) { l.OnFinished(outcome, error); });
    };

    for (int step = 0; step < std::max(1, turn.maxSteps); step++) {
        auto started = std::chrono::steady_clock::now();
        ChatResult result = provider->Chat(turn.request, callbacks, &m_cancel);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        AppProviderCache().SetRateLimits(turn.provider.id, result.rateLimits);

        Message reply;
        reply.role = Role::Assistant;
        reply.content = result.text;
        reply.reasoning = result.reasoning;
        reply.toolCalls = result.toolCalls;
        reply.providerId = turn.provider.id;
        reply.model = result.model.empty() ? turn.request.model : result.model;
        reply.usage = result.usage;
        ApplyPricing(modelInfo, reply.usage);
        reply.durationMs = elapsed.count();
        reply.stopReason = result.cancelled ? "cancelled" : result.stopReason;
        reply.providerData = result.providerData;
        reply.createdAt = NowSeconds();

        bool hasContent = !reply.content.empty() || !reply.reasoning.empty();
        if (result.cancelled || m_cancel) {
            // Keep what arrived, without tool calls that will never run.
            if (hasContent) {
                reply.toolCalls.clear();
                Post([reply](ChatListener& l) mutable { l.OnMessage(reply); });
            }
            finish(ChatListener::Outcome::Stopped, std::string());
            return;
        }
        if (!result.ok) {
            if (hasContent) {
                reply.toolCalls.clear();
                Post([reply](ChatListener& l) mutable { l.OnMessage(reply); });
            }
            finish(ChatListener::Outcome::Failed, result.error.empty() ? "The request failed" : result.error);
            return;
        }

        Post([reply](ChatListener& l) mutable { l.OnMessage(reply); });
        turn.request.messages.push_back(reply);

        if (reply.toolCalls.empty() || !turn.tools) {
            if (result.stopReason == "max_tokens" || result.stopReason == "length" || result.stopReason == "MAX_TOKENS") {
                finish(ChatListener::Outcome::Done, "The response was cut off at the output token limit.");
            } else {
                finish(ChatListener::Outcome::Done, std::string());
            }
            return;
        }

        for (const auto& call : reply.toolCalls) {
            if (m_cancel) break;
            std::string description = DescribeToolCall(call);
            Post([call, description](ChatListener& l) { l.OnToolRunning(call, description); });
            ToolOutcome outcome = RunTool(call, turn.toolContext);
            Message toolMsg;
            toolMsg.role = Role::Tool;
            toolMsg.toolCallId = call.id;
            toolMsg.toolName = call.name;
            toolMsg.content = outcome.output;
            toolMsg.toolError = outcome.error;
            toolMsg.reasoning = outcome.summary;  // the one-line summary, shown in the list
            toolMsg.providerId = turn.provider.id;
            toolMsg.createdAt = NowSeconds();
            Post([toolMsg](ChatListener& l) mutable { l.OnMessage(toolMsg); });
            turn.request.messages.push_back(toolMsg);
        }
        if (m_cancel) {
            // Every call needs a result before the next request; answer the rest.
            for (const auto& call : reply.toolCalls) {
                bool answered = false;
                for (const auto& m : turn.request.messages) {
                    if (m.role == Role::Tool && m.toolCallId == call.id) answered = true;
                }
                if (!answered) {
                    Message toolMsg;
                    toolMsg.role = Role::Tool;
                    toolMsg.toolCallId = call.id;
                    toolMsg.toolName = call.name;
                    toolMsg.content = "Stopped by the user before this ran.";
                    toolMsg.toolError = true;
                    toolMsg.reasoning = "Not run";
                    toolMsg.providerId = turn.provider.id;
                    Post([toolMsg](ChatListener& l) mutable { l.OnMessage(toolMsg); });
                }
            }
            finish(ChatListener::Outcome::Stopped, std::string());
            return;
        }
    }
    finish(ChatListener::Outcome::Failed,
           "Stopped after " + std::to_string(turn.maxSteps) + " tool steps. Send a message to let it continue.");
}

std::string GenerateTitle(const ProviderConfig& provider, const std::string& model, const std::string& userText,
                          const std::string& assistantText, std::string& error) {
    auto p = CreateProvider(provider);
    ChatRequest request;
    request.model = model;
    request.stream = true;
    request.maxTokens = 0;
    request.systemPrompt =
        "You name conversations. Reply with only a short title (2 to 6 words) for the conversation below: no quotes, "
        "no punctuation at the end, no explanation.";
    Message m;
    m.role = Role::User;
    m.content = "User: " + TruncateUtf8(userText, 4000) + "\n\nAssistant: " + TruncateUtf8(assistantText, 4000);
    request.messages.push_back(m);
    // Thinking is wasted on a title.
    request.reasoningEffort = provider.kind == "anthropic" || provider.kind == "gemini" ? "low" : "";
    ChatResult r = p->Chat(request, StreamCallbacks(), nullptr);
    if (!r.ok) {
        error = r.error;
        return std::string();
    }
    std::string title = OneLine(Trim(r.text));
    // Strip quotes, "Title:" and trailing periods the model added anyway.
    if (StartsWith(ToLower(title), "title:")) title = Trim(title.substr(6));
    while (!title.empty() && (title.front() == '"' || title.front() == '\'' || title.front() == '*' || title.front() == '#')) {
        title.erase(0, 1);
    }
    while (!title.empty() && (title.back() == '"' || title.back() == '\'' || title.back() == '.' || title.back() == '*')) {
        title.pop_back();
    }
    return TruncateUtf8(Trim(title), 120);
}

ChatResult QuickChat(const ProviderConfig& provider, const std::string& model, const std::string& prompt,
                     const std::atomic<bool>* cancel) {
    auto p = CreateProvider(provider);
    ChatRequest request;
    request.model = model;
    Message m;
    m.role = Role::User;
    m.content = prompt;
    request.messages.push_back(m);
    return p->Chat(request, StreamCallbacks(), cancel);
}
