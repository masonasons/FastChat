// Tests of FastChat's core: no windows. Provider tests run against
// tests/mock_server.py when FASTCHAT_MOCK names its address
// (e.g. http://127.0.0.1:8765).

#include "core/attachments.h"
#include "core/chat_engine.h"
#include "core/code_tools.h"
#include "core/computer_tools.h"
#include "core/conversation_import.h"
#include "core/desktop.h"
#include "core/database.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/settings.h"
#include "core/speech.h"
#include "core/notify.h"
#include "core/soundpack.h"
#include "core/sse.h"
#include "core/updater.h"
#include "core/util.h"

#include <miniz.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        g_checks++;                                                                                        \
        auto va = (a);                                                                                     \
        auto vb = (b);                                                                                     \
        if (!(va == vb)) {                                                                                 \
            g_failures++;                                                                                  \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b " (" << va << " vs " \
                      << vb << ")\n";                                                                      \
        }                                                                                                  \
    } while (0)

static void TestSse() {
    std::vector<std::pair<std::string, std::string>> events;
    SseParser p([&](const std::string& e, const std::string& d) {
        events.emplace_back(e, d);
        return true;
    });
    std::string stream = "event: a\ndata: {\"x\":1}\n\n: keepalive\n\ndata: line1\r\ndata: line2\r\n\r\ndata: [DONE]\n\n";
    // Feed one byte at a time: events must not depend on how the network splits them.
    for (char c : stream) p.Feed(&c, 1);
    p.Finish();
    CHECK_EQ(events.size(), size_t{3});
    if (events.size() == 3) {
        CHECK_EQ(events[0].first, std::string("a"));
        CHECK_EQ(events[0].second, std::string("{\"x\":1}"));
        CHECK_EQ(events[1].second, std::string("line1\nline2"));
        CHECK_EQ(events[2].second, std::string("[DONE]"));
    }
    std::vector<std::string> lines;
    NdjsonParser n([&](const std::string& l) {
        lines.push_back(l);
        return true;
    });
    std::string nd = "{\"a\":1}\n{\"b\":2}\n{\"c\":3}";
    n.Feed(nd.data(), 5);
    n.Feed(nd.data() + 5, nd.size() - 5);
    n.Finish();
    CHECK_EQ(lines.size(), size_t{3});
}

static void TestUtil() {
    CHECK_EQ(WideToUtf8(Utf8ToWide("héllo – 😀")), std::string("héllo – 😀"));
    CHECK_EQ(FormatThousands(1234567), std::string("1,234,567"));
    CHECK_EQ(FormatTokensShort(1000000), std::string("1M"));
    CHECK_EQ(FormatTokensShort(12345), std::string("12.3K"));
    CHECK_EQ(Base64Decode(Base64Encode("any carnal pleas")), std::string("any carnal pleas"));
    CHECK_EQ(OneLine("a\n\n  b\tc", 0), std::string("a b c"));
    CHECK_EQ(StripMarkdownForSpeech("## Title\n**bold** and [link](http://x)"), std::string("Title\nbold and link"));
    // Text that is not UTF-8 (a Latin-1 file, console output in an OEM code page,
    // overlong forms, encoded surrogates) is cleaned rather than sent as is.
    for (const std::string bad : {std::string("caf\xE9"), std::string("\xC0\x80"), std::string("a\xED\xA0\x80z"),
                                  std::string("\xF5\x80\x80\x80"), std::string("cut \xE2\x82")}) {
        CHECK(!IsValidUtf8(bad));
        CHECK(IsValidUtf8(SanitizeUtf8(bad)));
        json j = {{"content", bad}};
        bool threw = false;
        try {
            JsonDump(j);
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
    }
    CHECK(IsValidUtf8("caf\xC3\xA9 \xF0\x9F\x98\x80"));
    CHECK_EQ(SanitizeUtf8("caf\xE9!"), std::string("caf\xEF\xBF\xBD!"));
    // A lone surrogate from UTF-16 becomes the replacement character.
    CHECK(IsValidUtf8(WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800)))));
    auto blocks = ExtractCodeBlocks("text\n```cpp\nint x;\n```\nmore");
    CHECK_EQ(blocks.size(), size_t{1});
    if (!blocks.empty()) CHECK_EQ(blocks[0], std::string("int x;\n"));
}

static void TestDatabase(const std::string& dir) {
    Database db;
    CHECK(db.Open(JoinPath(dir, "test.db")));
    Conversation c;
    c.title = "Test";
    c.type = ConversationType::Code;
    c.folder = dir;
    c.settings.model = "m1";
    c.settings.speech.speakResponses = 2;
    c.settings.events[static_cast<int>(EventId::ToolCall)].soundEnabled = false;
    CHECK(db.CreateConversation(c));
    CHECK(c.id > 0);
    Message m;
    m.conversationId = c.id;
    m.role = Role::Assistant;
    m.content = "hi";
    m.toolCalls.push_back({"call_1", "read_file", "{\"path\":\"a\"}"});
    m.usage.inputTokens = 100;
    m.usage.outputTokens = 10;
    m.usage.cost = 0.5;
    m.providerData["anthropic"] = {{"content", json::array({{{"type", "thinking"}, {"signature", "s"}}})}};
    CHECK(db.AddMessage(m));
    Message m2 = m;
    m2.usage.cost = 0.25;
    CHECK(db.AddMessage(m2));
    auto list = db.ListConversations();
    CHECK_EQ(list.size(), size_t{1});
    if (!list.empty()) {
        CHECK_EQ(list[0].messageCount, int64_t{2});
        CHECK(std::abs(list[0].totalCost - 0.75) < 1e-9);
        CHECK_EQ(list[0].settings.model, std::string("m1"));
        CHECK(list[0].settings.speech.speakResponses.value_or(-1) == 2);
        CHECK(list[0].settings.events[static_cast<int>(EventId::ToolCall)].soundEnabled.value_or(true) == false);
        CHECK(list[0].type == ConversationType::Code);
    }
    auto messages = db.LoadMessages(c.id);
    CHECK_EQ(messages.size(), size_t{2});
    if (!messages.empty()) {
        CHECK_EQ(messages[0].toolCalls.size(), size_t{1});
        CHECK_EQ(messages[0].toolCalls[0].name, std::string("read_file"));
        CHECK(messages[0].providerData.contains("anthropic"));
    }
    CHECK(db.DeleteMessagesFrom(c.id, messages[1].id));
    CHECK_EQ(db.LoadMessages(c.id).size(), size_t{1});
    CHECK(db.DeleteConversation(c.id));
    CHECK(db.ListConversations().empty());
    db.Close();
}

static void TestSecrets() {
    std::string stored = ProtectSecret("sk-secret-123");
    CHECK(stored.find("sk-secret") == std::string::npos);
    CHECK_EQ(UnprotectSecret(stored), std::string("sk-secret-123"));
    CHECK_EQ(UnprotectSecret("typed-by-hand"), std::string("typed-by-hand"));
}

static void TestCodeTools(const std::string& dir) {
    std::string root = JoinPath(dir, "project");
    fs::create_directories(PathFromUtf8(root));
    ToolContext ctx;
    ctx.root = root;
    ctx.commandApproval = "auto";
    auto run = [&](const std::string& name, const json& args) { return RunTool({"id", name, args.dump()}, ctx); };

    auto w = run("write_file", {{"path", "src/main.cpp"}, {"content", "int main() {\n    return 0;\n}\n"}});
    CHECK(!w.error);
    auto r = run("read_file", {{"path", "src/main.cpp"}});
    CHECK(!r.error);
    CHECK(r.output.find("2\t    return 0;") != std::string::npos);
    auto e = run("edit_file", {{"path", "src/main.cpp"}, {"old_text", "return 0;"}, {"new_text", "return 1;"}});
    CHECK(!e.error);
    std::string data;
    ReadFileText(JoinPath(root, "src/main.cpp"), data);
    CHECK(data.find("return 1;") != std::string::npos);
    auto missing = run("edit_file", {{"path", "src/main.cpp"}, {"old_text", "nope"}, {"new_text", "x"}});
    CHECK(missing.error);
    // A file in another encoding reads back as UTF-8.
    WriteFileText(JoinPath(root, "latin1.txt"), "caf\xE9\n");
    auto latin = run("read_file", {{"path", "latin1.txt"}});
    CHECK(!latin.error);
    CHECK(IsValidUtf8(latin.output));
    auto find = run("find_files", {{"pattern", "*.cpp"}});
    CHECK(find.output.find("src/main.cpp") != std::string::npos);
    auto search = run("search_files", {{"pattern", "return \\d"}});
    CHECK(search.output.find("src/main.cpp:2:") != std::string::npos);
    auto list = run("list_directory", {{"path", "."}, {"recursive", true}});
    CHECK(list.output.find("src/") != std::string::npos);
    auto escape = run("read_file", {{"path", "../outside.txt"}});
    CHECK(escape.error);
    auto escape2 = run("write_file", {{"path", "../../evil.txt"}, {"content", "x"}});
    CHECK(escape2.error);
    auto cmd = run("run_command", {{"command", "echo fastchat-ok"}});
    CHECK(!cmd.error);
    CHECK(cmd.output.find("fastchat-ok") != std::string::npos);
    auto fail = run("run_command", {{"command", "exit 3"}});
    CHECK(fail.error);
    CHECK(fail.output.find("Exit code: 3") != std::string::npos);
    ctx.commandApproval = "deny";
    auto denied = run("run_command", {{"command", "echo no"}});
    CHECK(denied.error);
    ctx.commandApproval = "ask";
    ctx.approveCommand = [](const std::string&) { return false; };
    auto declined = run("run_command", {{"command", "echo no"}});
    CHECK(declined.error);
}

static ProviderConfig MockProvider(const std::string& base, const std::string& kind, const std::string& path) {
    ProviderConfig c;
    c.id = "mock-" + kind;
    c.kind = kind;
    c.name = "Mock " + kind;
    c.baseUrl = base + path;
    c.apiKey = "test";
    return c;
}

// A ChatRunner driven to the end, with its UI calls run on this thread.
struct TestListener : ChatListener {
    std::string text, reasoning;
    std::vector<Message> messages;
    std::vector<std::string> tools;
    bool finished = false;
    Outcome outcome = Outcome::Failed;
    std::string error;
    void OnReasoningDelta(const std::string& t) override { reasoning += t; }
    void OnTextDelta(const std::string& t) override { text += t; }
    void OnToolCallStarted(const std::string&) override {}
    void OnMessage(Message& m) override { messages.push_back(m); }
    void OnToolRunning(const ToolCall&, const std::string& d) override { tools.push_back(d); }
    void OnApproveCommand(const std::string& what, bool, std::function<void(bool)> answer) override {
        approvals.push_back(what);
        answer(approve);
    }
    std::vector<std::string> approvals;
    bool approve = true;
    // Questions with choices: what was asked, and how the "user" answers.
    std::vector<AgentQuestion> asked;
    bool answerQuestions = true;
    std::vector<std::string> questionAnswers;
    void OnAskQuestions(const std::vector<AgentQuestion>& q, std::function<void(bool, std::vector<std::string>)> answer) override {
        asked = q;
        answer(answerQuestions, questionAnswers);
    }
    std::vector<std::pair<std::string, std::string>> problems;
    std::vector<std::string> statuses;
    int delivered = 0;
    std::vector<ActivityEvent> activities;
    void OnActivity(const ActivityEvent& e) override { activities.push_back(e); }
    void OnProblem(const std::string& e, const std::string& d) override { problems.emplace_back(e, d); }
    void OnStatus(const std::string& s) override { statuses.push_back(s); }
    void OnQueuedDelivered(Message& m) override {
        delivered++;
        OnMessage(m);
    }
    void OnFinished(Outcome o, const std::string& e) override {
        finished = true;
        outcome = o;
        error = e;
    }
};

static TestListener RunTurn(const ProviderConfig& provider, const std::string& model, bool tools, const std::string& root) {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    auto runner = std::make_shared<ChatRunner>([&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(fn));
    });
    TestListener listener;
    runner->SetListener(&listener);
    ChatTurn turn;
    turn.provider = provider;
    turn.request.model = model;
    Message user;
    user.role = Role::User;
    user.content = "Hello";
    turn.request.messages.push_back(user);
    turn.tools = tools;
    turn.toolContext.root = root;
    turn.toolContext.commandApproval = "auto";
    runner->Start(turn);
    for (int i = 0; i < 3000 && !listener.finished; i++) {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(mutex);
            work.swap(queue);
        }
        for (auto& fn : work) fn();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner->SetListener(nullptr);
    return listener;
}

static void TestProvider(const std::string& base, const std::string& kind, const std::string& path, const std::string& model,
                         const std::string& root) {
    std::cout << "  " << kind << "\n";
    ProviderConfig config = MockProvider(base, kind, path);
    auto provider = CreateProvider(config);

    std::vector<ModelInfo> models;
    std::string error;
    CHECK(provider->ListModels(models, error, nullptr));
    CHECK(!models.empty());
    if (!error.empty()) std::cerr << "    list error: " << error << "\n";
    AppProviderCache().SetModels(config.id, models);

    // Plain streamed answer
    TestListener plain = RunTurn(config, model, false, root);
    CHECK(plain.finished);
    CHECK(plain.outcome == ChatListener::Outcome::Done);
    if (!plain.error.empty()) std::cerr << "    error: " << plain.error << "\n";
    CHECK_EQ(plain.text, std::string("Hello world. This is the second sentence."));
    CHECK_EQ(plain.messages.size(), size_t{1});
    if (!plain.messages.empty()) {
        CHECK_EQ(plain.messages[0].content, plain.text);
        CHECK(plain.messages[0].usage.HasTokens());
    }

    // A tool loop: list_directory, then the answer
    TestListener loop = RunTurn(config, model, true, root);
    CHECK(loop.finished);
    CHECK(loop.outcome == ChatListener::Outcome::Done);
    if (!loop.error.empty()) std::cerr << "    loop error: " << loop.error << "\n";
    CHECK_EQ(loop.messages.size(), size_t{3});  // call, result, answer
    if (loop.messages.size() == 3) {
        CHECK_EQ(loop.messages[0].toolCalls.size(), size_t{1});
        CHECK_EQ(loop.messages[0].toolCalls[0].name, std::string("list_directory"));
        CHECK(loop.messages[1].role == Role::Tool);
        CHECK(loop.messages[1].content.find("Contents of") != std::string::npos);
        CHECK_EQ(loop.messages[2].content, std::string("Hello world. This is the second sentence."));
    }
    CHECK_EQ(loop.tools.size(), size_t{1});
}

// Any turn, driven to the end; `afterStart` runs right after it starts (to queue messages).
static TestListener Drive(ChatTurn turn, std::function<void(ChatRunner&)> afterStart = nullptr,
                          std::function<void(TestListener&)> setup = nullptr) {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    auto runner = std::make_shared<ChatRunner>([&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(fn));
    });
    TestListener listener;
    if (setup) setup(listener);
    runner->SetListener(&listener);
    runner->Start(turn);
    if (afterStart) afterStart(*runner);
    for (int i = 0; i < 6000 && !listener.finished; i++) {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(mutex);
            work.swap(queue);
        }
        for (auto& fn : work) fn();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner->SetListener(nullptr);
    return listener;
}

static Message UserMessage(const std::string& text, int64_t id = 0) {
    Message m;
    m.role = Role::User;
    m.content = text;
    m.id = id;
    return m;
}

// Messages sent while it works, retries, error details, compaction.
static void TestQueueRetryCompaction(const std::string& base, const std::string& root) {
    std::cout << "Queue, retries, errors, compaction:\n";
    ProviderConfig mock = MockProvider(base, "openrouter", "/openai");

    // A plain answer, with a message queued meanwhile: answered as the next step.
    ChatTurn chat;
    chat.provider = mock;
    chat.request.model = "mock-model";
    chat.request.messages = {UserMessage("First")};
    TestListener q = Drive(chat, [](ChatRunner& r) { r.Enqueue(UserMessage("And another thing")); });
    CHECK(q.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(q.delivered, 1);
    CHECK_EQ(q.messages.size(), size_t{3});  // answer, the queued message, its answer
    if (q.messages.size() == 3) {
        CHECK(q.messages[0].role == Role::Assistant);
        CHECK(q.messages[1].role == Role::User && q.messages[1].content == "And another thing");
        CHECK(q.messages[2].role == Role::Assistant);
    }
    std::cout << "  queued during a chat: " << q.messages.size() << " messages\n";

    // In a tool loop it goes in after the tool results.
    ChatTurn tools = chat;
    tools.tools = true;
    tools.toolContext.root = root;
    tools.toolContext.commandApproval = "auto";
    TestListener qt = Drive(tools, [](ChatRunner& r) { r.Enqueue(UserMessage("Also check the tests")); });
    CHECK(qt.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(qt.messages.size(), size_t{4});  // call, result, queued message, answer
    bool toolActivity = false;
    for (const auto& e : qt.activities) toolActivity = toolActivity || (e.status == "completed" && e.durationMs >= 0);
    CHECK(toolActivity);
    if (qt.messages.size() == 4) {
        CHECK(qt.messages[1].role == Role::Tool);
        CHECK(qt.messages[2].role == Role::User);
        CHECK(qt.messages[3].role == Role::Assistant && qt.messages[3].toolCalls.empty());
    }

    // Two 503s, then an answer: retried, with the wait reported.
    ChatTurn flaky = chat;
    flaky.provider = MockProvider(base, "openrouter", "/flaky");
    TestListener f = Drive(flaky);
    CHECK(f.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(f.statuses.size(), size_t{2});
    if (!f.statuses.empty()) CHECK(f.statuses[0].find("Trying again") != std::string::npos);
    std::cout << "  retries: " << (f.statuses.empty() ? std::string("none") : f.statuses[0]) << "\n";

    // A failure explains itself.
    ChatTurn bad = chat;
    bad.request.model = "fail";
    TestListener b = Drive(bad);
    CHECK(b.outcome == ChatListener::Outcome::Failed);
    CHECK_EQ(b.problems.size(), size_t{1});
    if (!b.problems.empty()) {
        CHECK(b.problems[0].first.find("HTTP 400") != std::string::npos);
        CHECK(b.problems[0].second.find("HTTP status: 400") != std::string::npos);
        CHECK(b.problems[0].second.find("does not exist") != std::string::npos);
        std::cout << "  error: " << b.problems[0].first << "\n";
    }

    // Compaction on request: a summary note covering everything so far.
    ChatTurn compact = chat;
    Message a1;
    a1.role = Role::Assistant;
    a1.content = "Hi";
    a1.id = 2;
    compact.request.messages = {UserMessage("Hello", 1), a1, UserMessage("Tell me more", 3)};
    compact.compactOnly = true;
    compact.compactThrough = 3;
    TestListener c = Drive(compact);
    CHECK(c.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(c.messages.size(), size_t{1});
    if (c.messages.size() == 1) {
        CHECK(GetNoteKind(c.messages[0]) == NoteKind::Compaction);
        CHECK(c.messages[0].content.find("Summary of the conversation so far") != std::string::npos);
        CHECK_EQ(c.messages[0].providerData["fastchat"].value("through", int64_t{0}), int64_t{3});
        // What gets sent afterwards: the summary, then what came after it.
        std::vector<Message> all = compact.request.messages;
        Message summary = c.messages[0];
        summary.id = 4;
        all.push_back(summary);
        all.push_back(UserMessage("Next", 5));
        Message err = MakeNote(NoteKind::Error, "boom");
        err.id = 6;
        all.push_back(err);
        auto context = ContextMessages(all);
        CHECK_EQ(context.size(), size_t{2});
        if (context.size() == 2) {
            CHECK(context[0].role == Role::System && context[0].providerData.is_null());
            CHECK_EQ(context[1].content, std::string("Next"));
        }
    }
    // Automatic: compact first, then answer the new message.
    ChatTurn first = compact;
    first.compactOnly = false;
    first.compactFirst = true;
    first.compactThrough = 2;
    TestListener cf = Drive(first);
    CHECK(cf.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(cf.messages.size(), size_t{2});  // the summary, the answer
    if (cf.messages.size() == 2) {
        CHECK(GetNoteKind(cf.messages[0]) == NoteKind::Compaction);
        CHECK(cf.messages[1].role == Role::Assistant);
    }
    std::cout << "  compaction: summary of " << (c.messages.empty() ? 0 : c.messages[0].providerData["fastchat"].value("count", 0))
              << " messages\n";
}

// A turn with a coding agent (fake Claude Code / Codex), driven to the end.
static TestListener RunAgentTurn(const ProviderConfig& provider, std::vector<Message> history, const std::string& prompt,
                                 bool tools, bool approve, const std::string& root) {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    auto runner = std::make_shared<ChatRunner>([&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(fn));
    });
    TestListener listener;
    listener.approve = approve;
    runner->SetListener(&listener);
    ChatTurn turn;
    turn.provider = provider;
    turn.request.model = "haiku";
    Message user;
    user.role = Role::User;
    user.content = prompt;
    history.push_back(user);
    turn.request.messages = history;
    turn.tools = tools;
    turn.toolContext.root = root;
    turn.toolContext.commandApproval = "ask";
    runner->Start(turn);
    for (int i = 0; i < 3000 && !listener.finished; i++) {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(mutex);
            work.swap(queue);
        }
        for (auto& fn : work) fn();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner->SetListener(nullptr);
    return listener;
}

static void TestAgents(const std::string& root) {
    std::string testsDir = PathToUtf8(fs::path(__FILE__).parent_path());
#ifdef _WIN32
    std::string claudeProgram = JoinPath(testsDir, "fake_claude.cmd");
    std::string codexProgram = JoinPath(testsDir, "fake_codex.cmd");
#else
    std::string claudeProgram = JoinPath(testsDir, "fake_claude.py");
    std::string codexProgram = JoinPath(testsDir, "fake_codex.py");
#endif
    ProviderConfig claude;
    claude.id = "cc";
    claude.kind = "claude_code";
    claude.name = "Claude Code";
    claude.baseUrl = claudeProgram;

    std::cout << "  claude code: chat\n";
    TestListener chat = RunAgentTurn(claude, {}, "Hello there", false, true, root);
    if (!chat.error.empty()) std::cerr << "    error: " << chat.error << "\n";
    CHECK(chat.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(Trim(chat.text), std::string("Hello from fake Claude."));
    CHECK_EQ(chat.messages.size(), size_t{1});
    if (chat.messages.size() == 1) {
        CHECK_EQ(chat.messages[0].content, std::string("Hello from fake Claude."));
        CHECK(std::abs(chat.messages[0].usage.cost - 0.0125) < 1e-9);
        CHECK_EQ(chat.messages[0].usage.inputTokens, int64_t{153});
        CHECK_EQ(chat.messages[0].providerData["claude_code"].value("session_id", ""), std::string("sess-1"));
        CHECK_EQ(chat.messages[0].providerId, std::string("cc"));
    }
    AccountInfo account;
    CHECK(AppProviderCache().GetAccount("cc", account));
    CHECK_EQ(account.summary, std::string("Plan 25% used (seven day)"));

    std::cout << "  claude code: resume\n";
    TestListener resumed = RunAgentTurn(claude, chat.messages, "And again", false, true, root);
    CHECK(resumed.outcome == ChatListener::Outcome::Done);
    CHECK(resumed.text.find("Resumed sess-1") != std::string::npos);

    std::cout << "  claude code: approved write\n";
    TestListener write = RunAgentTurn(claude, {}, "Please write a file", true, true, root);
    if (!write.error.empty()) std::cerr << "    error: " << write.error << "\n";
    CHECK(write.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(write.approvals.size(), size_t{1});
    if (!write.approvals.empty()) CHECK_EQ(write.approvals[0], std::string("Write notes.txt"));
    CHECK_EQ(write.messages.size(), size_t{3});  // the call, its result, the answer
    if (write.messages.size() == 3) {
        CHECK_EQ(write.messages[0].toolCalls.size(), size_t{1});
        CHECK_EQ(write.messages[0].reasoning, std::string("I will write it."));
        CHECK(write.messages[1].role == Role::Tool);
        CHECK(!write.messages[1].toolError);
        CHECK_EQ(write.messages[1].toolName, std::string("Write"));
        CHECK_EQ(write.messages[2].content, std::string("Done."));
    }
    CHECK_EQ(write.tools.size(), size_t{1});

    std::cout << "  claude code: declined write\n";
    TestListener declined = RunAgentTurn(claude, {}, "Please write a file", true, false, root);
    CHECK(declined.messages.size() == 3 && declined.messages[1].toolError);
    CHECK(declined.messages.size() == 3 && declined.messages[2].content == "You declined.");

    std::cout << "  claude code: command\n";
    TestListener run = RunAgentTurn(claude, {}, "Please run something", true, true, root);
    CHECK(run.approvals.empty());
    CHECK_EQ(run.messages.size(), size_t{3});
    if (run.messages.size() == 3) {
        CHECK_EQ(run.messages[1].content, std::string("hi"));
        CHECK_EQ(run.messages[1].reasoning, std::string("Run: echo hi"));
        CHECK_EQ(run.messages[2].content, std::string("It printed hi."));
    }
    for (const auto& m : run.messages) CHECK(m.content.find("SUBAGENT") == std::string::npos);

    ProviderConfig codex;
    codex.id = "cx";
    codex.kind = "codex";
    codex.name = "Codex";
    codex.baseUrl = codexProgram;
    std::cout << "  codex: command and file change\n";
    TestListener cx = RunAgentTurn(codex, {}, "Please run the build", true, true, root);
    if (!cx.error.empty()) std::cerr << "    error: " << cx.error << "\n";
    CHECK(cx.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(cx.messages.size(), size_t{5});  // call, output, change call, change, answer
    if (cx.messages.size() == 5) {
        CHECK_EQ(cx.messages[0].toolCalls[0].name, std::string("run_command"));
        CHECK_EQ(cx.messages[0].reasoning, std::string("sandbox=workspace-write"));
        CHECK(cx.messages[1].content.find("Exit code: 0") != std::string::npos);
        CHECK(!cx.messages[1].toolError);
        CHECK_EQ(cx.messages[3].reasoning, std::string("Changed main.cpp"));
        CHECK_EQ(cx.messages[4].content, std::string("All done."));
        CHECK_EQ(cx.messages[4].usage.outputTokens, int64_t{20});
        CHECK_EQ(cx.messages[4].providerData["codex"].value("thread_id", ""), std::string("thread-1"));
    }
    std::cout << "  codex: chat and resume\n";
    TestListener cxChat = RunAgentTurn(codex, cx.messages, "Thanks", false, true, root);
    CHECK(cxChat.outcome == ChatListener::Outcome::Done);
    CHECK(cxChat.text.find("Resumed thread-1") != std::string::npos);
    CHECK(cxChat.messages.size() == 1 && cxChat.messages[0].reasoning == "sandbox=read-only");

    // Images go to Codex as files (and are cleaned up); a PDF is only named.
    std::cout << "  codex: attachments\n";
    {
        ChatTurn cxFiles;
        cxFiles.provider = codex;
        Message asked = UserMessage("What is this?");
        AttachToMessage(asked, {{"chart.png", "image/png", "PNG"}, {"report.pdf", "application/pdf", "%PDF"}});
        cxFiles.request.messages = {asked};
        TestListener cf = Drive(cxFiles);
        CHECK(cf.outcome == ChatListener::Outcome::Done);
        CHECK(cf.text.find("Images: image1.png 3 bytes.") != std::string::npos);
        CHECK(cf.text.find("A PDF was named.") != std::string::npos);
        bool leftOver = false;
        std::error_code ec;
        for (fs::directory_iterator d(PathFromUtf8(JoinPath(GetDataDir(), "agent-images")), ec), end; !ec && d != end; d.increment(ec)) {
            leftOver = true;
        }
        CHECK(!leftOver);
    }

    std::cout << "  claude code: message sent while it works\n";
    ChatTurn live;
    live.provider = claude;
    live.request.model = "haiku";
    live.request.messages = {UserMessage("Hello there")};
    TestListener lq = Drive(live, [](ChatRunner& r) { r.Enqueue(UserMessage("One more thing")); });
    CHECK(lq.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(lq.delivered, 1);
    bool gotIt = false;
    for (const auto& m : lq.messages) gotIt = gotIt || m.content == "Got: One more thing";
    CHECK(gotIt);

    // A message sent before the model's answer started streaming, which Claude
    // Code runs as a turn of its own: its approvals still get through (standard
    // input stays open until Claude Code has taken in every message).
    std::cout << "  claude code: approval in a turn of a message sent while it works\n";
    ChatTurn liveTools = live;
    liveTools.tools = true;
    liveTools.toolContext.root = root;
    liveTools.toolContext.commandApproval = "auto";
    TestListener lw = Drive(liveTools, [](ChatRunner& r) { r.Enqueue(UserMessage("Then write it down")); });
    CHECK(lw.outcome == ChatListener::Outcome::Done);
    bool allowed = false;
    for (const auto& m : lw.messages) allowed = allowed || m.content == "Got: Then write it down (allow)";
    CHECK(allowed);

    std::cout << "  claude code: subagent and background shell\n";
    ChatTurn agentTurn = live;
    agentTurn.tools = true;
    agentTurn.toolContext.root = root;
    agentTurn.toolContext.commandApproval = "ask";
    agentTurn.request.messages = {UserMessage("Start an agent and a background ping")};
    TestListener ag = Drive(agentTurn);
    CHECK(ag.outcome == ChatListener::Outcome::Done);
    // Its approval names the subagent asking.
    CHECK_EQ(ag.approvals.size(), size_t{1});
    if (!ag.approvals.empty()) CHECK_EQ(ag.approvals[0], std::string("Agent 'count files' wants to: Run: dir /b"));
    // The subagent's own words stay out of the conversation...
    for (const auto& m : ag.messages) CHECK(m.content.find("SUBAGENT") == std::string::npos);
    // ...and go to its activity, with its start, progress and end.
    bool started = false, progressed = false, ended = false, stepped = false, said = false, shellFile = false, shellDone = false;
    for (const auto& e : ag.activities) {
        if (e.id == "ag1" && e.kind == "agent" && e.status == "running" && e.description == "count files") started = true;
        if (e.id == "ag1" && e.toolUses == 1 && e.progress == "Running dir /b") progressed = true;
        if (e.id == "ag1" && e.status == "completed") ended = true;
        if (e.id == "ag1" && e.line == "Run: dir /b") stepped = true;
        if (e.id == "ag1" && e.line == "Says: SUBAGENT: two files") said = true;
        if (e.id == "bg1" && !e.outputFile.empty() && e.prompt == "ping -n 6 127.0.0.1") shellFile = true;
        if (e.id == "bg1" && e.status == "completed") shellDone = true;
    }
    CHECK(started);
    CHECK(progressed);
    CHECK(ended);
    CHECK(stepped);
    CHECK(said);
    CHECK(shellFile);
    CHECK(shellDone);
    std::cout << "    " << ag.activities.size() << " activity events\n";

    std::cout << "  claude code: /compact\n";
    ChatTurn cc = live;
    cc.request.messages = {UserMessage("/compact")};
    TestListener ccr = Drive(cc);
    CHECK(ccr.outcome == ChatListener::Outcome::Done);
    int notes = 0;
    for (const auto& m : ccr.messages) {
        if (!IsAgentCompactionNote(m)) continue;
        notes++;
        CHECK(m.content.find("31.9K tokens down to 6.9K") != std::string::npos);
        // Claude Code's summary, without its preamble and reminders.
        CHECK_EQ(m.providerData["fastchat"].value("summary", std::string()),
                 std::string("1. Primary Request and Intent:\n   The user said hello."));
    }
    CHECK_EQ(notes, 1);
    // Nothing else of the compaction becomes a message.
    for (const auto& m : ccr.messages) CHECK(m.content.find("This session is being continued") == std::string::npos);
    CHECK(std::find(ccr.statuses.begin(), ccr.statuses.end(), "Compacting the conversation") != ccr.statuses.end());

    // After a /compact (which gets no reply), only the new message is sent, in
    // the same session; not "/compact" again with the new message after it.
    std::cout << "  claude code: a message after /compact\n";
    Message reply;
    reply.id = 2;
    reply.role = Role::Assistant;
    reply.content = "Earlier answer.";
    reply.providerId = claude.id;
    reply.providerData["claude_code"] = {{"session_id", "sess-9"}};
    Message note = MakeNote(NoteKind::Note, "Compacted", {{"agentCompaction", true}});
    std::vector<Message> history = {UserMessage("Hi", 1), reply, UserMessage("/compact", 3), note,
                                    UserMessage("Swallowed by a compaction", 5), note, UserMessage("What next?", 7)};
    CHECK_EQ(ContextMessages(history).size(), size_t{5});        // notes left out for API models
    CHECK_EQ(ContextMessages(history, true).size(), size_t{7});  // kept for coding agents
    // The list starts at the latest compaction; the rest is in the full history.
    std::vector<bool> hidden = BeforeLatestCompaction(history);
    CHECK((hidden == std::vector<bool>{true, true, true, true, true, false, false}));
    // Claude Code compacting by itself keeps the message it is answering.
    Message autoNote = MakeNote(NoteKind::Note, "Claude Code compacted the conversation", {{"agentCompaction", true}, {"trigger", "auto"}});
    std::vector<Message> autoHistory = {UserMessage("Hi", 1), reply, UserMessage("Now this", 3), autoNote};
    CHECK((BeforeLatestCompaction(autoHistory) == std::vector<bool>{true, true, false, false}));
    // FastChat's summary covers the messages up to the one it names.
    Message summaryNote = MakeNote(NoteKind::Compaction, "Summary of the conversation so far: ...", {{"through", 2}});
    std::vector<Message> ownHistory = {UserMessage("Hi", 1), reply, UserMessage("Sent meanwhile", 3), summaryNote,
                                       UserMessage("After", 5)};
    CHECK((BeforeLatestCompaction(ownHistory) == std::vector<bool>{true, true, false, false, false}));
    CHECK((BeforeLatestCompaction({UserMessage("No compaction", 1)}) == std::vector<bool>{false}));
    ChatTurn after = live;
    after.request.messages = ContextMessages(history, true);
    TestListener afterResult = Drive(after);
    CHECK(afterResult.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(afterResult.text, std::string("Hello from fake Claude. Resumed sess-9. "));
    for (const auto& m : afterResult.messages) CHECK(!IsAgentCompactionNote(m));

    // A background task that ends after the reply gets a turn of its own, and
    // its approvals still reach FastChat (standard input stays open for it).
    std::cout << "  claude code: a background task's own turn\n";
    {
        ChatTurn bg = live;
        bg.request.messages = {UserMessage("Start something in the background")};
        TestListener done = Drive(bg);
        CHECK(done.outcome == ChatListener::Outcome::Done);
        bool allowed = false;
        for (const auto& m : done.messages) allowed = allowed || m.content == "Background done (allow).";
        CHECK(allowed);
    }

    // Questions with choices are always put to the user (even with everything
    // allowed), and the answers go back keyed by question.
    std::cout << "  claude code: questions\n";
    {
        ChatTurn ask = live;
        ask.toolContext.commandApproval = "auto";
        ask.request.messages = {UserMessage("Ask me a question")};
        TestListener answered = Drive(ask, nullptr, [](TestListener& l) { l.questionAnswers = {"Green", "Apple, Plum"}; });
        CHECK(answered.outcome == ChatListener::Outcome::Done);
        CHECK_EQ(answered.asked.size(), size_t{2});
        if (answered.asked.size() == 2) {
            CHECK(answered.asked[0].header == "Colour" && answered.asked[0].question == "Favourite colour?" && !answered.asked[0].multiSelect);
            CHECK(answered.asked[0].options.size() == 2 && answered.asked[0].options[1].label == "Green" &&
                  answered.asked[0].options[1].description == "calm");
            CHECK(answered.asked[1].multiSelect && answered.asked[1].options.size() == 2);
        }
        CHECK(answered.approvals.empty());  // a question, not a permission
        CHECK(std::find(answered.tools.begin(), answered.tools.end(), "Ask: Favourite colour? / Which fruits?") != answered.tools.end());
        bool chose = false, resultKept = false;
        for (const auto& m : answered.messages) {
            chose = chose || m.content == "You chose Green and Apple, Plum.";
            resultKept = resultKept || (m.role == Role::Tool && m.content.find("\"Favourite colour?\"=\"Green\"") != std::string::npos);
        }
        CHECK(chose);
        CHECK(resultKept);

        TestListener declined = Drive(ask, nullptr, [](TestListener& l) { l.answerQuestions = false; });
        bool toldNo = false;
        for (const auto& m : declined.messages) toldNo = toldNo || m.content == "You did not answer.";
        CHECK(toldNo);
    }

    // Attached images and PDFs go to Claude Code as content blocks, in the
    // first message and in ones sent while it works.
    std::cout << "  claude code: attachments\n";
    ChatTurn withFiles = live;
    Message asked = UserMessage("What are these?");
    AttachToMessage(asked, {{"chart.png", "image/png", "PNG"}, {"report.pdf", "application/pdf", "%PDF"}});
    withFiles.request.messages = {asked};
    TestListener wf = Drive(withFiles, [](ChatRunner& r) {
        Message more = UserMessage("And this");
        AttachToMessage(more, {{"photo.jpg", "image/jpeg", "JPG"}});
        r.Enqueue(more);
    });
    CHECK(wf.outcome == ChatListener::Outcome::Done);
    bool sawBoth = false, sawQueued = false;
    for (const auto& m : wf.messages) {
        sawBoth = sawBoth || m.content == "Saw image image/png and document application/pdf with: What are these?";
        sawQueued = sawQueued || m.content == "Got: And this with image image/jpeg";
    }
    CHECK(sawBoth);
    CHECK(sawQueued);

    std::cout << "  codex: message sent while it works\n";
    ChatTurn cxq;
    cxq.provider = codex;
    cxq.request.messages = {UserMessage("Thanks")};
    TestListener cxl = Drive(cxq, [](ChatRunner& r) { r.Enqueue(UserMessage("And again")); });
    CHECK(cxl.outcome == ChatListener::Outcome::Done);
    CHECK_EQ(cxl.delivered, 1);
    CHECK(!cxl.messages.empty() && cxl.messages.back().content.find("Resumed thread-1") != std::string::npos);
}

// A screenshot in a tool result reaches each API where it wants it (the mock
// answers "I see the image." only then).
static void TestImageRequests(const std::string& base) {
    std::cout << "Screenshots in requests:\n";
    struct Case {
        const char* kind;
        const char* path;
        const char* model;
    };
    for (const Case& c : {Case{"openrouter", "/openai", "mock-model"}, Case{"anthropic", "/anthropic", "claude-opus-5-5"},
                          Case{"gemini", "/gemini", "gemini-mock"}, Case{"ollama", "/ollama", "mock:latest"}}) {
        ProviderConfig config = MockProvider(base, c.kind, c.path);
        ChatRequest request;
        request.model = c.model;
        Message user;
        user.role = Role::User;
        user.content = "What is on screen?";
        Message call;
        call.role = Role::Assistant;
        call.toolCalls.push_back({"call_9", "look", "{}"});
        Message result;
        result.role = Role::Tool;
        result.toolCallId = "call_9";
        result.toolName = "look";
        result.content = "Window: Notepad";
        result.images.push_back({"image/jpeg", Base64Encode("not really a jpeg")});
        request.messages = {user, call, result};
        ChatResult r = CreateProvider(config)->Chat(request, StreamCallbacks(), nullptr);
        if (!r.ok) std::cerr << "    " << c.kind << " error: " << r.error << "\n";
        CHECK_EQ(r.text, std::string("I see the image."));
        std::cout << "  " << c.kind << ": " << r.text << "\n";

        // What the user attached: an image and a PDF, each as this API takes them.
        Message attached;
        attached.role = Role::User;
        attached.content = "What are these?";
        AttachToMessage(attached, {{"chart.png", "image/png", "\x89PNG\r\n\x1a\nfake"}, {"report.pdf", "application/pdf", "%PDF-1.4 fake"}});
        ChatRequest withFiles;
        withFiles.model = c.model;
        withFiles.messages = {attached};
        ChatResult a = CreateProvider(config)->Chat(withFiles, StreamCallbacks(), nullptr);
        if (!a.ok) std::cerr << "    " << c.kind << " error: " << a.error << "\n";
        CHECK_EQ(a.text, std::string("I see the attachments."));
        std::cout << "  " << c.kind << " attachments: " << a.text << "\n";
    }
#ifdef _WIN32
    // A whole computer use turn: the model looks (the real accessibility tree
    // and a real screenshot, sent only to the local mock), sees the image, answers.
    {
        std::mutex mutex;
        std::deque<std::function<void()>> queue;
        auto runner = std::make_shared<ChatRunner>([&](std::function<void()> fn) {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(std::move(fn));
        });
        TestListener listener;
        runner->SetListener(&listener);
        ChatTurn turn;
        turn.provider = MockProvider(base, "openrouter", "/openai");
        turn.request.model = "mock-model";
        Message user;
        user.role = Role::User;
        user.content = "What is on my screen?";
        turn.request.messages.push_back(user);
        turn.computer = true;
        turn.vision = true;
        turn.toolContext.commandApproval = "ask";
        runner->Start(turn);
        for (int i = 0; i < 3000 && !listener.finished; i++) {
            std::deque<std::function<void()>> work;
            {
                std::lock_guard<std::mutex> lock(mutex);
                work.swap(queue);
            }
            for (auto& fn : work) fn();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        runner->SetListener(nullptr);
        if (!listener.error.empty()) std::cerr << "    computer use error: " << listener.error << "\n";
        CHECK(listener.outcome == ChatListener::Outcome::Done);
        CHECK_EQ(listener.messages.size(), size_t{3});
        if (listener.messages.size() == 3) {
            CHECK_EQ(listener.messages[0].toolCalls[0].name, std::string("look"));
            CHECK_EQ(listener.messages[1].images.size(), size_t{1});
            CHECK(listener.messages[1].content.find("Window:") != std::string::npos);
            CHECK_EQ(listener.messages[2].content, std::string("I see the image."));
            std::cout << "  computer use turn: " << listener.messages[1].reasoning << "\n";
        }
        CHECK(listener.approvals.empty());  // looking never asks
    }
#endif
    // Only the latest screenshots are sent.
    std::vector<Message> history(4);
    for (auto& m : history) {
        m.role = Role::Tool;
        m.images.push_back({"image/jpeg", "x"});
    }
    Message attached;  // the user's own image, older than all of them, stays
    attached.images.push_back({"image/png", "y", "photo.png"});
    history.insert(history.begin(), attached);
    LimitImages(history, 2);
    CHECK(!history[0].images.empty());
    CHECK(history[1].images.empty() && history[2].images.empty());
    CHECK(!history[3].images.empty() && !history[4].images.empty());
    CHECK(history[1].content.find("left out") != std::string::npos);
}

#ifdef _WIN32
#include <windows.h>

// The desktop layer against a small window of our own, through accessibility
// only (no keystrokes or clicks reach the real desktop).
static std::atomic<bool> g_buttonPressed{false};

static LRESULT CALLBACK TestWindowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND && LOWORD(w) == 101 && HIWORD(w) == BN_CLICKED) g_buttonPressed = true;
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, msg, w, l);
}

static void TestDesktop() {
    std::cout << "Desktop (computer use):\n";
    std::atomic<HWND> window{nullptr};
    std::atomic<HWND> edit{nullptr};
    std::thread ui([&]() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TestWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FastChatTestWindow";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        HWND h = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"FastChat test window", WS_OVERLAPPEDWINDOW, 0, 0,
                                 360, 160, nullptr, nullptr, wc.hInstance, nullptr);
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 200, 24, h,
                                 reinterpret_cast<HMENU>(100), wc.hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Press me", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 50, 100, 30, h,
                        reinterpret_cast<HMENU>(101), wc.hInstance, nullptr);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        edit = e;
        window = h;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    while (!window) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        auto desktop = Desktop::Create();
        CHECK(desktop != nullptr);
        desktop->SetTargetHandle(window.load());
        std::string tree = desktop->Tree(200);
        CHECK(tree.find("FastChat test window") != std::string::npos);
        // Find the numbers of the edit and the button.
        int editId = 0, buttonId = 0;
        for (const auto& line : SplitLines(tree)) {
            size_t open = line.find('['), close = line.find(']');
            if (open == std::string::npos || close == std::string::npos) continue;
            int id = std::stoi(line.substr(open + 1, close - open - 1));
            if (line.find("] edit") != std::string::npos && !editId) editId = id;
            if (line.find("button \"Press me\"") != std::string::npos) buttonId = id;
        }
        CHECK(editId > 0);
        CHECK(buttonId > 0);
        std::string message;
        CHECK(desktop->SetText(editId, "hello from FastChat", message));
        wchar_t text[64] = L"";
        GetWindowTextW(edit.load(), text, 64);
        CHECK(std::wstring(text) == L"hello from FastChat");
        CHECK(desktop->Invoke(buttonId, message));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(g_buttonPressed.load());
        CHECK_EQ(desktop->ElementDescription(buttonId), std::string("Press me button"));
        int x = 0, y = 0;
        CHECK(desktop->ElementPoint(buttonId, x, y));
        Screenshot shot;
        std::string error;
        CHECK(desktop->Capture(shot, error));
        CHECK(shot.width > 0 && shot.width <= 1280 && shot.height <= 800);
        std::string jpeg = Base64Decode(shot.base64);
        CHECK(jpeg.size() > 1000 && static_cast<unsigned char>(jpeg[0]) == 0xFF && static_cast<unsigned char>(jpeg[1]) == 0xD8);
        std::cout << "  tree: " << SplitLines(tree).size() << " lines; screenshot " << shot.width << " x " << shot.height << ", "
                  << FormatBytes(jpeg.size()) << "\n";

        // The tools: looking is free; acting asks, and a "no" stops it.
        ComputerContext ctx;
        ctx.desktop = desktop.get();
        ctx.vision = true;
        std::vector<std::string> asked;
        bool allow = false;
        ctx.approve = [&](const std::string& what) {
            asked.push_back(what);
            return allow;
        };
        ToolOutcome look = RunComputerTool({"1", "look", "{}"}, ctx);
        CHECK(!look.error);
        CHECK_EQ(look.images.size(), size_t{1});
        CHECK(asked.empty());
        g_buttonPressed = false;
        ToolOutcome no = RunComputerTool({"2", "invoke", JsonDump({{"element", buttonId}})}, ctx);
        CHECK(no.error);
        CHECK(!g_buttonPressed.load());
        CHECK_EQ(asked.size(), size_t{1});
        if (!asked.empty()) CHECK_EQ(asked[0], std::string("Press Press me button"));
        allow = true;
        ToolOutcome yes = RunComputerTool({"3", "invoke", JsonDump({{"element", buttonId}})}, ctx);
        CHECK(!yes.error);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(g_buttonPressed.load());
        ctx.approval = "deny";
        ToolOutcome denied = RunComputerTool({"4", "set_text", JsonDump({{"element", editId}, {"text", "x"}})}, ctx);
        CHECK(denied.error);
        CHECK(IsComputerTool("press_keys") && !IsComputerTool("read_file"));

        // Real keystrokes, only when asked: they take the foreground for a moment.
        // The desktop refuses to type unless the test window is really in front.
        if (const char* input = std::getenv("FASTCHAT_TEST_INPUT"); input && *input) {
            std::string msg;
            desktop->SetText(editId, "", msg);
            desktop->FocusElement(editId, msg);
            if (desktop->TypeText("abc", msg)) {
                CHECK(desktop->PressKeys("shift+home", msg));
                CHECK(desktop->TypeText("Hi there", msg));
                CHECK(desktop->PressKeys("ctrl+a ctrl+c", msg));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                wchar_t typed[64] = L"";
                GetWindowTextW(edit.load(), typed, 64);
                CHECK(std::wstring(typed) == L"Hi there");
                g_buttonPressed = false;
                int bx = 0, by = 0;
                desktop->ElementPoint(buttonId, bx, by);
                CHECK(desktop->Click(bx, by, "left", 1, msg));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                CHECK(g_buttonPressed.load());
                std::cout << "  keyboard and mouse: typed, selected, clicked\n";
            } else {
                std::cout << "  keyboard and mouse: skipped (" << msg << ")\n";
            }
        }
    }
    PostMessageW(window.load(), WM_CLOSE, 0, 0);
    ui.join();
}
#endif

#ifdef __APPLE__
// The Mac desktop layer, without touching the screen: it can be made, measures
// the display, lists windows, and refuses unknown elements. Reading other apps
// needs the Accessibility permission for whatever runs the tests (Terminal...);
// without it, it says so instead.
static void TestDesktopMac() {
    std::cout << "Desktop (computer use):\n";
    auto desktop = Desktop::Create();
    CHECK(desktop != nullptr);
    if (!desktop) return;
    std::string screen = desktop->ScreenDescription();
    CHECK(screen.find(" x ") != std::string::npos);
    std::string why = desktop->Unavailable();
    if (!why.empty()) {
        CHECK(why.find("Accessibility") != std::string::npos);
        std::cout << "  screen " << screen << "; no Accessibility permission, so only that\n";
        return;
    }
    std::string windows = desktop->ListWindows();
    CHECK(!windows.empty());
    CHECK(windows.find("FastChat test") == std::string::npos);
    std::string tree = desktop->Tree(150);
    CHECK(!tree.empty());
    if (std::getenv("FASTCHAT_TEST_SHOW_TREE")) std::cout << tree;
    std::string message;
    CHECK(!desktop->Invoke(9999, message));
    CHECK(message.find("no element") != std::string::npos);
    ComputerContext ctx;
    ctx.desktop = desktop.get();
    ctx.approval = "deny";
    ToolOutcome denied = RunComputerTool({"1", "press_keys", JsonDump({{"keys", "cmd+s"}})}, ctx);
    CHECK(denied.error);
    std::cout << "  screen " << screen << "; " << SplitLines(windows).size() << " windows; tree of "
              << desktop->TargetDescription() << ": " << SplitLines(tree).size() << " lines\n";

    // Real keystrokes, only when asked: a new TextEdit document comes to the
    // front, is typed in, and is closed without saving.
    if (const char* input = std::getenv("FASTCHAT_TEST_INPUT"); input && *input) {
        RunShellCommand("osascript -e 'tell application \"TextEdit\" to activate' "
                        "-e 'tell application \"TextEdit\" to make new document'",
                        "", 15000, 4096, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        auto d = Desktop::Create();
        std::string look = d->Tree(200);
        if (std::getenv("FASTCHAT_TEST_SHOW_TREE")) std::cout << look;
        CHECK(look.find("(TextEdit)") != std::string::npos);
        CHECK(look.find("Menu bar:") != std::string::npos);
        int area = 0;
        for (const auto& line : SplitLines(look)) {
            size_t open = line.find('['), close = line.find(']');
            if (open != std::string::npos && close != std::string::npos && line.find("] text entry area") != std::string::npos) {
                area = std::stoi(line.substr(open + 1, close - open - 1));
                break;
            }
        }
        CHECK(area > 0);
        std::string msg;
        CHECK(d->SetText(area, "set through accessibility", msg));
        CHECK(d->ElementDescription(area).find("text entry area") != std::string::npos);
        CHECK(d->FocusElement(area, msg));
        CHECK(d->PressKeys("cmd+a", msg));
        CHECK(d->TypeText("Hi there\nsecond line", msg));
        CHECK(d->PressKeys("shift+up", msg));
        std::string focus = d->FocusedDescription();
        CHECK(focus.find("Hi there\nsecond line") != std::string::npos || focus.find("Hi there") != std::string::npos);
        int x = 0, y = 0;
        CHECK(d->ElementPoint(area, x, y));
        CHECK(d->Click(x, y, "left", 1, msg));
        Screenshot shot;
        std::string error;
        bool captured = d->Capture(shot, error);
        if (captured) {
            std::string jpeg = Base64Decode(shot.base64);
            CHECK(jpeg.size() > 1000 && static_cast<unsigned char>(jpeg[0]) == 0xFF);
        }
        std::cout << "  keyboard and mouse in TextEdit: " << focus << "; screenshot: "
                  << (captured ? std::to_string(shot.width) + " x " + std::to_string(shot.height) : error) << "\n";
        RunShellCommand("osascript -e 'tell application \"TextEdit\" to close front document saving no'", "", 15000, 4096,
                        nullptr);
    }
}
#endif

// Sound packs in the (test) data folder: lookup, variants, and what wins.
static void TestSoundPacks() {
    std::string packs = UserSoundPacksFolder();
    CHECK(StartsWith(packs, GetDataDir()));
    std::string pack = JoinPath(packs, "Test Pack");
    fs::create_directories(PathFromUtf8(pack));
    for (const char* name : {"tool_call.wav", "tool_call-edit.wav", "tool_call-command-git.wav",
                             "tool_call-command-git-commit.WAV", "response_complete.ogg", "notes.txt"}) {
        WriteFileText(JoinPath(pack, name), "");
    }
    ForgetSoundPackFiles();
    bool listed = false;
    for (const auto& p : ListSoundPacks()) listed = listed || p.name == "Test Pack";
    CHECK(listed);
    CHECK_EQ(FindSoundPackFolder("test pack"), pack);
    CHECK(FindSoundPackFolder("No such pack").empty());
    CHECK(FindSoundPackFolder("").empty());

    CHECK_EQ(ToolKind("Bash"), std::string("command"));
    CHECK_EQ(ToolKind("edit_file"), std::string("edit"));
    CHECK_EQ(ToolKind("MultiEdit"), std::string("edit"));
    CHECK_EQ(ToolKind("Glob"), std::string("list"));
    CHECK_EQ(ToolKind("mcp__github__search"), std::string("mcp"));
    CHECK_EQ(ToolKind("click"), std::string());

    auto words = [](const std::string& c) {
        std::string out;
        for (const auto& w : CommandWords(c)) out += (out.empty() ? "" : " ") + w;
        return out;
    };
    CHECK_EQ(words("git commit -m \"a && b\""), std::string("git commit"));
    CHECK_EQ(words("cd /d C:\\src && \"C:\\Program Files\\Python\\python.exe\" -m pytest"), std::string("python m"));
    CHECK_EQ(words("git -C C:\\stuff\\FastChat commit -m x"), std::string("git commit"));
    CHECK_EQ(words("git --no-pager -c core.pager=less log"), std::string("git log"));
    CHECK_EQ(words("Set-Location C:\\x; git status --short"), std::string("git status"));
    CHECK_EQ(words("set FOO=1 & build.bat release"), std::string("build release"));
    CHECK_EQ(words("FOO=1 make -j4"), std::string("make j4"));
    CHECK_EQ(words("git"), std::string("git"));
    CHECK_EQ(words("npm run build 2>&1 | tail -5"), std::string("npm run"));
    CHECK_EQ(words("python script.py"), std::string("python"));
    CHECK_EQ(words("cd src"), std::string());

    std::vector<std::string> v = ToolSoundVariants("Bash", "git commit -m x");
    CHECK(v.size() == 4 && v[0] == "command-git-commit" && v[1] == "command-git" && v[2] == "bash" && v[3] == "command");
    v = ToolSoundVariants("run_command", "dir");
    CHECK(v.size() == 3 && v[0] == "command-dir" && v[1] == "run_command" && v[2] == "command");
    v = ToolSoundVariants("edit", "");
    CHECK(v.size() == 1 && v[0] == "edit");

    // The most specific sound the pack has; the plain one; none (the tone).
    EventConfig c;
    auto file = [&](EventId id, const std::string& tool, const std::string& command) {
        std::string f = EventSoundFile(id, c, "Test Pack", ToolSoundVariants(tool, command));
        return PathToUtf8(PathFromUtf8(f).filename());
    };
    CHECK_EQ(file(EventId::ToolCall, "Bash", "git commit -m x"), std::string("tool_call-command-git-commit.WAV"));
    CHECK_EQ(file(EventId::ToolCall, "run_command", "cd x && git push"), std::string("tool_call-command-git.wav"));
    CHECK_EQ(file(EventId::ToolCall, "edit_file", ""), std::string("tool_call-edit.wav"));
    CHECK_EQ(file(EventId::ToolCall, "read_file", ""), std::string("tool_call.wav"));
    CHECK_EQ(file(EventId::ResponseComplete, "", ""), std::string("response_complete.ogg"));
    CHECK(EventSoundFile(EventId::MessageSent, c, "Test Pack").empty());
    CHECK(EventSoundFile(EventId::ToolCall, c, "").empty());
    CHECK(PackHasEventSound(pack, "tool_call"));
    CHECK(!PackHasEventSound(pack, "tool_result"));
    // A file chosen for the event beats the pack.
    c.soundFile = "C:\\mine.wav";
    CHECK_EQ(EventSoundFile(EventId::ToolCall, c, "Test Pack"), std::string("C:\\mine.wav"));

    // A conversation's own pack, "" for none, or the global one.
    AppSettings().sounds.pack = "Test Pack";
    ConversationSettings s;
    CHECK_EQ(ResolveSoundPack(&s), std::string("Test Pack"));
    s.soundPack = std::string();
    ConversationSettings back = ConversationSettingsFromJson(ConversationSettingsToJson(s));
    CHECK(back.soundPack.has_value() && back.soundPack->empty());
    CHECK(ResolveSoundPack(&back).empty());
    CHECK(!ConversationSettingsFromJson(ConversationSettingsToJson(ConversationSettings())).soundPack.has_value());
    AppSettings().sounds.pack.clear();
    CHECK(EventFromKey("tool_failed") == EventId::ToolFailed);
}

// A zip with these files (name -> content), compressed.
static void WriteZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip{};
    CHECK(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : files) {
        CHECK(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
    }
    void* data = nullptr;
    size_t size = 0;
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    CHECK(WriteFileText(path, std::string(static_cast<const char*>(data), size)));
    mz_free(data);  // the finished archive is ours to free
    mz_zip_writer_end(&zip);
}

static void TestSoundPackImport(const std::string& dir) {
    std::cout << "Sound pack import:\n";
    std::string packs = UserSoundPacksFolder();

    // Sounds in one folder: the pack takes the folder's name; paths are dropped.
    std::string zip1 = JoinPath(dir, "download.zip");
    WriteZip(zip1, {{"My Pack/response_complete.wav", "RIFF1"},
                    {"My Pack/tools/tool_call-edit.ogg", "OggS"},
                    {"My Pack/readme.txt", "not a sound"},
                    {"__MACOSX/My Pack/._response_complete.wav", "fork"}});
    CHECK_EQ(SoundPackNameForZip(zip1), std::string("My Pack"));
    SoundPackImport r = ImportSoundPackZip(zip1, SoundPackNameForZip(zip1));
    CHECK(r.ok);
    CHECK_EQ(r.sounds, 2);
    std::string folder = JoinPath(packs, "My Pack");
    CHECK_EQ(r.folder, folder);
    CHECK(fs::exists(PathFromUtf8(JoinPath(folder, "response_complete.wav"))));
    CHECK(fs::exists(PathFromUtf8(JoinPath(folder, "tool_call-edit.ogg"))));
    CHECK(!fs::exists(PathFromUtf8(JoinPath(folder, "readme.txt"))));
    CHECK(!fs::exists(PathFromUtf8(JoinPath(folder, "._response_complete.wav"))));
    std::string text;
    CHECK(ReadFileText(JoinPath(folder, "response_complete.wav"), text) && text == "RIFF1");
    CHECK_EQ(FindSoundPackFolder("My Pack"), folder);  // found straight away

    // Sounds at the top: the zip's name. A path climbing out stays inside the pack.
    std::string zip2 = JoinPath(dir, "Workshop.zip");
    WriteZip(zip2, {{"message_sent.wav", "a"}, {"../escaped.wav", "b"}});
    CHECK_EQ(SoundPackNameForZip(zip2), std::string("Workshop"));
    r = ImportSoundPackZip(zip2, "Workshop");
    CHECK(r.ok && r.sounds == 2);
    CHECK(fs::exists(PathFromUtf8(JoinPath(JoinPath(packs, "Workshop"), "escaped.wav"))));
    CHECK(!fs::exists(PathFromUtf8(JoinPath(packs, "escaped.wav"))));

    // Importing again replaces the pack: files the new zip lacks are gone.
    WriteZip(zip2, {{"compacted.wav", "c"}});
    r = ImportSoundPackZip(zip2, "Workshop");
    CHECK(r.ok && r.sounds == 1);
    CHECK(!fs::exists(PathFromUtf8(JoinPath(JoinPath(packs, "Workshop"), "message_sent.wav"))));
    CHECK(fs::exists(PathFromUtf8(JoinPath(JoinPath(packs, "Workshop"), "compacted.wav"))));

    // No sounds, or not a zip: an error, and the pack already there is kept.
    std::string zip3 = JoinPath(dir, "empty.zip");
    WriteZip(zip3, {{"notes.txt", "x"}});
    r = ImportSoundPackZip(zip3, "Workshop");
    CHECK(!r.ok && !r.error.empty());
    CHECK(fs::exists(PathFromUtf8(JoinPath(JoinPath(packs, "Workshop"), "compacted.wav"))));
    std::string notZip = JoinPath(dir, "not-a.zip");
    WriteFileText(notZip, "plain text");
    r = ImportSoundPackZip(notZip, "Workshop");
    CHECK(!r.ok);
    CHECK(fs::exists(PathFromUtf8(JoinPath(JoinPath(packs, "Workshop"), "compacted.wav"))));
    // The unpacking folder never shows as a pack.
    for (const auto& p : ListSoundPacks()) CHECK(!StartsWith(p.name, "."));

    // A real pack's zip, when asked for (unpacked into the test data folder).
    if (const char* real = std::getenv("FASTCHAT_TEST_PACK_ZIP"); real && *real) {
        std::string name = SoundPackNameForZip(real);
        SoundPackImport ri = ImportSoundPackZip(real, name.empty() ? std::string("Real") : name);
        if (!ri.ok) std::cerr << "    error: " << ri.error << "\n";
        CHECK(ri.ok && ri.sounds > 0);
        std::cout << "  " << real << ": pack " << name << ", " << ri.sounds << " sounds\n";
    }
}

// The updater: reading the latest release, where it may install, and the script
// that puts a new build in place.
static void TestUpdater(const std::string& dir) {
    std::cout << "Updater:\n";
    std::string commit = BuildCommit();
    std::cout << "  this build: " << (commit.empty() ? "(outside git)" : commit) << "\n";
    CHECK(commit.empty() || commit.size() == 7);

    json release = {{"tag_name", "latest"},
                    {"target_commitish", "0123456789abcdef0123456789abcdef01234567"},
                    {"body", "Built from 0123456789abcdef0123456789abcdef01234567: Attach files."},
                    {"assets",
                     {{{"name", "FastChat.zip"}, {"browser_download_url", "https://example.com/FastChat.zip"}},
                      {{"name", "FastChat-mac.zip"}, {"browser_download_url", "https://example.com/FastChat-mac.zip"}}}}};
    UpdateInfo u = ReadLatestRelease(release.dump(), "abcdef1", "FastChat.zip");
    CHECK(u.error.empty() && u.available);
    CHECK_EQ(u.commit, std::string("0123456"));
    CHECK_EQ(u.downloadUrl, std::string("https://example.com/FastChat.zip"));
    CHECK(u.notes.find("Attach files.") != std::string::npos);
    CHECK_EQ(ReadLatestRelease(release.dump(), "abcdef1", "FastChat-mac.zip").downloadUrl, std::string("https://example.com/FastChat-mac.zip"));
    CHECK(!ReadLatestRelease(release.dump(), "0123456", "FastChat.zip").available);  // already this build
    CHECK(!ReadLatestRelease(release.dump(), "", "FastChat.zip").available);         // made outside git
    CHECK(!ReadLatestRelease(release.dump(), "abcdef1", "FastChat-linux.zip").error.empty());  // nothing for it
    json byText = release;
    byText["target_commitish"] = "master";  // the commit then comes from the text
    byText["body"] = "Automated build of master (fedcba9876543210).";
    CHECK_EQ(ReadLatestRelease(byText.dump(), "abcdef1", "FastChat.zip").commit, std::string("fedcba9"));
    CHECK(!ReadLatestRelease("not json", "abcdef1", "FastChat.zip").error.empty());

    // Never over a source folder (a build of one's own), nor where it cannot write.
    std::string source = JoinPath(dir, "update-source");
    fs::create_directories(PathFromUtf8(source));
    WriteFileText(JoinPath(source, "CMakeLists.txt"), "project(x)");
    CHECK(UpdateBlocker(source).find("source folder") != std::string::npos);
    std::string plain = JoinPath(dir, "update-plain");
    fs::create_directories(PathFromUtf8(plain));
    CHECK(UpdateBlocker(plain).empty());
    CHECK(!UpdateBlocker(JoinPath(dir, "no-such-folder")).empty());

    // The real thing, when asked for: GitHub's latest build of FastChat.
    if (const char* real = std::getenv("FASTCHAT_REAL_UPDATE"); real && *real) {
        UpdateInfo latest = CheckForUpdate();
        if (!latest.error.empty()) std::cerr << "    error: " << latest.error << "\n";
        CHECK(latest.error.empty() && !latest.downloadUrl.empty());
        std::cout << "  GitHub's latest build: " << latest.commit << (latest.available ? ", newer than this one" : ", this one")
                  << "; package " << latest.downloadUrl << "\n";
    }

    std::string mac = MacUpdateScript(42, "/tmp/FastChat-mac.zip", "/tmp/x", "/Applications/Fast Chat's.app");
    CHECK(mac.find("kill -0 42") != std::string::npos);
    CHECK(mac.find("ditto -x -k '/tmp/FastChat-mac.zip' '/tmp/x'") != std::string::npos);
    CHECK(mac.find("open '/Applications/Fast Chat'\\''s.app'") != std::string::npos);  // quoted for the shell

#ifdef _WIN32
    // The Windows script for real (without starting FastChat at the end): the
    // package's FastChat folder replaces the files, and what it lacks stays.
    std::string root = JoinPath(dir, "update-run");
    std::string app = JoinPath(root, "app folder's copy");
    fs::create_directories(PathFromUtf8(app));
    WriteFileText(JoinPath(app, "FastChat.exe"), "old exe");
    WriteFileText(JoinPath(app, "README.md"), "old readme");
    WriteFileText(JoinPath(app, "portable.txt"), "stays");
    std::string zip = JoinPath(root, "FastChat.zip");
    WriteZip(zip, {{"FastChat/FastChat.exe", "new exe"}, {"FastChat/README.md", "new readme"}});
    std::string script = WindowsUpdateScript(999999, zip, JoinPath(root, "extract"), app);  // no such process: no wait
    CHECK(script.find("start \"\" \"" + app + "\\FastChat.exe\"") != std::string::npos);
    std::string withoutStart;
    for (const auto& line : SplitLines(script)) {
        if (!StartsWith(line, "start ")) withoutStart += line + "\n";
    }
    std::string bat = JoinPath(root, "update.bat");
    WriteFileText(bat, withoutStart);
    ProcessResult run = RunShellCommand("\"" + bat + "\"", root, 120000, 100000, nullptr);
    CHECK(run.started && !run.timedOut);
    std::string text;
    CHECK(ReadFileText(JoinPath(app, "FastChat.exe"), text) && text == "new exe");
    CHECK(ReadFileText(JoinPath(app, "README.md"), text) && text == "new readme");
    CHECK(ReadFileText(JoinPath(app, "portable.txt"), text) && text == "stays");
    CHECK(!fs::exists(PathFromUtf8(JoinPath(root, "extract"))));
    CHECK(!fs::exists(PathFromUtf8(zip)));
    CHECK(!fs::exists(PathFromUtf8(bat)));  // it deletes itself
    if (!run.output.empty() && !(text == "stays")) std::cerr << "    script output: " << run.output << "\n";
#endif
}

// Files attached to a message: what they are, what is refused, where they go.
static void TestAttachments(const std::string& dir) {
    std::cout << "Attachments:\n";
    CHECK_EQ(AttachmentType("a.png", "\x89PNG\r\n\x1a\nrest"), std::string("image/png"));
    CHECK_EQ(AttachmentType("a.jpg", "\xFF\xD8\xFF\xE0rest"), std::string("image/jpeg"));
    CHECK_EQ(AttachmentType("a.gif", "GIF89a..."), std::string("image/gif"));
    CHECK_EQ(AttachmentType("a.webp", std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)), std::string("image/webp"));
    CHECK_EQ(AttachmentType("a.pdf", "%PDF-1.7"), std::string("application/pdf"));
    CHECK_EQ(AttachmentType("notes.md", "# Notes\ncaf\xC3\xA9\n"), std::string("text/plain"));
    CHECK_EQ(AttachmentType("app.exe", std::string("MZ\x90\0\x03", 5)), std::string());  // binary: refused
    CHECK_EQ(AttachmentType("latin1.txt", "caf\xE9"), std::string());                     // not UTF-8 text

    Attachment a;
    std::string error;
    CHECK(!MakeAttachment("big.png", "\x89PNG\r\n\x1a\n" + std::string(kMaxImageBytes, 'x'), a, error));
    CHECK(error.find("big.png") != std::string::npos && error.find("up to") != std::string::npos);
    CHECK(!MakeAttachment("tool.exe", std::string("MZ\0\0", 4), a, error));
    CHECK(error.find("images (PNG, JPEG, GIF, WebP), PDFs and text files") != std::string::npos);
    CHECK(MakeAttachment("bom.txt", "\xEF\xBB\xBFhello", a, error));
    CHECK_EQ(a.data, std::string("hello"));  // the byte order mark is dropped

    std::string path = JoinPath(dir, "notes.txt");
    WriteFileText(path, "line one\nline two");
    CHECK(LoadAttachment(path, a, error) && a.name == "notes.txt" && a.IsText());
    CHECK(!LoadAttachment(JoinPath(dir, "missing.txt"), a, error));
    CHECK_EQ(DescribeAttachment(a), std::string("notes.txt, ") + FormatBytes(a.data.size()) + " text file");

    Message m;
    m.content = "Look at these";
    AttachToMessage(m, {{"pic.png", "image/png", "PNG"}, a, {"report.pdf", "application/pdf", "%PDF"}});
    CHECK_EQ(m.content, std::string("Look at these\n\n[Attached file: notes.txt]\nline one\nline two"));
    CHECK_EQ(m.images.size(), size_t{2});
    if (m.images.size() == 2) {
        CHECK(m.images[0].name == "pic.png" && Base64Decode(m.images[0].base64) == "PNG" && !m.images[0].IsPdf());
        CHECK(m.images[1].IsPdf() && m.images[1].name == "report.pdf");
    }
    CHECK_EQ(AttachmentsSuffix(m), std::string(", with pic.png and report.pdf"));
    Message shot;
    shot.images.push_back({"image/jpeg", "x"});
    CHECK_EQ(AttachmentsSuffix(shot), std::string(", with an image"));  // a screenshot has no name
    CHECK_EQ(AttachmentsSuffix(Message()), std::string());

    // Names stay with them in the database.
    Database db;
    CHECK(db.Open(JoinPath(dir, "attach.db")));
    Conversation c;
    std::vector<Message> one = {m};
    CHECK(db.CreateConversationWithMessages(c, one));
    std::vector<Message> back = db.LoadMessages(c.id);
    CHECK(back.size() == 1 && back[0].images.size() == 2 && back[0].images[1].name == "report.pdf" && back[0].images[1].IsPdf());
    db.Close();
}

// Basilisk conversations (.bskc: a zip with conversation.json and attachments).
static void TestBasiliskImport(const std::string& dir) {
    std::cout << "Basilisk import:\n";
    CHECK_EQ(ParseIsoTime("1970-01-01T00:00:10Z"), int64_t{10});
    CHECK_EQ(ParseIsoTime("1970-01-01T01:00:10+01:00"), int64_t{10});
    CHECK_EQ(ParseIsoTime("1970-01-01T00:00:10.123-00:30"), int64_t{1810});
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = 10;
    tm.tm_mday = 8;
    tm.tm_hour = 16;
    tm.tm_min = 15;
    tm.tm_sec = 20;
    tm.tm_isdst = -1;
    int64_t local = static_cast<int64_t>(std::mktime(&tm));
    CHECK_EQ(ParseIsoTime("2026-11-08T16:15:20.483556"), local);  // no zone: local time
    CHECK_EQ(ParseIsoTime("not a time"), int64_t{0});

    json model = {{"provider_id", "openrouter"}, {"model_id", "google/gemini-4-argon"}};
    json conv = {
        {"version", 3},
        {"title", nullptr},
        {"systems", {{{"role", "system"}, {"content", "Be brief."}}, {{"role", "system"}, {"content", "Be Taylor."}}}},
        {"messages",
         {{{"request", {{"role", "user"}, {"content", "hello"}, {"attachments", json::array()}, {"citations", nullptr}}},
           {"response", {{"role", "assistant"}, {"content", "hi"}, {"attachments", nullptr}, {"citations", nullptr}}},
           {"system_index", 0}, {"model", model}, {"temperature", 0.7}, {"max_tokens", 0}, {"top_p", 1.0}, {"stream", true},
           {"created_at", "2026-11-08T16:15:20.483556"}, {"updated_at", "2026-11-08T16:15:25.000001"}},
          {{"request",
            {{"role", "user"},
             {"content", "look"},
             {"attachments",
              {{{"location", {{"protocol", "zip"}, {"path", "attachments/pic.png"}, {"storage_options", json::object()}}},
                {"name", "pic.png"}, {"mime_type", "image/png"}, {"description", "a cat"}},
               {{"location", {{"protocol", "zip"}, {"path", "attachments/notes.txt"}, {"storage_options", json::object()}}},
                {"name", "notes.txt"}, {"mime_type", "text/plain"}},
               {{"location", "https://example.com/a.png"}, {"name", "a.png"}, {"mime_type", "image/png"}},
               {{"location", {{"protocol", "zip"}, {"path", "attachments/doc.pdf"}, {"storage_options", json::object()}}},
                {"name", "doc.pdf"}, {"mime_type", "application/pdf"}}}}}},
           {"response", {{"role", "assistant"}, {"content", "a cat"}, {"citations", {{{"title", "Cats"}, {"url", "https://cats.example"}}}}}},
           {"system_index", 1}, {"model", model}, {"temperature", 0.7}, {"max_tokens", 0},
           {"created_at", "2026-11-08T16:16:00"}, {"updated_at", "2026-11-08T16:16:05"}},
          {{"request", {{"role", "user"}, {"content", "bye"}, {"attachments", nullptr}}},
           {"response", nullptr},
           {"system_index", 1}, {"model", {{"provider_id", "unknownai"}, {"model_id", "x-1"}}}, {"temperature", 0.7},
           {"max_tokens", 2000}, {"created_at", "2026-11-08T16:17:00"}}}}};
    std::string bskc = JoinPath(dir, "Chat with Taylor.bskc");
    WriteZip(bskc, {{"conversation.json", conv.dump()},
                    {"attachments/pic.png", "PNG!"},
                    {"attachments/notes.txt", "hello"},
                    {"attachments/doc.pdf", "%PDF"}});

    ImportedConversation imp;
    std::string error;
    CHECK(ImportConversationFile(bskc, imp, error));
    if (!error.empty()) std::cerr << "    error: " << error << "\n";
    const Conversation& c = imp.conversation;
    CHECK_EQ(imp.source, std::string("Basilisk"));
    CHECK_EQ(c.title, std::string("Chat with Taylor"));  // no title: the file's name
    CHECK(c.type == ConversationType::Chat);
    CHECK_EQ(c.settings.systemPrompt.value_or("(none)"), std::string("Be Taylor."));  // the last exchange's
    CHECK(c.settings.temperature && std::abs(*c.settings.temperature - 0.7) < 1e-9);
    CHECK_EQ(c.settings.maxTokens.value_or(0), 2000);
    CHECK(c.settings.providerId.empty());  // no provider of those kinds here
    CHECK_EQ(c.createdAt, local);
    CHECK_EQ(imp.exchanges, 3);
    const auto& ms = imp.messages;
    CHECK_EQ(ms.size(), size_t{8});
    if (ms.size() == 8) {
        // Where it came from, and what did not come over.
        CHECK(GetNoteKind(ms[0]) == NoteKind::Note);
        CHECK(ms[0].content.find("Imported from Basilisk: 3 messages") != std::string::npos);
        CHECK(ms[0].content.find("unknownai") != std::string::npos);
        // The first exchange had another system prompt: kept in a note.
        CHECK(GetNoteKind(ms[1]) == NoteKind::Note && ms[1].content.find("Be brief.") != std::string::npos);
        CHECK(ms[2].role == Role::User && ms[2].content == "hello" && ms[2].createdAt == local);
        CHECK(ms[3].role == Role::Assistant && ms[3].content == "hi" && ms[3].model == "google/gemini-4-argon");
        CHECK_EQ(ms[3].createdAt, local + 5);
        CHECK(GetNoteKind(ms[4]) == NoteKind::Note && ms[4].content.find("the one this conversation uses now") != std::string::npos);
        // Attachments: images and PDFs go with the message, text files into it, links named.
        CHECK(ms[5].role == Role::User);
        CHECK(StartsWith(ms[5].content, "look"));
        CHECK(ms[5].content.find("[Attached file: notes.txt]\nhello") != std::string::npos);
        CHECK(ms[5].content.find("[Attached: https://example.com/a.png]") != std::string::npos);
        CHECK_EQ(ms[5].images.size(), size_t{2});
        if (ms[5].images.size() == 2) {
            CHECK_EQ(ms[5].images[0].mediaType, std::string("image/png"));
            CHECK_EQ(ms[5].images[0].name, std::string("pic.png"));
            CHECK_EQ(Base64Decode(ms[5].images[0].base64), std::string("PNG!"));
            CHECK(ms[5].images[1].IsPdf() && ms[5].images[1].name == "doc.pdf");
        }
        CHECK_EQ(ms[6].content, std::string("a cat\n\nSources:\n- Cats: https://cats.example"));
        CHECK(ms[7].role == Role::User && ms[7].content == "bye");  // no reply came
    }
    // Notes are never sent; the model gets the exchanges.
    CHECK_EQ(ContextMessages(ms).size(), size_t{5});

    // Saved in one go, images and all.
    Database db;
    CHECK(db.Open(JoinPath(dir, "import.db")));
    Conversation saved = imp.conversation;
    std::vector<Message> toSave = imp.messages;
    CHECK(db.CreateConversationWithMessages(saved, toSave));
    std::vector<Message> loaded = db.LoadMessages(saved.id);
    CHECK_EQ(loaded.size(), size_t{8});
    if (loaded.size() == 8) CHECK_EQ(loaded[5].images.size(), size_t{2});
    db.Close();

    // An older Basilisk file: one system prompt for the whole conversation.
    json old = {{"version", 1},
                {"title", "Old one"},
                {"system", {{"role", "system"}, {"content", "Old prompt"}}},
                {"messages", {{{"request", {{"role", "user"}, {"content", "q"}}},
                               {"response", {{"role", "assistant"}, {"content", "a"}}},
                               {"model", {{"provider_id", "openai"}, {"model_id", "gpt-4o"}}}}}}};
    ImportedConversation oldImp;
    CHECK(ImportBasiliskJson(old.dump(), nullptr, "file", oldImp, error));
    CHECK_EQ(oldImp.conversation.title, std::string("Old one"));
    CHECK_EQ(oldImp.conversation.settings.systemPrompt.value_or("(none)"), std::string("Old prompt"));
    CHECK_EQ(oldImp.messages.size(), size_t{3});  // the note, the question, the answer
    CHECK(!oldImp.conversation.settings.temperature);  // Basilisk's default 1: left to the model

    // Not a conversation.
    std::string notOne = JoinPath(dir, "plain.bskc");
    WriteFileText(notOne, "plain text");
    CHECK(!ImportConversationFile(notOne, oldImp, error) && !error.empty());

    // A real one, when asked for: only counts are printed.
    if (const char* real = std::getenv("FASTCHAT_TEST_BSKC"); real && *real) {
        ImportedConversation r;
        bool ok = ImportConversationFile(real, r, error);
        CHECK(ok);
        if (!ok) std::cerr << "    error: " << error << "\n";
        int notes = 0, images = 0;
        for (const auto& m : r.messages) {
            if (GetNoteKind(m) != NoteKind::None) notes++;
            images += static_cast<int>(m.images.size());
        }
        std::cout << "  real file: " << r.exchanges << " exchanges, " << r.messages.size() << " rows, " << notes
                  << " notes, " << images << " images, system prompt " << (r.conversation.settings.systemPrompt ? r.conversation.settings.systemPrompt->size() : 0)
                  << " chars, provider " << (r.conversation.settings.providerId.empty() ? "none" : "matched") << "\n";
        std::cout << "  first note: " << OneLine(r.messages.empty() ? std::string() : r.messages[0].content, 400) << "\n";
    }
}

int main() {
    std::string dir = PathToUtf8(fs::temp_directory_path() / ("fastchat-test-" + NewId().substr(0, 8)));
    fs::create_directories(PathFromUtf8(dir));
    std::cout << "Test folder: " << dir << "\n";
    // Keep the model cache and anything else out of the real data folder.
#ifdef _WIN32
    _putenv_s("FASTCHAT_DATA_DIR", dir.c_str());
#else
    setenv("FASTCHAT_DATA_DIR", dir.c_str(), 1);
#endif

    std::cout << "Prism backends:";
    bool oneCore = false;
    for (const auto& name : Speech::PrismBackendNames()) {
        std::cout << " [" << name << "]";
        oneCore = oneCore || name == "OneCore";
    }
    std::cout << "\n";
    if (oneCore) {
        // OneCore runs on the Windows Runtime (C++/WinRT), which the Windows 7
        // stand-ins sit in front of: its voices still list, without speaking.
        // Getting here without a crash is the test; how many there are depends on
        // the system (Windows Server, as on CI, offers none).
        size_t voices = Speech::ListVoices("prism", "OneCore").size();
        std::cout << "OneCore voices: " << voices << "\n";
    }
    if (Speech::SystemVoicesAvailable()) {
#ifdef _WIN32
        // SAPI is COM; in FastChat wxWidgets has set COM up on the UI thread.
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        // The system's own voices, without speaking: listed, and one can be chosen.
        std::vector<std::string> voices = Speech::ListVoices("system", "");
        std::cout << Speech::SystemVoicesName() << ": " << voices.size() << "\n";
        CHECK(!voices.empty());
        auto backend = CreateSystemVoiceBackend();
        CHECK(backend && !voices.empty() && backend->SetVoice(voices.front()));
        CHECK(backend && !backend->SetVoice("No such voice"));
#ifdef _WIN32
        backend.reset();
        if (SUCCEEDED(com)) CoUninitialize();
#endif
    }
    TestUtil();
    TestSse();
    TestSecrets();
    TestDatabase(dir);
    TestCodeTools(dir);
    TestSoundPacks();
    TestSoundPackImport(dir);
    TestBasiliskImport(dir);
    TestAttachments(dir);
    TestUpdater(dir);

    // Spoken templates: an emptied placeholder leaves no dangling colon.
    EventContext ctx;
    ctx.Set("summary", "");
    CHECK_EQ(ExpandTemplate("Approve command: {summary}", ctx), std::string("Approve command"));
    ctx.Set("model", "GLM");
    CHECK_EQ(ExpandTemplate("{model} is responding", ctx), std::string("GLM is responding"));

    std::cout << "Coding agents (fake Claude Code and Codex):\n";
    TestAgents(JoinPath(dir, "project"));

#ifdef _WIN32
    TestDesktop();
#elif defined(__APPLE__)
    TestDesktopMac();
#endif

    // The real Claude Code, when asked for (it uses the account it is logged in with).
    if (const char* real = std::getenv("FASTCHAT_REAL_CLAUDE"); real && *real) {
        std::cout << "Real Claude Code:\n";
        ProviderConfig claude;
        claude.id = "real-cc";
        claude.kind = "claude_code";
        claude.name = "Claude Code";
        std::string folder = JoinPath(dir, "real");
        fs::create_directories(PathFromUtf8(folder));
        TestListener pong = RunAgentTurn(claude, {}, "Reply with just the word pong.", false, true, folder);
        if (!pong.error.empty()) std::cerr << "    error: " << pong.error << "\n";
        CHECK(pong.outcome == ChatListener::Outcome::Done);
        CHECK(ToLower(pong.text).find("pong") != std::string::npos);
        std::cout << "  chat: " << OneLine(pong.text, 80) << "\n";
        TestListener write = RunAgentTurn(claude, {}, "Create a file named notes.txt containing the word hi, using your Write tool. Then say done.", true, true, folder);
        if (!write.error.empty()) std::cerr << "    error: " << write.error << "\n";
        CHECK(write.outcome == ChatListener::Outcome::Done);
        CHECK_EQ(write.approvals.size(), size_t{1});
        CHECK(fs::exists(PathFromUtf8(JoinPath(folder, "notes.txt"))));
        std::cout << "  write: " << write.messages.size() << " messages, approvals:";
        for (const auto& a : write.approvals) std::cout << " [" << a << "]";
        std::cout << "\n";
        TestListener again = RunAgentTurn(claude, write.messages, "What file did you just create? One short sentence, no tools.", true, true, folder);
        CHECK(again.outcome == ChatListener::Outcome::Done);
        CHECK(ToLower(again.text).find("notes") != std::string::npos);
        std::cout << "  resume: " << OneLine(again.text, 100) << "\n";
        AccountInfo account;
        if (AppProviderCache().GetAccount("real-cc", account)) std::cout << "  plan: " << account.summary << "\n";
    }

    const char* mock = std::getenv("FASTCHAT_MOCK");
    if (mock && *mock) {
        std::cout << "Providers against " << mock << ":\n";
        std::string root = JoinPath(dir, "project");
        TestProvider(mock, "openrouter", "/openai", "mock-model", root);
        TestProvider(mock, "anthropic", "/anthropic", "claude-opus-5-5", root);
        TestProvider(mock, "gemini", "/gemini", "gemini-mock", root);
        TestProvider(mock, "ollama", "/ollama", "mock:latest", root);
        TestImageRequests(mock);
        TestQueueRetryCompaction(mock, JoinPath(dir, "project"));

        // OpenRouter's account details
        auto p = CreateProvider(MockProvider(mock, "openrouter", "/openai"));
        AccountInfo info = p->GetAccountInfo(nullptr);
        CHECK(info.ok);
        CHECK(std::abs(info.creditsRemaining - 15.75) < 1e-9);
        // Costs: OpenRouter reports its own; Anthropic's come from the price table.
        // An error response becomes a readable message.
        ChatResult bad = QuickChat(MockProvider(mock, "openrouter", "/openai"), "fail", "x", nullptr);
        CHECK(!bad.ok);
        CHECK(bad.error.find("does not exist") != std::string::npos);
    } else {
        std::cout << "FASTCHAT_MOCK not set: provider tests skipped.\n";
    }

    std::error_code ec;
    fs::remove_all(PathFromUtf8(dir), ec);
    std::cout << g_checks << " checks, " << g_failures << " failed\n";
    return g_failures ? 1 : 0;
}
