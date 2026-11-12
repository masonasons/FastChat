#pragma once

// Runs one exchange of a conversation on a worker thread: sends the history to
// the provider, streams the reply back, and in code conversations runs the
// tools the model asks for and sends their results, until the model is done.
// Everything it reports arrives on the UI thread through ChatListener.

#include "core/code_tools.h"
#include "core/providers/provider.h"

#include <atomic>
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
    enum class Outcome { Done, Stopped, Failed };
    virtual void OnFinished(Outcome outcome, const std::string& error) = 0;
};

struct ChatTurn {
    ProviderConfig provider;
    ChatRequest request;          // model, system prompt, settings; messages = the history
    bool tools = false;           // code conversation
    ToolContext toolContext;
    int maxSteps = 50;
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

private:
    void Run(ChatTurn turn);
    void RunTurn(ChatTurn turn);
    void RunAgentTurn(ChatTurn turn, Provider& provider);
    void Post(std::function<void(ChatListener&)> fn);
    bool AskApproval(const std::string& what, bool isCommand);

    std::function<void(std::function<void()>)> m_post;
    ChatListener* m_listener = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
};

// Ask a model for a short title for a conversation (blocking; worker thread).
std::string GenerateTitle(const ProviderConfig& provider, const std::string& model, const std::string& userText,
                          const std::string& assistantText, std::string& error);

// One request, no streaming, no tools: for settings tests ("Say hello").
ChatResult QuickChat(const ProviderConfig& provider, const std::string& model, const std::string& prompt,
                     const std::atomic<bool>* cancel);
