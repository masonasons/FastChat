#include "core/platform.h"
#include "core/util.h"

#include <cstdio>
#include <mutex>

void AppendLog(const std::string& text) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::string path = JoinPath(GetDataDir(), "fastchat.log");
#ifdef _WIN32
    FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"ab");
#else
    FILE* f = std::fopen(path.c_str(), "ab");
#endif
    if (!f) return;
    std::string line = FormatLocalTime(NowSeconds(), true) + "  " + text + "\n";
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);
}
