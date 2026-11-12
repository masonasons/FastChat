#include "core/platform.h"
#include "core/util.h"
#include "version.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <filesystem>

std::string GetExecutableDir() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring dir(path, n);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return WideToUtf8(dir);
}

std::string GetDataDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    // FASTCHAT_DATA_DIR puts the data elsewhere (development, tests).
    if (const char* over = std::getenv("FASTCHAT_DATA_DIR"); over && *over) {
        std::error_code ec;
        std::filesystem::create_directories(PathFromUtf8(over), ec);
        cached = over;
        return cached;
    }
    std::string exeDir = GetExecutableDir();
    std::wstring marker = Utf8ToWide(exeDir + "\\portable.txt");
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        cached = exeDir;
        return cached;
    }
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        std::wstring dir = std::wstring(appData) + L"\\" APP_NAME_W;
        CoTaskMemFree(appData);
        CreateDirectoryW(dir.c_str(), nullptr);
        cached = WideToUtf8(dir);
        return cached;
    }
    cached = exeDir;
    return cached;
}

std::string GetHomeDir() {
    PWSTR docs = nullptr;
    std::string result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &docs)) && docs) {
        result = WideToUtf8(docs);
    }
    CoTaskMemFree(docs);
    return result;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

void OpenWithSystem(const std::string& path) {
    ShellExecuteW(nullptr, L"open", Utf8ToWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
