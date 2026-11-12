// Running shell commands for code conversations: cmd.exe /c in a job object, so
// a timeout or Stop ends the whole process tree.

#include "core/platform.h"
#include "core/util.h"

#include <windows.h>

#include <chrono>

namespace {

// Console output is in the OEM code page unless the program writes UTF-8; take it
// as UTF-8 when it is well-formed (the strict check in util), otherwise convert
// from the OEM code page.
std::string ConsoleToUtf8(const std::string& s) {
    if (s.empty() || IsValidUtf8(s)) return s;
    UINT cp = GetOEMCP();
    int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return WideToUtf8(w);
}

}  // namespace

ProcessResult RunShellCommand(const std::string& command, const std::string& workingDir, unsigned timeoutMs,
                              size_t maxOutputBytes, const std::atomic<bool>* cancel) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.error = "Could not create a pipe";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    wchar_t comspec[MAX_PATH] = L"cmd.exe";
    GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    std::wstring cmdLine = L"\"" + std::wstring(comspec) + L"\" /d /s /c \"" + Utf8ToWide(command) + L"\"";
    std::wstring dir = Utf8ToWide(workingDir);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                  dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
        result.error = "Could not start the command (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(readPipe);
        if (job) CloseHandle(job);
        return result;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    result.started = true;

    auto start = std::chrono::steady_clock::now();
    std::string raw;
    char buffer[8192];
    bool exited = false;
    for (;;) {
        DWORD available = 0;
        bool gotData = false;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                gotData = true;
                if (raw.size() < maxOutputBytes) {
                    raw.append(buffer, std::min<size_t>(read, maxOutputBytes - raw.size()));
                    if (raw.size() >= maxOutputBytes) result.truncated = true;
                } else {
                    result.truncated = true;
                }
            }
        }
        if (!gotData) {
            if (exited) break;
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                exited = true;  // one more pass drains what is left in the pipe
                continue;
            }
        }
        if (cancel && cancel->load()) {
            result.cancelled = true;
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (timeoutMs && elapsed.count() > timeoutMs) {
            result.timedOut = true;
            break;
        }
    }
    if (result.cancelled || result.timedOut) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    if (job) CloseHandle(job);
    result.output = ConsoleToUtf8(raw);
    return result;
}
