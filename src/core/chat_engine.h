#pragma once

// Runs one exchange of a conversation on a worker thread: sends the history to
// the provider, streams the reply back, and in code conversations runs the
// tools the model asks for and sends their results, until the model is done.
// Everything it reports arrives on the UI thread through ChatListener.

#include "core/code_tools.h"
#include "core/computer_tools.h"
#include "core/providers/provider.h"

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ChatListener {
public:
    virtual ~ChatListener() = default;
    virtual void OnReasoningDelta(const std::string& text) = 0;
    virtual void OnTextDelta(const std::string& text) = 0;
    virtual void OnToolCallStarted(const std::string& toolName) = 0;
    // A finished assistant turn (possibly with tool calls) or a tool result,
    // to store and show. The listener assigns its id.
    virtual void OnMessage(Message& message) = 0;
    // About to run a tool.
    virtual void OnToolRunning(const ToolCall& call, const std::string& description) = 0;
    // Asked when a command (or, for coding agents, another action such as
    // writing a file) needs approval; call `answer` (on the UI thread) once.
    virtual void OnApproveCommand(const std::string& what, bool isCommand, std::function<void(bool)> answer) = 0;
    // A coding agent asks the user questions with choices; call `answer` (on the
    // UI thread) once: whether they answered, and one answer per question.
    virtual void OnAskQuestions(const std::vector<AgentQuestion>& questions,
                                std::function<void(bool answered, std::vector<std::string> answers)> answer) {
        (void)questions;
        answer(false, {});
    }
    enum class Outcome { Done, Stopped, Failed };
    virtual void OnFinished(Outcome outcome, const std::string& error) = 0;
    // What it is doing now ("Trying again in 2 seconds...").
    virtual void OnStatus(const std::string& text) { (void)text; }
    // A failure, with its details, before OnFinished (or on its own when the
    // turn carries on, as after a failed compaction).
    virtual void OnProblem(const std::string& error, const std::string& details) { (void)error; (void)details; }
    // A message the user sent while it ran has now gone to the model.
    virtual void OnQueuedDelivered(Message& message) { OnMessage(message); }
    // A subagent, background shell or tool started, progressed or ended.
    virtual void OnActivity(const ActivityEvent& event) { (void)event; }
};

struct ChatTurn {
    ProviderConfig provider;
    ChatRequest request;          // model, system prompt, settings; messages = the history
    bool tools = false;           // code conversation
    bool computer = false;        // computer use conversation
    bool vision = true;           // the model can see images (computer use sends screenshots)
    ToolContext toolContext;      // also the approval setting for computer use
    int maxSteps = 50;
    // Compaction: summarize the history first (compactFirst), or only that
    // (compactOnly). The summary covers messages up to compactThrough (an id).
    bool compactFirst = false;
    bool compactOnly = false;
    int64_t compactThrough = 0;
};

class ChatRunner : public std::enable_shared_from_this<ChatRunner> {
public:
    // `post` runs a function on the UI thread.
    explicit ChatRunner(std::function<void(std::function<void()>)> post);
    ~ChatRunner();

    void SetListener(ChatListener* listener) { m_listener = listener; }  // UI thread
    bool Start(ChatTurn turn);
    void Stop();
    bool Running() const { return m_running.load(); }

    // Messages sent while it runs; they go to the model at its next step.
    void Enqueue(Message message);
    std::vector<Message> TakeQueued();
    bool CancelQueued(size_t index);  // false if it already went

private:
    void Run(ChatTurn turn);
    void RunTurn(ChatTurn turn);
    void RunAgentTurn(ChatTurn turn, Provider& provider);
    void Post(std::function<void(ChatListener&)> fn);
    bool AskApproval(const std::string& what, bool isCommand);
    bool AskQuestions(const std::vector<AgentQuestion>& questions, std::vector<std::string>& answers);

    std::function<void(std::function<void()>)> m_post;
    ChatListener* m_listener = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::mutex m_queueMutex;
    std::deque<Message> m_queue;
};

// Summarize `messages` into a compaction note covering up to `through`.
// On failure, `failure` holds the error. Blocking.
bool SummarizeConversation(Provider& provider, const std::string& model, const std::vector<Message>& messages,
                           int64_t through, const std::atomic<bool>* cancel, Message& summary, ChatResult& failure);

// Keep the screenshots of only the last `keep` tool results that have any
// (the user's own attachments all stay).
void LimitImages(std::vector<Message>& messages, int keep);

// Ask a model for a short title for a conversation (blocking; worker thread).
std::string GenerateTitle(const ProviderConfig& provider, const std::string& model, const std::string& userText,
                          const std::string& assistantText, std::string& error);

// One request, no streaming, no tools: for settings tests ("Say hello").
ChatResult QuickChat(const ProviderConfig& provider, const std::string& model, const std::string& prompt,
                     const std::atomic<bool>* cancel);
