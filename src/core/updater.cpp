#include "core/updater.h"
#include "core/platform.h"
#include "core/util.h"

#include "build_commit.h"  // generated: FASTCHAT_BUILD_COMMIT

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool LooksLikeCommit(const std::string& s) {
    if (s.size() < 7) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// The commit a release was built from: its target when that is a commit (CI
// creates it at the commit), else the first run of 7+ hex digits in its text.
std::string ReleaseCommit(const json& release) {
    std::string target = release.value("target_commitish", std::string());
    if (LooksLikeCommit(target)) return target;
    std::string body = release.value("body", std::string());
    for (size_t i = 0; i < body.size();) {
        if (!std::isxdigit(static_cast<unsigned char>(body[i]))) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < body.size() && std::isxdigit(static_cast<unsigned char>(body[j]))) j++;
        if (j - i >= 7) return body.substr(i, j - i);
        i = j;
    }
    return std::string();
}

std::string AssetUrl(const json& release, const std::string& name) {
    for (const auto& a : release.value("assets", json::array())) {
        if (a.is_object() && a.value("name", std::string()) == name) return a.value("browser_download_url", std::string());
    }
    return std::string();
}

// Quoted for a shell script (single quotes, with any inside escaped).
std::string ShellQuote(const std::string& s) {
    return "'" + ReplaceAll(s, "'", "'\\''") + "'";
}

// For a batch file: % doubled (it would expand otherwise).
std::string Bat(const std::string& s) {
    return ReplaceAll(s, "%", "%%");
}

// For a single-quoted PowerShell string inside the batch file.
std::string Ps(const std::string& s) {
    return Bat(ReplaceAll(s, "'", "''"));
}

}  // namespace

const char* BuildCommit() {
    return FASTCHAT_BUILD_COMMIT;
}

const char* UpdatePackageName() {
#ifdef __APPLE__
    return "FastChat-mac.zip";
#else
    return "FastChat.zip";
#endif
}

UpdateInfo ReadLatestRelease(const std::string& releaseJson, const std::string& currentCommit, const std::string& package) {
    UpdateInfo info;
    json release = json::parse(releaseJson, nullptr, false);
    if (release.is_discarded() || !release.is_object()) {
        info.error = "GitHub's answer could not be read.";
        return info;
    }
    std::string remote = ReleaseCommit(release);
    info.commit = remote.substr(0, 7);
    info.notes = release.value("body", std::string());
    info.downloadUrl = AssetUrl(release, package);
    if (remote.empty() || info.downloadUrl.empty()) {
        info.error = "No build of FastChat for this system has been published yet.";
        return info;
    }
    // A build made outside git cannot be compared: it is never offered one.
    if (currentCommit.empty()) return info;
    info.available = info.commit != currentCommit.substr(0, 7);
    return info;
}

UpdateInfo CheckForUpdate() {
    HttpRequest request;
    request.url = std::string("https://api.github.com/repos/") + kUpdateRepo + "/releases/tags/latest";
    request.headers.push_back({"Accept", "application/vnd.github+json"});
    request.connectTimeoutMs = 20000;
    request.receiveTimeoutMs = 30000;
    HttpResponse response = HttpSend(request);
    if (!response.Ok()) {
        UpdateInfo info;
        if (response.status == 404) info.error = "No build of FastChat has been published yet.";
        else if (!response.error.empty()) info.error = "GitHub could not be reached: " + response.error;
        else info.error = "GitHub answered with an error (HTTP " + std::to_string(response.status) + ").";
        return info;
    }
    return ReadLatestRelease(response.body, BuildCommit(), UpdatePackageName());
}

bool DownloadUpdate(const std::string& url, std::string& path, std::string& error) {
    HttpRequest request;
    request.url = url;
    request.receiveTimeoutMs = 120000;
    HttpResponse response = HttpSend(request);
    if (!response.Ok() || response.body.empty()) {
        error = response.error.empty() ? "The download failed (HTTP " + std::to_string(response.status) + ")." : response.error;
        return false;
    }
    std::error_code ec;
    fs::path temp = fs::temp_directory_path(ec);
    if (ec) {
        error = "There is no temporary folder to download to.";
        return false;
    }
    path = PathToUtf8(temp / UpdatePackageName());
    if (!WriteFileText(path, response.body)) {
        error = "The download could not be saved to " + path + ".";
        return false;
    }
    return true;
}

std::string UpdateBlocker(const std::string& appDir) {
    fs::path dir = PathFromUtf8(appDir);
#ifdef __APPLE__
    dir = dir.parent_path();  // the folder the app bundle is in
#endif
    std::error_code ec;
    if (fs::exists(dir / "CMakeLists.txt", ec) || fs::exists(dir / ".git", ec)) {
        return "This FastChat runs from its source folder (a build of your own), which the updater does not write over. "
               "Pull and build instead, or download the latest build from https://github.com/" +
               std::string(kUpdateRepo) + "/releases/latest.";
    }
    // Can the files there be replaced?
    fs::path probe = dir / (".fastchat-update-check-" + NewId().substr(0, 8));
    if (!WriteFileText(PathToUtf8(probe), "")) {
        return "FastChat cannot write to its folder (" + PathToUtf8(dir) +
               "), so it cannot update itself there. Download the latest build from https://github.com/" +
               std::string(kUpdateRepo) + "/releases/latest.";
    }
    fs::remove(probe, ec);
    return std::string();
}

std::string WindowsUpdateScript(unsigned long pid, const std::string& zipPath, const std::string& extractDir,
                                const std::string& appDir) {
    // Waits for FastChat to quit (a minute at most), unzips the package, puts its
    // FastChat folder over FastChat's, starts it again and deletes itself. On
    // Windows 7's PowerShell 2, which has no Expand-Archive, the shell unzips it.
    std::string p = std::to_string(pid);
    std::string zip = Bat(zipPath), extract = Bat(extractDir), app = Bat(appDir);
    std::string s;
    s += "@echo off\r\nchcp 65001 >nul\r\nsetlocal enableextensions\r\n";
    // Started from PowerShell 7, its module path would lead Windows PowerShell
    // to modules it cannot load (Expand-Archive among them); without one it
    // uses its own.
    s += "set \"PSModulePath=\"\r\n";
    s += "set /a TRIES=0\r\n";
    s += ":wait\r\n";
    s += "tasklist /fi \"PID eq " + p + "\" 2>nul | find \"" + p + "\" >nul\r\n";
    s += "if errorlevel 1 goto unzip\r\n";
    s += "set /a TRIES+=1\r\n";
    s += "if %TRIES% GEQ 60 goto finish\r\n";
    s += "ping -n 2 127.0.0.1 >nul\r\n";
    s += "goto wait\r\n";
    s += ":unzip\r\n";
    s += "if exist \"" + extract + "\" rmdir /s /q \"" + extract + "\"\r\n";
    s += "set \"PSMAJOR=0\"\r\n";
    s += "for /f \"usebackq delims=\" %%v in (`powershell -NoProfile -Command \"$PSVersionTable.PSVersion.Major\" 2^>nul`) do set \"PSMAJOR=%%v\"\r\n";
    s += "if \"%PSMAJOR%\"==\"\" set \"PSMAJOR=0\"\r\n";
    // Three ways to unzip, each tried while the package's FastChat.exe has not
    // appeared: Expand-Archive (PowerShell 5), tar (Windows 10 and later), and
    // the shell's own zip folders (Windows 7).
    std::string missing = "if not exist \"" + extract + "\\FastChat\\FastChat.exe\" if not exist \"" + extract + "\\FastChat.exe\" ";
    s += "if %PSMAJOR% GEQ 5 powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
         Ps(zipPath) + "' -DestinationPath '" + Ps(extractDir) + "' -Force\"\r\n";
    s += missing + "if exist \"%SystemRoot%\\System32\\tar.exe\" (\r\n";
    s += "  if not exist \"" + extract + "\" mkdir \"" + extract + "\"\r\n";
    s += "  \"%SystemRoot%\\System32\\tar.exe\" -xf \"" + zip + "\" -C \"" + extract + "\"\r\n";
    s += ")\r\n";
    s += missing + "(\r\n";
    s += "  powershell -NoProfile -ExecutionPolicy Bypass -Command \"$null=New-Item -ItemType Directory -Force -Path '" +
         Ps(extractDir) + "'; $s=New-Object -ComObject Shell.Application; $z=$s.NameSpace('" + Ps(zipPath) +
         "'); $d=$s.NameSpace('" + Ps(extractDir) +
         "'); $d.CopyHere($z.Items(),20); $n=$z.Items().Count; $t=0; "
         "do { Start-Sleep -Milliseconds 300; $t=$t+1 } while ($d.Items().Count -lt $n -and $t -lt 200); "
         "Start-Sleep -Milliseconds 1500\"\r\n";
    s += ")\r\n";
    // The zip holds a FastChat folder; take what is in it.
    s += "set \"SRC=" + extract + "\"\r\n";
    s += "if exist \"" + extract + "\\FastChat\\FastChat.exe\" set \"SRC=" + extract + "\\FastChat\"\r\n";
    s += "if exist \"%SRC%\\FastChat.exe\" xcopy /s /e /y /q \"%SRC%\\*\" \"" + app + "\\\" >nul\r\n";
    s += "rmdir /s /q \"" + extract + "\"\r\n";
    s += "del /q \"" + zip + "\"\r\n";
    s += ":finish\r\n";
    s += "start \"\" \"" + app + "\\FastChat.exe\"\r\n";
    s += "del \"%~f0\"\r\n";
    return s;
}

std::string MacUpdateScript(long pid, const std::string& zipPath, const std::string& extractDir, const std::string& appBundle) {
    // Waits for FastChat to quit (a minute at most), unzips the package, swaps
    // the app bundle for the new one (the old one comes back if that fails),
    // opens it and deletes itself.
    std::string zip = ShellQuote(zipPath), extract = ShellQuote(extractDir), app = ShellQuote(appBundle),
                old = ShellQuote(appBundle + ".old");
    std::string s;
    s += "#!/bin/sh\n";
    s += "i=0\n";
    s += "while kill -0 " + std::to_string(pid) + " 2>/dev/null; do\n";
    s += "  i=$((i+1)); [ $i -gt 120 ] && exit 1\n";
    s += "  sleep 0.5\n";
    s += "done\n";
    s += "rm -rf " + extract + "\n";
    s += "mkdir -p " + extract + "\n";
    s += "if ditto -x -k " + zip + " " + extract + "; then\n";
    s += "  NEW=$(find " + extract + " -maxdepth 3 -type d -name FastChat.app | head -n 1)\n";
    s += "  if [ -n \"$NEW\" ]; then\n";
    s += "    rm -rf " + old + "\n";
    s += "    if mv " + app + " " + old + "; then\n";
    s += "      if ditto \"$NEW\" " + app + "; then rm -rf " + old + "; else rm -rf " + app + "; mv " + old + " " + app + "; fi\n";
    s += "    fi\n";
    s += "  fi\n";
    s += "fi\n";
    s += "rm -rf " + extract + " " + zip + "\n";
    s += "open " + app + "\n";
    s += "rm -f \"$0\"\n";
    return s;
}

// Where the running FastChat lives: its folder, or on the Mac its app bundle
// ("" when it is not in one).
static std::string AppLocation() {
    std::string appDir = GetExecutableDir();
#ifdef __APPLE__
    // .../FastChat.app/Contents/MacOS: the bundle is two folders up.
    fs::path bundle = PathFromUtf8(appDir).parent_path().parent_path();
    return bundle.extension() == ".app" ? PathToUtf8(bundle) : std::string();
#else
    return appDir;
#endif
}

std::string UpdateBlockerHere() {
    std::string app = AppLocation();
    if (app.empty()) return "FastChat is not running from an app bundle, so it cannot replace itself.";
    return UpdateBlocker(app);
}

bool StartUpdateInstall(const std::string& packagePath, std::string& error) {
    std::string appDir = AppLocation();
    if (std::string blocker = UpdateBlockerHere(); !blocker.empty()) {
        error = blocker;
        return false;
    }
    std::error_code ec;
    fs::path temp = fs::temp_directory_path(ec);
    std::string extract = PathToUtf8(temp / "FastChat-update");
#ifdef _WIN32
    std::string script = PathToUtf8(temp / "fastchat-update.bat");
    if (!WriteFileText(script, WindowsUpdateScript(GetCurrentProcessId(), packagePath, extract, appDir))) {
        error = "The update script could not be written.";
        return false;
    }
    // cmd runs it while FastChat quits, in a hidden console that the programs it
    // starts (PowerShell, xcopy) share, so no window appears.
    std::wstring command = L"cmd.exe /c \"\"" + Utf8ToWide(script) + L"\"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = "The update could not be started (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::string script = PathToUtf8(temp / "fastchat-update.sh");
    if (!WriteFileText(script, MacUpdateScript(static_cast<long>(getpid()), packagePath, extract, appDir))) {
        error = "The update script could not be written.";
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        error = "The update could not be started.";
        return false;
    }
    if (child == 0) {
        setsid();  // its own session: it outlives FastChat
        execl("/bin/sh", "sh", script.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}
