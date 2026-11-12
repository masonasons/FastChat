// Tests of FastChat's core: no windows. Provider tests run against
// tests/mock_server.py when FASTCHAT_MOCK names its address
// (e.g. http://127.0.0.1:8765).

#include "core/chat_engine.h"
#include "core/code_tools.h"
#include "core/database.h"
#include "core/platform.h"
#include "core/providers/provider.h"
#include "core/settings.h"
#include "core/speech.h"
#include "core/notify.h"
#include "core/sse.h"
#include "core/util.h"

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
    for (const auto& name : Speech::PrismBackendNames()) std::cout << " [" << name << "]";
    std::cout << "\n";
    TestUtil();
    TestSse();
    TestSecrets();
    TestDatabase(dir);
    TestCodeTools(dir);

    // Spoken templates: an emptied placeholder leaves no dangling colon.
    EventContext ctx;
    ctx.Set("summary", "");
    CHECK_EQ(ExpandTemplate("Approve command: {summary}", ctx), std::string("Approve command"));
    ctx.Set("model", "GLM");
    CHECK_EQ(ExpandTemplate("{model} is responding", ctx), std::string("GLM is responding"));

    std::cout << "Coding agents (fake Claude Code and Codex):\n";
    TestAgents(JoinPath(dir, "project"));

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
