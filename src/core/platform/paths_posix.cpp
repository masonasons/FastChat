#include "core/platform.h"
#include "core/util.h"
#include "version.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

std::string GetExecutableDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return fs::path(buf).parent_path().string();
    }
    return ".";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string(".") : p.parent_path().string();
#endif
}

std::string GetHomeDir() {
    const char* home = std::getenv("HOME");
    return home ? home : ".";
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
    if (fs::exists(exeDir + "/portable.txt")) {
        cached = exeDir;
        return cached;
    }
#ifdef __APPLE__
    std::string dir = GetHomeDir() + "/Library/Application Support/" APP_NAME;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string dir = (xdg && *xdg ? std::string(xdg) : GetHomeDir() + "/.config") + "/fastchat";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    cached = dir;
    return cached;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

void OpenWithSystem(const std::string& path) {
#ifdef __APPLE__
    std::string cmd = "open \"" + path + "\" &";
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}
