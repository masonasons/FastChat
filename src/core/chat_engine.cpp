#include "core/chat_engine.h"
#include "core/util.h"

#include "core/platform.h"

#include <chrono>
#include <thread>

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

bool ChatRunner::AskQuestions(const std::vector<AgentQuestion>& questions, std::vector<std::string>& answers) {
    using Reply = std::pair<bool, std::vector<std::string>>;
    auto promise = std::make_shared<std::promise<Reply>>();
    auto replied = std::make_shared<std::atomic<bool>>(false);
    std::future<Reply> future = promise->get_future();
    std::weak_ptr<ChatRunner> weak = weak_from_this();
    m_post([weak, questions, promise, replied]() {
        auto self = weak.lock();
        auto answer = [promise, replied](bool answered, std::vector<std::string> list) {
            if (!replied->exchange(true)) promise->set_value({answered, std::move(list)});
        };
        if (!self || !self->m_listener || self->m_cancel) {
            answer(false, {});
            return;
        }
        self->m_listener->OnAskQuestions(questions, answer);
    });
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (m_cancel) {
            if (!replied->exchange(true)) promise->set_value({false, {}});
            break;
        }
    }
    Reply reply = future.get();
    if (!reply.first || reply.second.size() != questions.size()) return false;
    answers = std::move(reply.second);
    return true;
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

void LimitImages(std::vector<Message>& messages, int keep) {
    // Screenshots in tool results only: what the user attached always goes.
    int seen = 0;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->images.empty() || it->role != Role::Tool) continue;
        if (seen++ < keep) continue;
        it->images.clear();
        it->content += "\n(An earlier screenshot was here; it was left out to save space.)";
    }
}

void ChatRunner::RunAgentTurn(ChatTurn turn, Provider& provider) {
    AgentTurn agent;
    agent.request = turn.request;
    agent.folder = turn.toolContext.root;
    agent.tools = turn.tools;
    agent.approval = turn.toolContext.commandApproval;
    // Continue the agent's own session when nothing but new messages from the
    // user came after its last reply; otherwise start afresh with the history.
    // Its compactions (/compact gets no reply) continue the same session.
    const auto& messages = turn.request.messages;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        const Message& m = messages[i];
        if (m.role == Role::User || IsAgentCompactionNote(m)) continue;
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
    callbacks.askQuestions = [this](const std::vector<AgentQuestion>& questions, std::vector<std::string>& answers) {
        return AskQuestions(questions, answers);
    };
    callbacks.onLimits = [providerId](const std::vector<std::pair<std::string, std::string>>& limits, const std::string& summary) {
        AppProviderCache().SetRateLimits(providerId, limits);
        if (!summary.empty()) AppProviderCache().SetSummary(providerId, summary);
    };

    // Messages sent meanwhile: Claude Code takes them while it works; any left
    // over (and all of Codex's) go in a follow-up turn of the same session.
    callbacks.takeQueued = [this]() { return TakeQueued(); };
    callbacks.onQueuedDelivered = [this](Message& m) {
        m.createdAt = NowSeconds();
        Message copy = m;
        Post([copy](ChatListener& l) mutable { l.OnQueuedDelivered(copy); });
    };
    callbacks.onActivity = [this](const ActivityEvent& event) {
        Post([event](ChatListener& l) { l.OnActivity(event); });
    };
    callbacks.onStatus = [this](const std::string& text) {
        Post([text](ChatListener& l) { l.OnStatus(text); });
    };
    callbacks.onNote = [this, providerId](Message& note) {
        note.providerId = providerId;
        note.createdAt = NowSeconds();
        Message copy = note;
        Post([copy](ChatListener& l) mutable { l.OnMessage(copy); });
    };

    ChatResult result;
    for (;;) {
        result = provider.RunAgent(agent, callbacks, &m_cancel);
        if (!result.ok || result.cancelled || m_cancel) break;
        std::vector<Message> queued = TakeQueued();
        if (queued.empty()) break;
        for (auto& m : queued) {
            callbacks.onQueuedDelivered(m);
            agent.request.messages.push_back(m);
        }
        for (const char* kind : {"claude_code", "codex"}) {
            if (auto pd = result.providerData.find(kind); pd != result.providerData.end() && pd->is_object()) {
                for (const char* key : {"session_id", "thread_id"}) {
                    if (auto id = pd->find(key); id != pd->end() && id->is_string() && !id->get<std::string>().empty()) {
                        agent.resumeSession = id->get<std::string>();
                    }
                }
            }
        }
    }
    m_running = false;
    ChatListener::Outcome outcome = result.cancelled || m_cancel ? ChatListener::Outcome::Stopped
                                    : result.ok                  ? ChatListener::Outcome::Done
                                                                 : ChatListener::Outcome::Failed;
    std::string error = outcome == ChatListener::Outcome::Failed ? result.error : std::string();
    if (outcome == ChatListener::Outcome::Failed) {
        std::string details = result.errorDetails.empty() ? "Provider: " + turn.provider.name + "\n" : result.errorDetails;
        AppendLog("Agent failed (" + turn.provider.name + "): " + error);
        Post([error, details](ChatListener& l) { l.OnProblem(error, details); });
    }
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

    // Computer use: the desktop lives on this thread for the whole turn.
    std::unique_ptr<Desktop> desktop;
    ComputerContext computer;
    if (turn.computer) {
        desktop = Desktop::Create();
        std::string why = desktop ? desktop->Unavailable() : std::string("Computer use works on Windows and the Mac only, so far.");
        if (!why.empty()) {
            m_running = false;
            Post([why](ChatListener& l) { l.OnFinished(ChatListener::Outcome::Failed, why); });
            return;
        }
        computer.desktop = desktop.get();
        computer.vision = turn.vision;
        computer.approval = turn.toolContext.commandApproval;
        computer.cancel = &m_cancel;
        computer.approve = [this](const std::string& what) { return AskApproval(what, false); };
        turn.request.tools = ComputerToolDefinitions(turn.vision);
        std::string extra = turn.request.systemPrompt;
        turn.request.systemPrompt = ComputerSystemPrompt(turn.vision, desktop->ScreenDescription());
        if (!Trim(extra).empty()) turn.request.systemPrompt += "\n" + extra;
    }
    if (turn.tools) {
        turn.request.tools = CodeToolDefinitions();
        turn.toolContext.cancel = &m_cancel;
        turn.toolContext.approveCommand = [this](const std::string& command) { return AskApproval(command, true); };
    }

    StreamCallbacks callbacks;
    callbacks.onText = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnTextDelta(t); }); };
    callbacks.onReasoning = [this](const std::string& t) { Post([t](ChatListener& l) { l.OnReasoningDelta(t); }); };
    callbacks.onToolCallStarted = [this](const std::string& n) { Post([n](ChatListener& l) { l.OnToolCallStarted(n); }); };

    auto finish = [this, &turn](ChatListener::Outcome outcome, const std::string& error,
                                const std::string& details = std::string()) {
        m_running = false;
        if (outcome == ChatListener::Outcome::Failed) {
            AppendLog("Request failed (" + turn.provider.name + ", " + turn.request.model + "): " + error +
                      (details.empty() ? std::string() : " | " + OneLine(details, 2000)));
            Post([error, details](ChatListener& l) { l.OnProblem(error, details); });
        }
        Post([outcome, error](ChatListener& l) { l.OnFinished(outcome, error); });
    };

    // Compaction: summarize what came before the new messages, then go on
    // with the summary in their place (or stop there when that is all).
    if (turn.compactFirst || turn.compactOnly) {
        auto& messages = turn.request.messages;
        size_t firstNew = messages.size();
        if (!turn.compactOnly) {
            while (firstNew > 0 && messages[firstNew - 1].role == Role::User) firstNew--;
        }
        std::vector<Message> older(messages.begin(), messages.begin() + firstNew);
        std::vector<Message> newer(messages.begin() + firstNew, messages.end());
        Post([](ChatListener& l) { l.OnStatus("Compacting the conversation"); });
        Message summary;
        ChatResult failure;
        if (!older.empty() && SummarizeConversation(*provider, turn.request.model, older, turn.compactThrough, &m_cancel,
                                                    summary, failure)) {
            summary.providerId = turn.provider.id;
            summary.model = turn.request.model;
            Message saved = summary;
            Post([saved](ChatListener& l) mutable { l.OnMessage(saved); });
            Message context = summary;
            context.providerData = json();
            messages = {context};
            messages.insert(messages.end(), newer.begin(), newer.end());
        } else if (m_cancel) {
            finish(ChatListener::Outcome::Stopped, std::string());
            return;
        } else if (turn.compactOnly) {
            finish(ChatListener::Outcome::Failed, "Compaction failed: " + failure.error, failure.errorDetails);
            return;
        } else {
            std::string error = "Compaction failed, so the whole conversation was sent: " + failure.error;
            std::string details = failure.errorDetails;
            Post([error, details](ChatListener& l) { l.OnProblem(error, details); });
        }
        if (turn.compactOnly) {
            finish(ChatListener::Outcome::Done, std::string());
            return;
        }
    }

    // Messages the user sent while this ran go in at the next step.
    auto deliverQueued = [this, &turn]() {
        std::vector<Message> queued = TakeQueued();
        for (auto& m : queued) {
            m.createdAt = NowSeconds();
            Message copy = m;
            Post([copy](ChatListener& l) mutable { l.OnQueuedDelivered(copy); });
            turn.request.messages.push_back(m);
        }
        return !queued.empty();
    };

    for (int step = 0; step < std::max(1, turn.maxSteps); step++) {
        auto started = std::chrono::steady_clock::now();
        // Only the latest screenshots go along; older ones would fill the context.
        ChatRequest sending = turn.request;
        LimitImages(sending.messages, 2);
        // Rate limits, server errors and dropped connections are tried again,
        // as long as nothing of the reply has arrived yet.
        ChatResult result;
        for (int attempt = 0;; attempt++) {
            result = provider->Chat(sending, callbacks, &m_cancel);
            bool arrived = !result.text.empty() || !result.reasoning.empty() || !result.toolCalls.empty();
            if (result.ok || m_cancel || arrived || attempt >= 2 || !result.Retriable()) break;
            int seconds = attempt == 0 ? 2 : 6;
            AppendLog("Retrying (" + turn.provider.name + ", " + turn.request.model + "): " + result.error);
            std::string status = "Trying again in " + std::to_string(seconds) + " seconds: " + result.error;
            Post([status](ChatListener& l) { l.OnStatus(status); });
            for (int i = 0; i < seconds * 10 && !m_cancel; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (m_cancel) break;
        }
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
            std::string details = result.errorDetails;
            if (details.empty()) details = "Provider: " + turn.provider.name + "\nModel: " + turn.request.model + "\n";
            finish(ChatListener::Outcome::Failed,
                   result.error.empty() ? "The request failed and the provider gave no reason" : result.error, details);
            return;
        }

        Post([reply](ChatListener& l) mutable { l.OnMessage(reply); });
        turn.request.messages.push_back(reply);

        if (reply.toolCalls.empty() || !(turn.tools || turn.computer)) {
            // A message sent meanwhile keeps the conversation going.
            if (deliverQueued()) continue;
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
            ActivityEvent started;
            started.id = call.id;
            started.toolUseId = call.id;
            started.kind = call.name == "run_command" ? "shell" : "tool";
            started.description = description;
            started.status = "running";
            Post([started](ChatListener& l) { l.OnActivity(started); });
            auto toolStart = std::chrono::steady_clock::now();
            ToolOutcome outcome = turn.computer ? RunComputerTool(call, computer) : RunTool(call, turn.toolContext);
            ActivityEvent ended;
            ended.id = call.id;
            ended.status = m_cancel ? "stopped" : outcome.error ? "failed" : "completed";
            ended.summary = outcome.summary;
            ended.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - toolStart).count();
            ended.line = TruncateUtf8(outcome.output, 4000);
            Post([ended](ChatListener& l) { l.OnActivity(ended); });
            Message toolMsg;
            toolMsg.role = Role::Tool;
            toolMsg.toolCallId = call.id;
            toolMsg.toolName = call.name;
            toolMsg.content = outcome.output;
            toolMsg.toolError = outcome.error;
            toolMsg.images = outcome.images;
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
        deliverQueued();
    }
    finish(ChatListener::Outcome::Failed,
           "Stopped after " + std::to_string(turn.maxSteps) + " tool steps. Send a message to let it continue.");
}

void ChatRunner::Enqueue(Message message) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.push_back(std::move(message));
}

std::vector<Message> ChatRunner::TakeQueued() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    std::vector<Message> out(m_queue.begin(), m_queue.end());
    m_queue.clear();
    return out;
}

bool ChatRunner::CancelQueued(size_t index) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (index >= m_queue.size()) return false;
    m_queue.erase(m_queue.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// The conversation written out for summarizing: long tool output and old
// reasoning are trimmed, so it fits even when the conversation nearly fills
// the model's context.
static std::string Transcript(const std::vector<Message>& messages) {
    std::string t;
    for (const auto& m : messages) {
        switch (m.role) {
            case Role::User: t += "User: " + m.content + "\n\n"; break;
            case Role::System: t += "Earlier summary: " + m.content + "\n\n"; break;
            case Role::Assistant:
                if (!m.content.empty()) t += "Assistant: " + m.content + "\n\n";
                for (const auto& c : m.toolCalls) t += "[Assistant used " + c.name + ": " + OneLine(c.arguments, 400) + "]\n";
                break;
            case Role::Tool:
                t += "[Result of " + m.toolName + (m.toolError ? " (failed)" : "") + ": " +
                     TruncateUtf8(m.content, 1500) + (m.content.size() > 1500 ? " ...]" : "]") + "\n\n";
                break;
        }
    }
    return t;
}

bool SummarizeConversation(Provider& provider, const std::string& model, const std::vector<Message>& messages,
                           int64_t through, const std::atomic<bool>* cancel, Message& summary, ChatResult& failure) {
    ChatRequest request;
    request.model = model;
    request.systemPrompt =
        "You compact long conversations so they can continue in a fresh context. Write a summary that lets the "
        "assistant carry on as if it remembered everything that matters: the user's goals and requests, decisions "
        "made and why, the current state of the work (files created or changed, commands run and what they "
        "showed, what works and what does not), problems still open, the next steps that were planned, and the "
        "user's preferences. Keep exact names, paths, numbers and code that later work needs. Leave out chit-chat "
        "and anything finished that no longer matters. Write plain text with short headings.";
    request.reasoningEffort = provider.Config().kind == "anthropic" || provider.Config().kind == "gemini" ? "low" : "";
    Message user;
    user.role = Role::User;
    user.content = "Here is the conversation to summarize:\n\n" + Transcript(messages) + "\nWrite the summary now.";
    request.messages.push_back(user);
    failure = provider.Chat(request, StreamCallbacks(), cancel);
    if (!failure.ok || Trim(failure.text).empty()) {
        if (failure.ok) failure.error = "The model wrote no summary";
        return false;
    }
    int64_t before = 0;
    for (const auto& m : messages) before += EstimateTokens(m.content) + EstimateTokens(m.reasoning) / 2;
    int count = 0;
    for (const auto& m : messages) {
        if (GetNoteKind(m) == NoteKind::None) count++;
    }
    summary = MakeNote(NoteKind::Compaction, "Summary of the conversation so far:\n\n" + Trim(failure.text),
                       {{"through", through}, {"count", count}, {"tokensBefore", before},
                        {"tokensAfter", failure.usage.outputTokens}});
    summary.usage = failure.usage;
    summary.createdAt = NowSeconds();
    return true;
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
