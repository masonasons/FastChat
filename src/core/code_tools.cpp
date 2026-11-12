#include "core/code_tools.h"
#include "core/platform.h"
#include "core/util.h"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <system_error>

namespace fs = std::filesystem;

namespace {

const size_t kMaxReadBytes = 256 * 1024;
const size_t kMaxCommandOutput = 64 * 1024;
const int kMaxListEntries = 600;
const int kMaxSearchMatches = 200;

// Folders never worth walking into.
bool SkipDir(const std::string& name) {
    static const char* kSkip[] = {".git", "node_modules", ".svn", ".hg", "__pycache__", ".venv", "venv",
                                  ".vs", ".idea", "build", "out", "dist", "target", ".next", ".cache"};
    for (const char* s : kSkip) {
        if (name == s) return true;
    }
    return false;
}

std::string Rel(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    fs::path r = fs::relative(p, root, ec);
    std::string s = ec ? PathToUtf8(p) : PathToUtf8(r);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s.empty() ? "." : s;
}

// Resolve a path the model gave against the project folder; false if it
// escapes the folder.
bool Resolve(const ToolContext& ctx, const std::string& given, fs::path& out, std::string& error) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(PathFromUtf8(ctx.root), ec);
    fs::path p = PathFromUtf8(given.empty() ? "." : given);
    if (p.is_relative()) p = root / p;
    p = fs::weakly_canonical(p, ec);
    if (ec) p = p.lexically_normal();
    std::string rs = PathToUtf8(root), ps = PathToUtf8(p);
#ifdef _WIN32
    rs = ToLower(rs);
    ps = ToLower(ps);
#endif
    if (ps != rs && !(StartsWith(ps, rs) && (ps[rs.size()] == '\\' || ps[rs.size()] == '/'))) {
        error = "The path " + given + " is outside the project folder.";
        return false;
    }
    out = p;
    return true;
}

bool LooksBinary(const std::string& data) {
    size_t n = std::min<size_t>(data.size(), 8000);
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\0') return true;
    }
    return false;
}

json Args(const ToolCall& call) {
    return ArgumentsToObject(call.arguments);
}

std::string Str(const json& args, const char* key, const std::string& def = std::string()) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return JsonDump(*it);
}

int64_t Int(const json& args, const char* key, int64_t def) {
    auto it = args.find(key);
    if (it == args.end()) return def;
    if (it->is_number()) return it->get<int64_t>();
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return def;
}

bool Bool(const json& args, const char* key, bool def) {
    auto it = args.find(key);
    if (it == args.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return it->get<std::string>() == "true";
    return def;
}

ToolOutcome Fail(const std::string& message) {
    return {message, true, message};
}

// --- list_directory ---
ToolOutcome ListDirectory(const json& args, const ToolContext& ctx) {
    fs::path dir;
    std::string error;
    if (!Resolve(ctx, Str(args, "path", "."), dir, error)) return Fail(error);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return Fail("Not a folder: " + Str(args, "path", "."));
    bool recursive = Bool(args, "recursive", false);
    fs::path root = PathFromUtf8(ctx.root);
    std::vector<std::string> lines;
    int count = 0;
    bool more = false;
    auto add = [&](const fs::directory_entry& e) {
        if (count >= kMaxListEntries) {
            more = true;
            return;
        }
        std::string rel = Rel(e.path(), dir);
        if (e.is_directory(ec)) {
            lines.push_back(rel + "/");
        } else {
            lines.push_back(rel + "  (" + FormatBytes(e.file_size(ec)) + ")");
        }
        count++;
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec) && SkipDir(PathToUtf8(it->path().filename()))) {
                it.disable_recursion_pending();
                continue;
            }
            add(*it);
            if (more) break;
        }
    } else {
        for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) add(e);
    }
    std::sort(lines.begin(), lines.end());
    std::string out = "Contents of " + Rel(dir, root) + ":\n";
    for (const auto& l : lines) out += l + "\n";
    if (lines.empty()) out += "(empty)\n";
    if (more) out += "... (more entries not shown)\n";
    return {out, false, "Listed " + Rel(dir, root) + ", " + std::to_string(count) + " entries"};
}

// --- read_file ---
ToolOutcome ReadFile(const json& args, const ToolContext& ctx) {
    fs::path file;
    std::string error;
    std::string given = Str(args, "path");
    if (given.empty()) return Fail("read_file needs a path.");
    if (!Resolve(ctx, given, file, error)) return Fail(error);
    std::string data;
    if (!ReadFileText(PathToUtf8(file), data)) return Fail("Could not read " + given);
    if (LooksBinary(data)) return Fail(given + " is a binary file (" + FormatBytes(data.size()) + ").");
    std::vector<std::string> lines = SplitLines(data);
    int64_t start = std::max<int64_t>(1, Int(args, "start_line", 1));
    int64_t end = Int(args, "end_line", static_cast<int64_t>(lines.size()));
    end = std::min<int64_t>(end, static_cast<int64_t>(lines.size()));
    std::string out;
    size_t bytes = 0;
    int64_t last = start - 1;
    for (int64_t i = start; i <= end; i++) {
        std::string line = std::to_string(i) + "\t" + lines[static_cast<size_t>(i - 1)] + "\n";
        if (bytes + line.size() > kMaxReadBytes) break;
        out += line;
        bytes += line.size();
        last = i;
    }
    if (last < end) {
        out += "... (stopped at line " + std::to_string(last) + " of " + std::to_string(lines.size()) +
               "; read on with start_line)\n";
    }
    if (lines.empty()) out = "(empty file)\n";
    return {out, false, "Read " + given + " (" + std::to_string(lines.size()) + " lines)"};
}

// --- write_file ---
ToolOutcome WriteFile(const json& args, const ToolContext& ctx) {
    fs::path file;
    std::string error;
    std::string given = Str(args, "path");
    if (given.empty()) return Fail("write_file needs a path.");
    if (!Resolve(ctx, given, file, error)) return Fail(error);
    std::string content = Str(args, "content");
    std::error_code ec;
    bool existed = fs::exists(file, ec);
    fs::create_directories(file.parent_path(), ec);
    if (!WriteFileText(PathToUtf8(file), content)) return Fail("Could not write " + given);
    size_t lines = SplitLines(content).size();
    std::string what = (existed ? "Overwrote " : "Created ") + given + " (" + std::to_string(lines) + " lines)";
    return {what, false, what};
}

// --- edit_file ---
ToolOutcome EditFile(const json& args, const ToolContext& ctx) {
    fs::path file;
    std::string error;
    std::string given = Str(args, "path");
    if (given.empty()) return Fail("edit_file needs a path.");
    if (!Resolve(ctx, given, file, error)) return Fail(error);
    std::string oldText = Str(args, "old_text");
    std::string newText = Str(args, "new_text");
    bool all = Bool(args, "replace_all", false);
    std::string data;
    if (!ReadFileText(PathToUtf8(file), data)) return Fail("Could not read " + given);
    if (oldText.empty()) return Fail("old_text is empty; use write_file to create a file.");
    // Files with Windows line endings: match text the model wrote with \n.
    bool crlf = data.find("\r\n") != std::string::npos && oldText.find("\r\n") == std::string::npos;
    if (crlf) {
        oldText = ReplaceAll(oldText, "\n", "\r\n");
        newText = ReplaceAll(newText, "\n", "\r\n");
    }
    size_t count = 0;
    for (size_t pos = data.find(oldText); pos != std::string::npos; pos = data.find(oldText, pos + oldText.size())) count++;
    if (count == 0) return Fail("old_text was not found in " + given + ". Read the file again and copy the text exactly.");
    if (count > 1 && !all) {
        return Fail("old_text appears " + std::to_string(count) + " times in " + given +
                    ". Include more surrounding text to make it unique, or set replace_all.");
    }
    data = all ? ReplaceAll(data, oldText, newText) : data.replace(data.find(oldText), oldText.size(), newText);
    if (!WriteFileText(PathToUtf8(file), data)) return Fail("Could not write " + given);
    std::string what = "Edited " + given + (count > 1 ? " (" + std::to_string(count) + " places)" : "");
    return {what, false, what};
}

// Wildcards: * (not across /), ** (across /), ?
bool GlobMatch(const std::string& pattern, const std::string& text) {
    std::string re;
    for (size_t i = 0; i < pattern.size(); i++) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                re += ".*";
                i++;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') i++;
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') {
            re += "[^/]";
        } else if (std::string("\\^$.|+()[]{}").find(c) != std::string::npos) {
            re += '\\';
            re += c;
        } else {
            re += c;
        }
    }
    try {
        return std::regex_match(text, std::regex(re, std::regex::icase));
    } catch (...) {
        return false;
    }
}

template <typename Fn>
void WalkFiles(const fs::path& dir, const std::atomic<bool>* cancel, Fn fn) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || (cancel && cancel->load())) break;
        if (it->is_directory(ec)) {
            if (SkipDir(PathToUtf8(it->path().filename()))) it.disable_recursion_pending();
            continue;
        }
        if (!fn(*it)) break;
    }
}

// --- find_files ---
ToolOutcome FindFiles(const json& args, const ToolContext& ctx) {
    std::string pattern = Str(args, "pattern");
    if (pattern.empty()) return Fail("find_files needs a pattern.");
    fs::path dir;
    std::string error;
    if (!Resolve(ctx, Str(args, "path", "."), dir, error)) return Fail(error);
    bool hasSlash = pattern.find('/') != std::string::npos;
    std::vector<std::string> found;
    WalkFiles(dir, ctx.cancel, [&](const fs::directory_entry& e) {
        std::string rel = Rel(e.path(), dir);
        std::string name = PathToUtf8(e.path().filename());
        if (GlobMatch(pattern, hasSlash ? rel : name)) found.push_back(rel);
        return found.size() < static_cast<size_t>(kMaxListEntries);
    });
    std::sort(found.begin(), found.end());
    std::string out;
    for (const auto& f : found) out += f + "\n";
    if (found.empty()) out = "No files match " + pattern + "\n";
    return {out, false, "Found " + std::to_string(found.size()) + " files matching " + pattern};
}

// --- search_files ---
ToolOutcome SearchFiles(const json& args, const ToolContext& ctx) {
    std::string pattern = Str(args, "pattern");
    if (pattern.empty()) return Fail("search_files needs a pattern.");
    fs::path dir;
    std::string error;
    if (!Resolve(ctx, Str(args, "path", "."), dir, error)) return Fail(error);
    std::string glob = Str(args, "file_pattern");
    bool ignoreCase = Bool(args, "ignore_case", false);
    std::regex re;
    try {
        re = std::regex(pattern, ignoreCase ? std::regex::icase | std::regex::ECMAScript : std::regex::ECMAScript);
    } catch (const std::exception&) {
        return Fail("The pattern is not a valid regular expression.");
    }
    std::string out;
    int matches = 0, files = 0;
    WalkFiles(dir, ctx.cancel, [&](const fs::directory_entry& e) {
        std::error_code ec;
        if (e.file_size(ec) > 2 * 1024 * 1024) return true;
        std::string name = PathToUtf8(e.path().filename());
        if (!glob.empty() && !GlobMatch(glob, name)) return true;
        std::string data;
        if (!ReadFileText(PathToUtf8(e.path()), data) || LooksBinary(data)) return true;
        auto lines = SplitLines(data);
        bool any = false;
        for (size_t i = 0; i < lines.size(); i++) {
            if (std::regex_search(lines[i], re)) {
                out += Rel(e.path(), dir) + ":" + std::to_string(i + 1) + ": " + TruncateUtf8(lines[i], 300) + "\n";
                any = true;
                if (++matches >= kMaxSearchMatches) return false;
            }
        }
        if (any) files++;
        return true;
    });
    if (matches == 0) out = "No matches for " + pattern + "\n";
    if (matches >= kMaxSearchMatches) out += "... (stopped at " + std::to_string(kMaxSearchMatches) + " matches)\n";
    return {out, false, std::to_string(matches) + " matches in " + std::to_string(files) + " files for " + pattern};
}

// --- run_command ---
ToolOutcome RunCommand(const json& args, const ToolContext& ctx) {
    std::string command = Str(args, "command");
    if (command.empty()) return Fail("run_command needs a command.");
    if (ctx.commandApproval == "deny") {
        return Fail("Running commands is turned off for this conversation. Tell the user the command to run instead.");
    }
    if (ctx.commandApproval != "auto") {
        if (!ctx.approveCommand || !ctx.approveCommand(command)) {
            return Fail("The user declined to run this command.");
        }
    }
    unsigned timeout = ctx.commandTimeoutMs;
    int64_t asked = Int(args, "timeout_seconds", 0);
    if (asked > 0) timeout = static_cast<unsigned>(std::min<int64_t>(asked, 3600) * 1000);
    ProcessResult r = RunShellCommand(command, ctx.root, timeout, kMaxCommandOutput, ctx.cancel);
    if (!r.started) return Fail(r.error.empty() ? "The command could not start." : r.error);
    std::string out = r.output;
    if (r.truncated) out += "\n... (output cut off at " + FormatBytes(kMaxCommandOutput) + ")";
    if (r.timedOut) out += "\n(The command was stopped after " + std::to_string(timeout / 1000) + " seconds.)";
    if (r.cancelled) out += "\n(The command was stopped by the user.)";
    out += "\nExit code: " + std::to_string(r.exitCode);
    std::string summary = "Ran " + OneLine(command, 80) + ", exit code " + std::to_string(r.exitCode);
    if (r.timedOut) summary += ", timed out";
    return {out, r.exitCode != 0 || r.timedOut, summary};
}

json Schema(std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required) {
    json p = json::object();
    for (const auto& [name, schema] : props) p[name] = schema;
    json req = json::array();
    for (const char* r : required) req.push_back(r);
    return {{"type", "object"}, {"properties", p}, {"required", req}};
}

json Type(const char* type, const char* description) {
    return {{"type", type}, {"description", description}};
}

}  // namespace

std::vector<ToolDef> CodeToolDefinitions() {
    return {
        {"list_directory", "List the files and folders in a folder of the project.",
         Schema({{"path", Type("string", "Folder relative to the project root; '.' for the root")},
                 {"recursive", Type("boolean", "Include everything below it (skips .git, node_modules, build output)")}},
                {})},
        {"read_file", "Read a text file. Lines are numbered. Large files can be read in parts.",
         Schema({{"path", Type("string", "File path relative to the project root")},
                 {"start_line", Type("integer", "First line to read (1-based)")},
                 {"end_line", Type("integer", "Last line to read")}},
                {"path"})},
        {"write_file", "Create a file, or replace a file's whole content. Creates folders as needed.",
         Schema({{"path", Type("string", "File path relative to the project root")},
                 {"content", Type("string", "The complete new content of the file")}},
                {"path", "content"})},
        {"edit_file",
         "Replace exact text in a file. old_text must match the file exactly (including indentation) and be unique "
         "unless replace_all is set. Prefer this over write_file for changes to existing files.",
         Schema({{"path", Type("string", "File path relative to the project root")},
                 {"old_text", Type("string", "The exact text to replace")},
                 {"new_text", Type("string", "The text to put in its place")},
                 {"replace_all", Type("boolean", "Replace every occurrence")}},
                {"path", "old_text", "new_text"})},
        {"find_files", "Find files by name pattern (* and ? wildcards; ** crosses folders, e.g. src/**/*.cpp).",
         Schema({{"pattern", Type("string", "Name or path pattern")},
                 {"path", Type("string", "Folder to search under (default: the project root)")}},
                {"pattern"})},
        {"search_files", "Search file contents with a regular expression. Returns matching lines with file and line number.",
         Schema({{"pattern", Type("string", "Regular expression (ECMAScript syntax)")},
                 {"path", Type("string", "Folder to search under (default: the project root)")},
                 {"file_pattern", Type("string", "Only files whose names match this wildcard pattern, e.g. *.h")},
                 {"ignore_case", Type("boolean", "Case-insensitive search")}},
                {"pattern"})},
        {"run_command",
         "Run a shell command in the project folder and return its output and exit code. Use it to build, run tests, "
         "or use git. Commands must not wait for input.",
         Schema({{"command", Type("string", "The command line")},
                 {"timeout_seconds", Type("integer", "Stop the command after this many seconds (default 120)")}},
                {"command"})},
    };
}

static ToolOutcome RunToolUnchecked(const ToolCall& call, const ToolContext& context);

ToolOutcome RunTool(const ToolCall& call, const ToolContext& context) {
    // Files and command output can be in any encoding; what goes to the model
    // and the database must be UTF-8.
    ToolOutcome outcome = RunToolUnchecked(call, context);
    outcome.output = SanitizeUtf8(outcome.output);
    outcome.summary = SanitizeUtf8(outcome.summary);
    return outcome;
}

static ToolOutcome RunToolUnchecked(const ToolCall& call, const ToolContext& context) {
    json args = Args(call);
    if (!Trim(call.arguments).empty() && args.empty() && Trim(call.arguments) != "{}") {
        return Fail("The arguments were not valid JSON: " + OneLine(call.arguments, 200));
    }
    try {
        if (call.name == "list_directory") return ListDirectory(args, context);
        if (call.name == "read_file") return ReadFile(args, context);
        if (call.name == "write_file") return WriteFile(args, context);
        if (call.name == "edit_file") return EditFile(args, context);
        if (call.name == "find_files") return FindFiles(args, context);
        if (call.name == "search_files") return SearchFiles(args, context);
        if (call.name == "run_command") return RunCommand(args, context);
    } catch (const std::exception& e) {
        return Fail(std::string("The tool failed: ") + e.what());
    } catch (...) {
        return Fail("The tool failed.");
    }
    return Fail("There is no tool named " + call.name + ".");
}

std::string DescribeToolCall(const ToolCall& call) {
    json args = Args(call);
    const std::string& n = call.name;
    if (n == "list_directory") return "List " + Str(args, "path", ".");
    if (n == "read_file") return "Read " + Str(args, "path");
    if (n == "write_file") return "Write " + Str(args, "path");
    if (n == "edit_file") return "Edit " + Str(args, "path");
    if (n == "find_files") return "Find " + Str(args, "pattern");
    if (n == "search_files") return "Search for " + Str(args, "pattern");
    if (n == "run_command") return "Run: " + OneLine(Str(args, "command"), 120);
    return n + " " + OneLine(call.arguments, 80);
}

std::string CodeSystemPrompt(const std::string& folder) {
#ifdef _WIN32
    const char* os = "Windows (commands run in cmd.exe)";
#elif defined(__APPLE__)
    const char* os = "macOS (commands run in /bin/sh)";
#else
    const char* os = "Linux (commands run in /bin/sh)";
#endif
    return "You are a coding assistant working in the project folder " + folder + " on " + os +
           ". Use the tools to look at the code before changing it, make focused edits with edit_file, and check "
           "your work by building or running tests when the project has them. Paths are relative to the project "
           "folder. Keep explanations short; the user may be listening through a screen reader, so prefer plain "
           "sentences over tables and heavy formatting.";
}
