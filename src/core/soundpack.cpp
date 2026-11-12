#include "core/soundpack.h"
#include "core/platform.h"
#include "core/util.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace {

bool IsSoundExtension(const std::string& ext) {
    std::string e = ToLower(ext);
    return e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac";
}

// Where packs are looked for, the user's first.
std::vector<std::string> PackRoots() {
    std::vector<std::string> roots;
    roots.push_back(JoinPath(GetDataDir(), "soundpacks"));
    std::string exeDir = GetExecutableDir();
    roots.push_back(JoinPath(exeDir, "soundpacks"));
#ifdef __APPLE__
    roots.push_back(JoinPath(JoinPath(exeDir, "../Resources"), "soundpacks"));  // inside FastChat.app
#endif
    return roots;
}

// Pack folder -> lower-case file name without extension -> file.
std::map<std::string, std::map<std::string, std::string>>& FileCache() {
    static std::map<std::string, std::map<std::string, std::string>> cache;
    return cache;
}

const std::map<std::string, std::string>& PackFiles(const std::string& folder) {
    auto& cache = FileCache();
    auto it = cache.find(folder);
    if (it != cache.end()) return it->second;
    std::map<std::string, std::string> files;
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator d(PathFromUtf8(folder), ec), end; !ec && d != end; d.increment(ec)) {
        std::error_code fe;
        if (d->is_regular_file(fe) && IsSoundExtension(PathToUtf8(d->path().extension()))) found.push_back(d->path());
    }
    std::sort(found.begin(), found.end());  // the same pick every time when two formats share a name
    for (const auto& p : found) files.emplace(ToLower(PathToUtf8(p.stem())), PathToUtf8(p));
    return cache.emplace(folder, std::move(files)).first->second;
}

// Splits a command line into words, keeping quoted parts together (quotes removed).
std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    bool have = false;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            if (c == quote) quote = 0;
            else cur.push_back(c);
        } else if (c == '"' || c == '\'') {
            quote = c;
            have = true;
        } else if (c == ' ' || c == '\t') {
            if (have || !cur.empty()) words.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur.push_back(c);
        }
    }
    if (have || !cur.empty()) words.push_back(cur);
    return words;
}

// The command's pieces joined by && || ; | or new lines (quotes respected).
std::vector<std::string> SplitCommands(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            cur.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur.push_back(c);
        } else if (c == '&' || c == '|' || c == ';' || c == '\n' || c == '\r') {
            parts.push_back(cur);
            cur.clear();
            if (i + 1 < s.size() && (c == '&' || c == '|') && s[i + 1] == c) i++;
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

bool IsPlainWord(const std::string& w) {
    if (w.empty()) return false;
    for (char c : w) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::vector<SoundPackInfo> ListSoundPacks() {
    std::map<std::string, SoundPackInfo> byName;  // lower-case name -> pack; the first root wins
    for (const auto& root : PackRoots()) {
        std::error_code ec;
        for (fs::directory_iterator d(PathFromUtf8(root), ec), end; !ec && d != end; d.increment(ec)) {
            std::error_code fe;
            if (!d->is_directory(fe)) continue;
            SoundPackInfo info;
            info.name = PathToUtf8(d->path().filename());
            info.folder = PathToUtf8(d->path());
            byName.emplace(ToLower(info.name), info);
        }
    }
    std::vector<SoundPackInfo> packs;
    for (auto& [key, info] : byName) packs.push_back(info);
    return packs;
}

std::string UserSoundPacksFolder() {
    std::string folder = JoinPath(GetDataDir(), "soundpacks");
    std::error_code ec;
    fs::create_directories(PathFromUtf8(folder), ec);
    return folder;
}

// Pack name (lower case) -> its folder, or "" when there is none.
static std::map<std::string, std::string>& FolderCache() {
    static std::map<std::string, std::string> cache;
    return cache;
}

std::string FindSoundPackFolder(const std::string& name) {
    if (Trim(name).empty()) return std::string();
    std::string want = ToLower(name);
    auto& cache = FolderCache();
    if (auto it = cache.find(want); it != cache.end()) return it->second;
    std::string folder;
    for (const auto& p : ListSoundPacks()) {
        if (ToLower(p.name) == want) folder = p.folder;
    }
    cache[want] = folder;
    return folder;
}

std::string FindPackSound(const std::string& folder, const std::vector<std::string>& names) {
    if (folder.empty()) return std::string();
    const auto& files = PackFiles(folder);
    for (const auto& n : names) {
        auto it = files.find(ToLower(n));
        if (it != files.end()) return it->second;
    }
    return std::string();
}

bool PackHasEventSound(const std::string& folder, const std::string& eventKey) {
    if (folder.empty()) return false;
    std::string key = ToLower(eventKey);
    for (const auto& [name, path] : PackFiles(folder)) {
        if (name == key || StartsWith(name, key + "-")) return true;
    }
    return false;
}

void ForgetSoundPackFiles() {
    FileCache().clear();
    FolderCache().clear();
}

std::string ToolKind(const std::string& toolName) {
    std::string n = ToLower(toolName);
    if (n == "run_command" || n == "bash" || n == "powershell" || n == "shell" || n == "local_shell" ||
        n == "exec_command" || n == "bashoutput") {
        return "command";
    }
    if (n == "read_file" || n == "read" || n == "notebookread") return "read";
    if (n == "write_file" || n == "write") return "write";
    if (n == "edit_file" || n == "edit" || n == "multiedit" || n == "notebookedit" || n == "apply_patch") return "edit";
    if (n == "search_files" || n == "grep") return "search";
    if (n == "list_directory" || n == "find_files" || n == "glob" || n == "ls") return "list";
    if (n == "webfetch" || n == "websearch" || n == "web_fetch" || n == "web_search") return "web";
    if (n == "task" || n == "agent") return "agent";
    if (n == "todowrite" || n == "update_plan") return "todo";
    if (n == "exitplanmode" || n == "enterplanmode") return "plan";
    if (StartsWith(n, "mcp__") || StartsWith(n, "mcp_")) return "mcp";
    return std::string();
}

std::vector<std::string> CommandWords(const std::string& command) {
    for (const auto& part : SplitCommands(command)) {
        std::vector<std::string> words = SplitWords(Trim(part));
        size_t i = 0;
        // Variable settings in front of a command (FOO=1 make) and "call x.bat".
        while (i < words.size() && (words[i].find('=') != std::string::npos || ToLower(words[i]) == "call")) i++;
        if (i >= words.size()) continue;
        std::string program = words[i];
        size_t slash = program.find_last_of("\\/");
        if (slash != std::string::npos) program = program.substr(slash + 1);
        program = ToLower(program);
        for (const char* ext : {".exe", ".cmd", ".bat", ".com", ".ps1", ".sh"}) {
            if (EndsWith(program, ext) && program.size() > strlen(ext)) {
                program.resize(program.size() - strlen(ext));
                break;
            }
        }
        if (program.empty()) continue;
        // Moving around and setting things up is not what the command is about.
        if (program == "cd" || program == "chdir" || program == "pushd" || program == "popd" || program == "set" ||
            program == "export" || program == "setlocal" || program == "endlocal" || program == "cd.." ||
            program == "set-location" || program == "sl") {
            continue;
        }
        std::vector<std::string> result{program};
        if (i + 1 < words.size()) {
            std::string sub = ToLower(words[i + 1]);
            if (IsPlainWord(sub) && sub[0] != '-') result.push_back(sub);
        }
        return result;
    }
    return {};
}

std::vector<std::string> ToolSoundVariants(const std::string& toolName, const std::string& command) {
    std::vector<std::string> variants;
    std::string kind = ToolKind(toolName);
    if (kind == "command") {
        std::vector<std::string> words = CommandWords(command);
        if (words.size() >= 2) variants.push_back("command-" + words[0] + "-" + words[1]);
        if (!words.empty()) variants.push_back("command-" + words[0]);
    }
    std::string name = ToLower(Trim(toolName));
    if (!name.empty() && name != kind) variants.push_back(name);
    if (!kind.empty()) variants.push_back(kind);
    return variants;
}
