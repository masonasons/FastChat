#include "core/soundpack.h"
#include "core/platform.h"
#include "core/util.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

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
            if (StartsWith(info.name, ".")) continue;  // hidden, or a pack being imported
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

// ---------------------------------------------------------------------------
// Importing from a zip
// ---------------------------------------------------------------------------

namespace {

// A zip read into memory: FastChat's own file reading copes with any path.
class ZipReader {
public:
    ~ZipReader() {
        if (m_open) mz_zip_reader_end(&m_zip);
    }
    bool Open(const std::string& path, std::string& error) {
        if (!ReadFileText(path, m_data)) {
            error = "Could not open " + path + ".";
            return false;
        }
        if (!mz_zip_reader_init_mem(&m_zip, m_data.data(), m_data.size(), 0)) {
            error = path + " could not be read as a zip (" + mz_zip_get_error_string(mz_zip_get_last_error(&m_zip)) + ").";
            return false;
        }
        m_open = true;
        return true;
    }
    mz_zip_archive* Zip() { return &m_zip; }
    mz_uint Count() { return mz_zip_reader_get_num_files(&m_zip); }

    // Entry i's path ("a/b.wav"), if it is a sound to take: not a folder, and
    // not one of the Mac's resource fork copies.
    bool SoundEntry(mz_uint i, std::string& path) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m_zip, i, &st) || st.m_is_directory) return false;
        path = ReplaceAll(SanitizeUtf8(st.m_filename), "\\", "/");
        std::string base = BaseName(path);
        if (StartsWith(path, "__MACOSX/") || StartsWith(base, "._")) return false;
        size_t dot = base.find_last_of('.');
        return dot != std::string::npos && IsSoundExtension(base.substr(dot));
    }

    static std::string BaseName(const std::string& path) {
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

private:
    std::string m_data;
    mz_zip_archive m_zip{};
    bool m_open = false;
};

// A name that is safe as one folder name.
std::string CleanPackName(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    name = Trim(name);
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    while (!name.empty() && name.front() == '.') name.erase(0, 1);
    return name;
}

}  // namespace

std::string SoundPackNameForZip(const std::string& zipPath) {
    ZipReader zip;
    std::string error;
    if (!zip.Open(zipPath, error)) return std::string();
    std::set<std::string> tops;
    bool any = false, atRoot = false;
    for (mz_uint i = 0; i < zip.Count(); i++) {
        std::string path;
        if (!zip.SoundEntry(i, path)) continue;
        any = true;
        size_t slash = path.find('/');
        if (slash == std::string::npos) atRoot = true;
        else tops.insert(path.substr(0, slash));
    }
    std::string name;
    if (any && !atRoot && tops.size() == 1) name = CleanPackName(*tops.begin());
    if (name.empty()) name = CleanPackName(PathToUtf8(PathFromUtf8(zipPath).stem()));
    return name;
}

SoundPackImport ImportSoundPackZip(const std::string& zipPath, const std::string& packName) {
    SoundPackImport r;
    r.name = CleanPackName(packName);
    if (r.name.empty()) {
        r.error = "The sound pack needs a name.";
        return r;
    }
    ZipReader zip;
    if (!zip.Open(zipPath, r.error)) return r;

    std::string root = UserSoundPacksFolder();
    r.folder = JoinPath(root, r.name);
    // Unpacked beside it first (a hidden name, not listed as a pack), so a zip
    // that fails halfway leaves the old pack.
    std::string temp = JoinPath(root, "." + r.name + ".importing");
    std::error_code ec;
    fs::remove_all(PathFromUtf8(temp), ec);
    fs::create_directories(PathFromUtf8(temp), ec);
    if (ec) {
        r.error = "Could not create " + temp + ": " + ec.message();
        return r;
    }
    auto fail = [&](const std::string& why) {
        std::error_code ignore;
        fs::remove_all(PathFromUtf8(temp), ignore);
        r.error = why;
        r.sounds = 0;
        return r;
    };

    std::set<std::string> written;
    for (mz_uint i = 0; i < zip.Count(); i++) {
        std::string path;
        if (!zip.SoundEntry(i, path)) continue;
        std::string base = ZipReader::BaseName(path);
        if (!written.insert(ToLower(base)).second) continue;  // the first of the same name wins
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(zip.Zip(), i, &size, 0);
        if (!data) {
            return fail("Could not unpack " + path + " from the zip (" +
                        mz_zip_get_error_string(mz_zip_get_last_error(zip.Zip())) + ").");
        }
        std::string bytes(static_cast<const char*>(data), size);
        mz_free(data);
        std::string target = JoinPath(temp, base);
        if (!WriteFileText(target, bytes)) return fail("Could not write " + target + ".");
        r.sounds++;
    }
    if (r.sounds == 0) return fail("There are no sound files (.wav, .ogg, .mp3, .flac) in the zip.");

    fs::remove_all(PathFromUtf8(r.folder), ec);
    if (ec) return fail("Could not replace the pack already at " + r.folder + ": " + ec.message());
    fs::rename(PathFromUtf8(temp), PathFromUtf8(r.folder), ec);
    if (ec) return fail("Could not move the pack into place at " + r.folder + ": " + ec.message());
    ForgetSoundPackFiles();
    r.ok = true;
    return r;
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
        size_t next = i + 1;
        if (program == "git") {
            // git's own options come before the subcommand: git -C path commit.
            while (next < words.size() && StartsWith(words[next], "-")) {
                std::string opt = words[next++];
                if ((opt == "-C" || opt == "-c" || opt == "--git-dir" || opt == "--work-tree" || opt == "--namespace") &&
                    next < words.size()) {
                    next++;
                }
            }
        }
        if (next < words.size()) {
            // An option counts without its dashes: "python -m" gives "m".
            std::string sub = ToLower(words[next]);
            size_t dashes = sub.find_first_not_of('-');
            sub = dashes == std::string::npos ? std::string() : sub.substr(dashes);
            if (IsPlainWord(sub)) result.push_back(sub);
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
