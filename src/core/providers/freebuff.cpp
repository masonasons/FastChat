// The Freebuff CLI as a provider, driven the way BlindPilot drives it
// (https://github.com/serrebidev/blindpilot).
//
// Freebuff has no headless mode: its CLI is a full-screen terminal interface,
// takes no prompt argument, and a prompt piped to its standard input waits for a
// terminal. So a pseudo-terminal runs the real interface off screen, the prompt
// is pasted into its composer, and the turn is followed on the screen. The
// answer itself is taken from the chat Freebuff saves under
// ~/.config/manicode/projects/<project>/chats/<id> (chat-messages.json and
// log.jsonl), which is written once the reply is finished and is exact where the
// screen is only a layout. Its tools come from those same blocks.

#include "core/providers/provider_impl.h"
#include "core/platform.h"
#include "core/terminal_screen.h"
#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* kPreferredModel = "z-ai/glm-5.3-flash";
const char* kInterrupted = "[response interrupted]";
const char* kBootFingerprint = "Enhanced CLI fingerprint generated successfully";
const char* kQuestionMarker = "Some questions for you";

// Freebuff's interface, as its own words on the screen.
const char* kComposerMarkers[] = {"Enter a coding task or / for commands", "Describe your task"};
const char* kPickerMarkers[] = {"RECOMMENDED", "Start coding for free"};

const int kCols = 180;
const int kRows = 60;
// Two minutes without a repaint, before the message is given up on. A first
// launch downloads and unpacks Freebuff, and only half of that draws.
const int64_t kStartupSilenceMs = 120000;
// The longest a single turn is listened to.
const int64_t kTurnLimitMs = 60LL * 60 * 1000;
const unsigned kKeySettleMs = 150;

const char* kInstallHelp =
    "Install it with npm install -g freebuff (https://freebuff.com/cli), or set the program in the provider's settings.";
const char* kRejoinMessage =
    "Freebuff dropped its session and is holding the message until it rejoins, but nothing is answering. "
    "Quit and reopen Freebuff, then send the message again.";

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += separator;
        out += part;
    }
    return out;
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

void SleepMs(int64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------
// Where Freebuff keeps a conversation
// ---------------------------------------------------------------------------

std::string ConfigDir() {
    // Freebuff reads its own config folder from this, and the tests point it
    // somewhere harmless.
    const char* env = std::getenv("FREEBUFF_CONFIG_DIR");
    if (env && *env) return PathToUtf8(PathFromUtf8(env));
    return JoinPath(GetHomeDir(), ".config/manicode");
}

std::string ChatRoot() { return JoinPath(ConfigDir(), "projects"); }

std::string FolderName(const std::string& path) {
    return PathToUtf8(PathFromUtf8(path).lexically_normal().filename());
}

// Every folder a Freebuff chat for this workspace could be stored under.
// Freebuff derives its bucket from the Git root, which is not always the
// workspace's own name, so they are all searched.
std::vector<std::string> ChatFolders(const std::string& cwd) {
    const std::string root = ChatRoot();
    std::vector<std::string> folders{JoinPath(JoinPath(root, FolderName(cwd)), "chats"), JoinPath(root, "chats")};
    std::error_code ec;
    for (fs::directory_iterator it(PathFromUtf8(root), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code isDir;
        if (!it->is_directory(isDir) || isDir) continue;
        std::string chats = JoinPath(PathToUtf8(it->path()), "chats");
        if (std::find(folders.begin(), folders.end(), chats) == folders.end()) folders.push_back(chats);
    }
    return folders;
}

int64_t ModifiedAt(const std::string& path) {
    std::error_code ec;
    auto when = fs::last_write_time(PathFromUtf8(path), ec);
    return ec ? 0 : static_cast<int64_t>(when.time_since_epoch().count());
}

int64_t FileSize(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(PathFromUtf8(path), ec);
    return ec ? 0 : static_cast<int64_t>(size);
}

bool LooksLikeModelId(const std::string& id) {
    size_t slash = id.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == id.size()) return false;
    if (id.size() > 90 || id.find('/', slash + 1) != std::string::npos) return false;
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '/' && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

// Freebuff has no command that lists models, and the endpoint its catalog comes
// from wants a signed device key, but the catalog itself is compiled into its
// program. So the ids are read back out of it, in the order its picker shows
// them, which is how each one is written there:
//   "vendor/model":{section:"premium",order:40,more:!0}
std::vector<std::string> ReadCatalog(const std::string& path) {
    std::vector<std::string> ids;
    std::ifstream in(PathFromUtf8(path), std::ios::binary);
    if (!in) return ids;

    const std::string marker = "\":{section:";
    const size_t chunkSize = 1 << 20;
    std::string chunk(chunkSize, '\0');
    std::string carry;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunkSize));
        size_t got = static_cast<size_t>(in.gcount());
        if (got == 0) break;
        std::string window = carry + std::string(chunk.data(), got);
        for (size_t at = window.find(marker); at != std::string::npos; at = window.find(marker, at + marker.size())) {
            size_t opening = at == 0 ? std::string::npos : window.rfind('"', at - 1);
            if (opening == std::string::npos) continue;
            std::string id = window.substr(opening + 1, at - opening - 1);
            if (!LooksLikeModelId(id)) continue;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        // Keep enough of the end for an entry split across two chunks.
        carry = window.size() > 256 ? window.substr(window.size() - 256) : window;
    }
    return ids;
}

// The ids of every chat, and when it last changed.
std::map<std::string, int64_t> ChatDirs(const std::string& cwd) {
    std::map<std::string, int64_t> found;
    for (const auto& folder : ChatFolders(cwd)) {
        std::error_code ec;
        for (fs::directory_iterator it(PathFromUtf8(folder), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code isDir;
            if (!it->is_directory(isDir) || isDir) continue;
            std::string id = PathToUtf8(it->path().filename());
            found[id] = std::max(found[id], ModifiedAt(PathToUtf8(it->path())));
        }
    }
    return found;
}

std::string ChatPath(const std::string& cwd, const std::string& session) {
    if (session.empty()) return std::string();
    for (const auto& folder : ChatFolders(cwd)) {
        std::string candidate = JoinPath(folder, session);
        std::error_code ec;
        if (fs::is_directory(PathFromUtf8(candidate), ec)) return candidate;
    }
    return std::string();
}

// The CLI only resumes ids in this project's own bucket; anywhere else it falls
// back to "most recent", which would continue a conversation nobody opened.
std::string ResumeChatPath(const std::string& cwd, const std::string& session) {
    std::string chat = ChatPath(cwd, session);
    if (chat.empty()) return chat;
    std::string expected = JoinPath(JoinPath(ChatRoot(), FolderName(cwd)), "chats");
    return PathToUtf8(PathFromUtf8(chat).parent_path()) == PathToUtf8(PathFromUtf8(expected)) ? chat : std::string();
}

// The id of a chat this launch created: a new folder after the terminal was
// started, newest first.
std::string NewChatPath(const std::map<std::string, int64_t>& before, const std::string& cwd) {
    std::map<std::string, int64_t> after = ChatDirs(cwd);
    std::string newest;
    int64_t newestAt = -1;
    for (const auto& [id, when] : after) {
        if (before.count(id) || when < newestAt) continue;
        newest = id;
        newestAt = when;
    }
    return newest.empty() ? std::string() : ChatPath(cwd, newest);
}

// Freebuff's per-chat log, from `offset` on.
std::string LogTail(const std::string& chat, int64_t offset) {
    if (chat.empty()) return std::string();
    std::ifstream in(PathFromUtf8(JoinPath(chat, "log.jsonl")), std::ios::binary);
    if (!in) return std::string();
    in.seekg(static_cast<std::streamoff>(std::max<int64_t>(0, offset)));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > 1024 * 1024) text = text.substr(text.size() - 1024 * 1024);
    return text;
}

// Event names only, never the prompt or answer text inside the log's data.
std::vector<std::string> LogMessages(const std::string& tail) {
    std::vector<std::string> messages;
    for (const auto& line : SplitLines(tail)) {
        json entry = json::parse(line, nullptr, false);
        if (!entry.is_object()) continue;
        std::string message = Str(entry, "msg");
        if (!message.empty()) messages.push_back(message);
    }
    return messages;
}

// Whether the log's last word on the session is a drop nobody answered.
// "session over" is also the first line of a healthy boot, so it is only a drop
// when nothing later reconnected.
bool LogPendingDrop(const std::string& tail) {
    bool pending = false;
    for (const auto& message : LogMessages(tail)) {
        if (StartsWith(message, "[chat-runtime] Freebuff session over;")) pending = true;
        else if (StartsWith(message, "Reconnection detected") || StartsWith(message, "Start agent ") ||
                 message == kBootFingerprint) pending = false;
    }
    return pending;
}

// Whether a launch's log shows a Freebuff that can take a message: the boot's
// last shared milestone, or a session that dropped and reconnected.
bool LogBootFinished(const std::string& tail) {
    if (LogPendingDrop(tail)) return false;
    for (const auto& message : LogMessages(tail)) {
        if (message == kBootFingerprint || StartsWith(message, "Reconnection detected") ||
            StartsWith(message, "Start agent ")) {
            return true;
        }
    }
    return false;
}

// complete / cancelled / disconnected / "" out of the chat's own log.
std::string RunStatus(const std::string& chat, int64_t offset) {
    std::string tail = LogTail(chat, offset);
    if (Trim(tail).empty()) return std::string();
    std::vector<std::string> messages = LogMessages(tail);
    for (const auto& message : messages) {
        if (StartsWith(message, "Agent run cancelled by user")) return "cancelled";
    }
    for (const auto& message : messages) {
        if (message == "Main prompt finished") return "complete";
    }
    if (LogPendingDrop(tail)) return "disconnected";
    return std::string();
}

// Whether the chat runtime has reconnected and can take a message. A message
// given to the composer before this is dropped silently, which is the bug this
// whole wait exists to avoid.
bool BootReady(const std::string& cwd, const std::map<std::string, int64_t>& before, const std::string& chat,
               int64_t offset) {
    if (!chat.empty()) {
        std::string tail = LogTail(chat, offset);
        return !Trim(tail).empty() && LogBootFinished(tail);
    }
    std::string created = NewChatPath(before, cwd);
    if (created.empty()) return false;  // no chat yet: the boot is still in progress
    std::string tail = LogTail(created, 0);
    return !Trim(tail).empty() && LogBootFinished(tail);
}

// ---------------------------------------------------------------------------
// The saved chat
// ---------------------------------------------------------------------------

struct Snapshot {
    std::string id;
    std::string thinking;
    std::string answer;
    json blocks = json::array();
};

// The newest AI message that actually carries a reply. Freebuff writes a
// mode-divider ahead of every turn that also looks like an AI message.
json AnswerMessage(const json& payload) {
    if (!payload.is_array()) return json();
    for (auto it = payload.rbegin(); it != payload.rend(); ++it) {
        if (!it->is_object() || Str(*it, "variant") != "ai") continue;
        auto blocks = it->find("blocks");
        if (blocks == it->end() || !blocks->is_array()) continue;
        for (const auto& block : *blocks) {
            if (!block.is_object()) continue;
            std::string kind = Str(block, "type");
            if (kind == "text" || kind == "agent") return *it;
        }
    }
    return json();
}

Snapshot ReadChat(const std::string& chat) {
    Snapshot snap;
    std::string text;
    if (chat.empty() || !ReadFileText(JoinPath(chat, "chat-messages.json"), text)) return snap;
    json message = AnswerMessage(json::parse(text, nullptr, false));
    if (!message.is_object()) return snap;
    snap.id = Str(message, "id");
    snap.blocks = message.value("blocks", json::array());
    std::vector<std::string> thinking, answer;
    for (const auto& block : snap.blocks) {
        if (!block.is_object() || Str(block, "type") != "text") continue;
        std::string content = Trim(Str(block, "content"));
        if (EndsWith(content, kInterrupted)) {
            content = Trim(content.substr(0, content.size() - std::string(kInterrupted).size()));
        }
        if (content.empty()) continue;
        if (Str(block, "textType") == "reasoning") thinking.push_back(content);
        else answer.push_back(content);
    }
    snap.thinking = Join(thinking, "\n\n");
    snap.answer = Join(answer, "\n\n");
    return snap;
}

std::string ChatStamp(const std::string& chat) {
    std::string stamp;
    for (const char* name : {"chat-messages.json", "log.jsonl"}) {
        std::string path = JoinPath(chat, name);
        stamp += std::to_string(ModifiedAt(path)) + ":" + std::to_string(FileSize(path)) + "|";
    }
    return stamp;
}

// ---------------------------------------------------------------------------
// Selecting the model
// ---------------------------------------------------------------------------

// Freebuff reads its model once, at launch, and rewrites the setting to its own
// recommendation after a turn, so the choice is put back before every run.
void SelectModel(const std::string& model) {
    if (model.empty()) return;
    std::string path = JoinPath(ConfigDir(), "settings.json");
    json payload = json::object();
    std::string text;
    if (ReadFileText(path, text)) {
        json parsed = json::parse(text, nullptr, false);
        if (parsed.is_object()) payload = parsed;
    }
    if (payload.value("freebuffModel", std::string()) == model) return;
    payload["freebuffModel"] = model;
    std::string temporary = path + ".fastchat.tmp";
    if (!WriteFileText(temporary, payload.dump(2) + "\n")) return;
    std::error_code ec;
    fs::rename(PathFromUtf8(temporary), PathFromUtf8(path), ec);
}

// ---------------------------------------------------------------------------
// Reading the screen
// ---------------------------------------------------------------------------

bool HasComposer(const std::string& visible) {
    for (const char* marker : kComposerMarkers) {
        if (Has(visible, marker)) return true;
    }
    for (const auto& line : SplitLines(visible)) {
        std::string trimmed = Trim(line);
        if (trimmed == "\xE2\x80\xBA" || trimmed == ">") return true;  // the composer's own marker
    }
    return false;
}

bool HasModelPicker(const std::string& visible) {
    for (const char* marker : kPickerMarkers) {
        if (Has(visible, marker)) return true;
    }
    return Has(visible, "See all ") && Has(visible, "models");
}

bool IsBusy(const std::string& visible) {
    return Has(visible, "thinking...") || Has(visible, "thinking\xE2\x80\xA6") || Has(visible, "working...") ||
           Has(visible, "working\xE2\x80\xA6") || Has(visible, "Esc to stop") || Has(visible, "Esc to interrupt") ||
           Has(visible, "\xE2\x96\xA0 Esc");
}

std::string LastVisibleLine(const std::string& visible) {
    std::vector<std::string> lines = SplitLines(visible);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string trimmed = Trim(*it);
        if (!trimmed.empty()) return TruncateUtf8(trimmed, 200);
    }
    return std::string();
}

std::string LaunchFailure(const std::string& visible) {
    std::string reason = LastVisibleLine(visible);
    if (reason.empty()) {
        return "Freebuff's terminal closed before it was ready for a prompt, without saying why. "
               "Check that Freebuff runs in a terminal, then try again.";
    }
    std::string lower = ToLower(reason);
    if (Has(lower, "node") && (Has(lower, "not found") || Has(lower, "no such file or directory"))) {
        return "Freebuff could not start: " + reason + ". Freebuff runs on Node.js, and it was not found; install it, "
               "or set the program in the provider's settings.";
    }
    return "Freebuff's terminal closed before it was ready for a prompt: " + reason;
}

std::string StartupSilence(const std::string& visible) {
    std::string reason = LastVisibleLine(visible);
    std::string waited = std::to_string(kStartupSilenceMs / 1000);
    if (reason.empty()) {
        return "Freebuff started but printed nothing at all for " + waited +
               " seconds, so there was never a prompt to send the message to. Run freebuff in a terminal to see "
               "what it does there.";
    }
    return "Freebuff stopped short of a prompt and printed nothing for " + waited +
           " seconds. The last thing it showed was: " + reason;
}

// The question box Freebuff's own `ask_user` tool draws.
struct ScreenQuestion {
    std::string text;
    std::vector<std::string> options;
    bool multi = false;
};

std::vector<std::string> QuestionBox(const std::string& visible) {
    std::vector<std::string> lines = SplitLines(visible);
    auto start = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return Has(line, kQuestionMarker);
    });
    std::vector<std::string> body;
    if (start == lines.end()) return body;
    for (auto it = start + 1; it != lines.end(); ++it) {
        std::string trimmed = Trim(*it);
        if (StartsWith(trimmed, "\xE2\x95\xB0")) break;  // the box's bottom corner
        if (StartsWith(trimmed, "\xE2\x94\x82")) trimmed.erase(0, 3);  // its left side
        if (EndsWith(trimmed, "\xE2\x94\x82")) trimmed.erase(trimmed.size() - 3);
        body.push_back(Trim(trimmed));
    }
    return body;
}

// (how many questions there are, which one is open, and that question).
bool ParseOpenQuestion(const std::string& visible, int& total, int& openIndex, ScreenQuestion& question) {
    std::vector<std::string> body = QuestionBox(visible);
    total = 0;
    openIndex = -1;
    if (body.empty()) return false;
    for (const auto& line : body) {
        bool open = StartsWith(line, "\xE2\x96\xBC");   // down-pointing: the open question
        bool folded = StartsWith(line, "\xE2\x96\xB6");  // right-pointing: one still folded away
        if (open || folded) {
            std::string text = Trim(line.substr(3));
            // "2. Question" when there is more than one.
            size_t digits = 0;
            while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') digits++;
            if (digits && text.size() > digits + 1 && text[digits] == '.') text = Trim(text.substr(digits + 1));
            if (open && openIndex < 0) {
                openIndex = total;
                question.text = text;
                question.options.clear();
                question.multi = false;
            }
            total++;
            continue;
        }
        if (openIndex < 0 || total != openIndex + 1) continue;
        if (Has(line, "(Select multiple options)")) {
            question.multi = true;
            continue;
        }
        if (StartsWith(line, "\xE2\x97\x8B") || StartsWith(line, "\xE2\x97\x8F")) {  // ○ / ●
            question.options.push_back(Trim(line.substr(3)));
        } else if (StartsWith(line, "\xE2\x98\x90") || StartsWith(line, "\xE2\x98\x91")) {  // ☐ / ☑
            question.multi = true;
            question.options.push_back(Trim(line.substr(3)));
        }
    }
    if (openIndex < 0 || question.text.empty()) return false;
    // Freebuff adds "Custom" itself, below the model's own answers.
    if (!question.options.empty() && question.options.back() == "Custom") question.options.pop_back();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The provider
// ---------------------------------------------------------------------------

namespace {

class FreebuffProvider : public Provider {
public:
    using Provider::Provider;

    bool IsAgent() const override { return true; }

    std::string Program() const { return m_config.baseUrl.empty() ? FindProgram("freebuff") : m_config.baseUrl; }

    ChatResult RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks, const std::atomic<bool>* cancel) override;

    ChatResult Chat(const ChatRequest& request, const StreamCallbacks& callbacks, const std::atomic<bool>* cancel) override {
        AgentTurn turn;
        turn.request = request;
        AgentCallbacks cb;
        static_cast<StreamCallbacks&>(cb) = callbacks;
        return RunAgent(turn, cb, cancel);
    }

    // Freebuff's catalog, read from the program once and kept: the file is a
    // hundred megabytes and it only changes when Freebuff updates itself.
    std::vector<std::string> CatalogModels(const std::string& program) const {
        static std::mutex lock;
        static std::map<std::string, std::vector<std::string>> cache;
        std::lock_guard<std::mutex> held(lock);
        auto known = cache.find(program);
        if (known != cache.end()) return known->second;

        std::vector<std::string> ids = ReadCatalog(program);
        if (ids.empty()) {
            // What is on the PATH may be a launcher (.cmd, or a Volta script);
            // the program it starts lives beside the config it keeps.
            ids = ReadCatalog(JoinPath(ConfigDir(), "freebuff.exe"));
        }
        if (ids.empty()) ids = ReadCatalog(JoinPath(ConfigDir(), "freebuff"));
        cache[program] = ids;
        return ids;
    }

    bool ListModels(std::vector<ModelInfo>& out, std::string& error, const std::atomic<bool>*) override {
        std::string program = Program();
        if (program.empty()) {
            error = std::string("Freebuff was not found. ") + kInstallHelp;
            return false;
        }
        out.clear();
        for (const std::string& id : CatalogModels(program)) {
            ModelInfo info;
            info.id = id;
            info.name = id;
            info.reasoning = id == kPreferredModel;
            out.push_back(info);
        }
        if (out.empty()) {
            // No readable catalog (an older Freebuff, or one that keeps it
            // elsewhere). Its recommendation still works, and any other id can
            // be typed in.
            ModelInfo info;
            info.id = kPreferredModel;
            info.name = kPreferredModel;
            info.description = "Freebuff's recommended model. Type another model's name to ask it to use that instead.";
            info.reasoning = true;
            out.push_back(info);
            return true;
        }
        // Freebuff's picker opens on the model it recommends, so keep that one
        // first: FastChat takes the first as the default.
        auto preferred = std::find_if(out.begin(), out.end(),
                                      [](const ModelInfo& model) { return model.id == kPreferredModel; });
        if (preferred != out.end()) std::rotate(out.begin(), preferred, preferred + 1);
        return true;
    }

    AccountInfo GetAccountInfo(const std::atomic<bool>*) override {
        AccountInfo info;
        info.fetchedAt = NowSeconds();
        std::string program = Program();
        if (program.empty()) {
            info.error = "Freebuff was not found.";
            return info;
        }
        info.Add("Program", program);
        info.Add("Config folder", ConfigDir());
        std::string credentials = JoinPath(ConfigDir(), "credentials.json");
        std::string text;
        if (!ReadFileText(credentials, text)) {
            info.Add("Login", "not signed in (run: freebuff login)");
        } else {
            json account = json::parse(text, nullptr, false);
            std::string email;
            for (const char* key : {"email", "login", "user"}) {
                if (email.empty() && account.is_object()) email = Str(account, key);
            }
            info.Add("Login", email.empty() ? std::string("signed in") : "signed in as " + email);
        }
        info.Add("Plan usage", "Freebuff does not report plan limits in this mode");
        info.ok = true;
        return info;
    }
};

// Tell the user the rest of the answer that never made it onto the screen.
void PostMessages(const Snapshot& snap, const std::string& session, const AgentCallbacks& callbacks) {
    auto post = [&](Message m) {
        m.providerData["freebuff"] = {{"session_id", session}};
        if (callbacks.onMessage) callbacks.onMessage(m);
    };
    std::string thinking;
    int index = 0;
    for (const auto& block : snap.blocks) {
        if (!block.is_object()) continue;
        std::string kind = Str(block, "type");
        if (kind == "agent") {
            if (!callbacks.onActivity) continue;
            ActivityEvent event;
            event.id = Str(block, "agentId");
            if (event.id.empty()) event.id = "agent-" + std::to_string(index);
            event.kind = "agent";
            event.agentType = Str(block, "agentType");
            event.description = Str(block, "agentName");
            if (event.description.empty()) event.description = event.agentType.empty() ? "agent" : event.agentType;
            std::string status = ToLower(Str(block, "status"));
            event.status = status.empty() ? "running" : status;
            if (event.status == "complete") event.status = "completed";
            event.prompt = Str(block, "initialPrompt");
            callbacks.onActivity(event);
            index++;
            continue;
        }
        if (kind != "tool") {
            index++;
            continue;
        }
        std::string rawName = Str(block, "toolName");
        if (rawName.empty()) rawName = "tool";
        std::string lowerName = ToLower(rawName);
        std::string name = rawName;
        if (Has(lowerName, "write") || Has(lowerName, "edit") || Has(lowerName, "patch") ||
            Has(lowerName, "replace")) {
            name = "apply_patch";
        } else if (Has(lowerName, "bash") || Has(lowerName, "shell") || Has(lowerName, "terminal") ||
                   Has(lowerName, "command")) {
            name = "run_command";
        }
        json input = block.contains("input") && block["input"].is_object() ? block["input"] : json::object();
        std::string id = Str(block, "toolCallId");
        if (id.empty()) id = Str(block, "id");
        if (id.empty()) id = "tool-" + std::to_string(index);

        json args = input;
        std::string summary;
        if (name == "apply_patch") {
            std::string path = Str(input, "path");
            if (path.empty()) path = Str(input, "file_path");
            if (path.empty()) path = Str(input, "file");
            if (!path.empty()) {
                args = json{{"changes", json::array({json{{"path", path}, {"kind", "update"}}})}};
                summary = "Changed " + FolderName(path);
            } else {
                summary = "Changed files";
            }
        } else if (name == "run_command") {
            std::string command = Str(input, "command");
            if (command.empty()) command = Str(input, "cmd");
            summary = "Ran " + OneLine(command, 80);
        } else {
            summary = "Used " + name;
        }

        Message call;
        call.role = Role::Assistant;
        call.reasoning = thinking;
        thinking.clear();
        call.toolCalls.push_back({id, name, JsonDump(args)});
        post(call);

        std::string output;
        for (const char* key : {"output", "aggregated_output", "result"}) {
            auto it = block.find(key);
            if (it == block.end() || it->is_null()) continue;
            output = it->is_string() ? it->get<std::string>() : JsonDump(*it);
            break;
        }
        Message tool;
        tool.role = Role::Tool;
        tool.toolCallId = id;
        tool.toolName = name;
        tool.content = SanitizeUtf8(output);
        tool.toolError = Str(block, "status") == "failed";
        tool.reasoning = summary;
        post(tool);
        index++;
    }
    if (!snap.answer.empty() || !snap.thinking.empty()) {
        Message answer;
        answer.role = Role::Assistant;
        answer.content = snap.answer;
        answer.reasoning = snap.thinking;
        post(answer);
    }
}

void Press(PseudoTerminal& pty, const char* key, int times = 1) {
    for (int i = 0; i < std::max(0, times); i++) {
        pty.Write(key);
        SleepMs(kKeySettleMs);
    }
}

bool PasteText(PseudoTerminal& pty, const std::string& text) {
    // OpenTUI handles paste and Enter as separate input events, and raw key
    // events discard newlines: a paste preserves them.
    return pty.Write("\x1b[200~" + text + "\x1b[201~");
}

// Work through Freebuff's question box and submit the answers, one question at a
// time (only one question's answers are on screen at once).
bool AnswerQuestions(PseudoTerminal& pty, TerminalScreen& screen, const AgentCallbacks& callbacks) {
    auto refresh = [&](unsigned waitMs) {
        std::string chunk;
        if (pty.Read(chunk, waitMs)) screen.Feed(chunk);
        return Trim(screen.Display());
    };
    for (int pass = 0; pass < 16; pass++) {
        std::string visible = refresh(kKeySettleMs);
        if (!Has(visible, kQuestionMarker)) return true;
        int total = 0, index = -1;
        ScreenQuestion question;
        if (!ParseOpenQuestion(visible, total, index, question)) {
            visible = refresh(500);
            if (!ParseOpenQuestion(visible, total, index, question)) return false;
        }
        if (!callbacks.askQuestions) {
            Press(pty, "\x1b");  // closes the box; Freebuff tells the model they were skipped
            refresh(500);
            return true;
        }
        AgentQuestion asked;
        asked.question = question.text;
        asked.multiSelect = question.multi;
        for (const auto& option : question.options) asked.options.push_back({option, std::string()});
        std::vector<std::string> answers;
        if (!callbacks.askQuestions({asked}, answers) || answers.empty()) {
            Press(pty, "\x1b");
            refresh(500);
            return true;
        }
        const std::string& chosen = answers[0];
        if (!question.multi) {
            int target = static_cast<int>(question.options.size());  // past the answers: "Custom"
            for (size_t i = 0; i < question.options.size(); i++) {
                if (question.options[i] == chosen) target = static_cast<int>(i);
            }
            Press(pty, "\x1b[B", target);
            Press(pty, "\r");
            if (target == static_cast<int>(question.options.size())) {
                PasteText(pty, chosen);
                SleepMs(kKeySettleMs);
                Press(pty, "\r");
            }
        } else {
            int position = 0;
            for (size_t i = 0; i < question.options.size(); i++) {
                if (std::find(answers.begin(), answers.end(), question.options[i]) == answers.end()) continue;
                Press(pty, "\x1b[B", static_cast<int>(i) - position);
                Press(pty, "\r");
                position = static_cast<int>(i);
            }
            // Ticking a box leaves the focus where it was; one step past the
            // last answer wraps onto the next question.
            Press(pty, "\x1b[B", static_cast<int>(question.options.size()) - position + 1);
        }
        if (index >= total - 1) {
            Press(pty, "\t");
            Press(pty, "\r");
            break;
        }
    }
    return true;
}

}  // namespace

ChatResult FreebuffProvider::RunAgent(const AgentTurn& turn, const AgentCallbacks& callbacks,
                                      const std::atomic<bool>* cancel) {
    ChatResult result;
    std::string program = Program();
    if (program.empty()) {
        result.error = std::string("Freebuff was not found. ") + kInstallHelp;
        return result;
    }
    const ChatRequest& req = turn.request;
    std::string folder = turn.tools && !turn.folder.empty() ? turn.folder : AgentChatFolder();
    std::string model = req.model.empty() || req.model == "default" ? kPreferredModel : req.model;
    SelectModel(model);

    std::string chat = turn.resumeSession.empty() ? std::string() : ResumeChatPath(folder, turn.resumeSession);
    if (!turn.resumeSession.empty() && chat.empty()) {
        result.error = "Freebuff cannot resume this conversation in this folder. Open its original folder, or start a "
                       "new conversation.";
        return result;
    }
    std::map<std::string, int64_t> before = ChatDirs(folder);
    int64_t logOffset = chat.empty() ? 0 : FileSize(JoinPath(chat, "log.jsonl"));
    // A resumed chat already holds the previous turn's answer; its id is what
    // tells this turn's answer apart from it.
    std::string baseline = ReadChat(chat).id;
    std::string session = turn.resumeSession;

    // Attachments: Freebuff has no attachment flag, but it reads files with its
    // own tools, so they are written down and named in the prompt.
    struct RemoveFolder {
        std::string path;
        ~RemoveFolder() {
            std::error_code ec;
            if (!path.empty()) fs::remove_all(PathFromUtf8(path), ec);
        }
    } attachments;
    std::string prompt = AgentPrompt(turn);
    if (!Trim(req.systemPrompt).empty()) prompt = "Instructions: " + req.systemPrompt + "\n\n" + prompt;
    int count = 0;
    for (const auto& file : AgentAttachments(turn)) {
        if (file.IsPdf()) {
            prompt += "\n\n[" + (file.name.empty() ? std::string("A PDF") : file.name) +
                      " was attached, but Freebuff cannot read PDFs.]";
            continue;
        }
        if (attachments.path.empty()) {
            attachments.path = JoinPath(JoinPath(GetDataDir(), "agent-images"), NewId());
            std::error_code ec;
            fs::create_directories(PathFromUtf8(attachments.path), ec);
        }
        std::string ext = file.mediaType == "image/png"    ? ".png"
                          : file.mediaType == "image/gif"  ? ".gif"
                          : file.mediaType == "image/webp" ? ".webp"
                                                           : ".jpg";
        std::string path = JoinPath(attachments.path, "image" + std::to_string(++count) + ext);
        if (!WriteFileText(path, Base64Decode(file.base64))) continue;
        prompt += "\n\n[Attached image: " + (file.name.empty() ? std::string("an image") : file.name) +
                  " (saved at " + path + ")]";
    }

    std::vector<std::string> argv{program, "--cwd", folder, "--trust-agents"};
    if (!session.empty()) {
        argv.push_back("--continue");
        argv.push_back(session);
    }
    PseudoTerminal pty;
    std::string error;
    if (!pty.Start(argv, folder, {}, kCols, kRows, error)) {
        result.error = error;
        return result;
    }
    TerminalScreen screen(kCols, kRows, 4000);
    std::string visible, chatStamp, structured, structuredId;
    bool sent = false, acceptedModel = false, finished = false;
    int64_t lastChange = NowMillis(), startedAt = 0, nextHeartbeat = 0, completionAt = 0;
    const int64_t deadline = NowMillis() + kTurnLimitMs;

    while (!(cancel && cancel->load()) && NowMillis() < deadline) {
        std::string chunk;
        if (pty.Read(chunk, 30)) {
            // A repaint arrives as a burst of small writes; taking it whole
            // means laying the screen out once instead of dozens of times.
            for (int i = 0; i < 256; i++) {
                std::string more;
                if (!pty.Read(more, 0)) break;
                chunk += more;
            }
            screen.Feed(chunk);
        } else if (pty.Ended()) {
            if (!sent) {
                result.error = LaunchFailure(visible);
                return result;
            }
            break;  // it ended mid-turn: whatever is saved is the answer
        }
        std::string current = Trim(screen.Display());
        if (current != visible) {
            visible = current;
            lastChange = NowMillis();
        }

        // Freebuff opens on a model chooser rather than the composer: take the
        // model it is offering, which is the one selected before launch.
        if (!acceptedModel && !sent && HasModelPicker(visible)) {
            pty.Write("\r");
            acceptedModel = true;
            continue;
        }

        bool atPrompt = HasComposer(visible);
        bool busy = IsBusy(visible);

        if (!sent && atPrompt) {
            // 0.0.168 paints its composer before the runtime behind it has
            // reconnected, and a message given to it is lost silently.
            if (!BootReady(folder, before, chat, logOffset)) {
                if (NowMillis() - lastChange >= kStartupSilenceMs) {
                    result.error = StartupSilence(visible);
                    return result;
                }
                continue;
            }
            if (chat.empty()) {
                chat = NewChatPath(before, folder);
                if (!chat.empty()) {
                    session = FolderName(chat);
                    logOffset = 0;
                    baseline = ReadChat(chat).id;
                    if (callbacks.onStatus) callbacks.onStatus("Freebuff is starting a conversation");
                }
            }
            if (!PasteText(pty, prompt)) {
                result.error = "Could not send the prompt to Freebuff";
                return result;
            }
            SleepMs(50);
            pty.Write("\r");
            sent = true;
            startedAt = NowMillis();
            nextHeartbeat = startedAt + 30000;
            if (callbacks.onStatus) callbacks.onStatus("Freebuff is working");
            continue;
        }
        if (!sent) {
            if (NowMillis() - lastChange >= kStartupSilenceMs) {
                result.error = StartupSilence(visible);
                return result;
            }
            continue;
        }

        // A question box stops the turn until it is answered. An answered box
        // stays on screen with its questions folded away, so it is a question
        // actually being asked - not the title alone - that takes over.
        if (Has(visible, kQuestionMarker)) {
            int total = 0, openIndex = -1;
            ScreenQuestion asked;
            if (ParseOpenQuestion(visible, total, openIndex, asked)) {
                AnswerQuestions(pty, screen, callbacks);
                continue;
            }
        }
        if (chat.empty()) {
            chat = NewChatPath(before, folder);
            if (!chat.empty()) {
                session = FolderName(chat);
                logOffset = 0;
                baseline = ReadChat(chat).id;
            }
        }
        if (!chat.empty()) {
            std::string stamp = ChatStamp(chat);
            if (stamp != chatStamp) {
                chatStamp = stamp;
                Snapshot snap = ReadChat(chat);
                if (!snap.id.empty() && snap.id != baseline) {
                    if (snap.id != structuredId) {
                        structuredId = snap.id;
                        baseline.clear();
                    }
                    if (!snap.answer.empty()) structured = snap.answer;
                }
            }
            std::string status = RunStatus(chat, logOffset);
            if (status == "cancelled") {
                result.error = "Freebuff reported that the response was interrupted";
                break;
            }
            if (status == "complete") {
                if (!completionAt) completionAt = NowMillis();
                else if (NowMillis() - completionAt >= 200) {
                    finished = true;
                    break;
                }
            }
        }
        if (NowMillis() >= nextHeartbeat) {
            if (callbacks.onStatus) {
                callbacks.onStatus("Freebuff is still working; " +
                                   std::to_string((NowMillis() - startedAt) / 1000) + " seconds elapsed");
            }
            nextHeartbeat = NowMillis() + 30000;
        }
        (void)busy;
    }

    bool cancelled = cancel && cancel->load();
    pty.Kill();
    result.providerData["freebuff"] = {{"session_id", session}};
    if (cancelled) {
        result.cancelled = true;
        return result;
    }
    if (!sent) {
        result.error = "Freebuff did not become ready for input";
        return result;
    }
    if (result.error.empty()) {
        Snapshot snap = ReadChat(chat);
        bool isNew = !snap.id.empty() && snap.id != baseline;
        if (isNew && !snap.answer.empty()) structured = snap.answer;
        if (!isNew) {
            // The saved chat still holds the answer this turn was resumed from.
            snap = Snapshot();
        }
        if (snap.answer.empty() && snap.blocks.empty() && structured.empty()) {
            if (!finished) {
                result.error = "Freebuff was still working an hour after the message was sent and had produced no "
                               "answer, so FastChat stopped waiting for it.";
            } else {
                result.error = "Freebuff finished without saving an answer";
            }
        } else {
            PostMessages(snap, session, callbacks);
            result.text = snap.answer.empty() ? structured : snap.answer;
            result.reasoning = snap.thinking;
        }
    }
    if (!result.error.empty()) {
        std::string cli = FindProgram("freebuff");
        result.errorDetails = "Program: " + program + "\n";
        result.errorDetails += "Config folder: " + ConfigDir() + "\n";
        result.errorDetails += "Chat: " + (chat.empty() ? std::string("(none found)") : chat) + "\n";
        result.errorDetails += "What to do:\n";
        if (cli.empty()) result.errorDetails += std::string("  - ") + kInstallHelp + "\n";
        result.errorDetails += "  - Run freebuff in a terminal to see what it does there.\n";
        if (chat.empty() && !session.empty()) result.errorDetails += std::string("  - ") + kRejoinMessage + "\n";
    }
    if (result.model.empty()) result.model = model;
    result.ok = result.error.empty();
    return result;
}

std::unique_ptr<Provider> CreateFreebuffProvider(const ProviderConfig& config) {
    return std::make_unique<FreebuffProvider>(config);
}
