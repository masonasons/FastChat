// A child process with pipes on Windows: a reader thread turns standard output
// into lines, another collects standard error, and a job object ends the whole
// tree when FastChat is done with it.

#include "core/platform.h"
#include "core/util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// Quote one argument so the C runtime's parser reads it back unchanged.
void AppendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') cmd.append(backslashes * 2 + 1, L'\\');
        else cmd.append(backslashes, L'\\');
        cmd += arg[i];
    }
    cmd += L'"';
}

bool IsBatch(const std::wstring& path) {
    std::wstring lower = path;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    return lower.size() > 4 && (lower.compare(lower.size() - 4, 4, L".cmd") == 0 || lower.compare(lower.size() - 4, 4, L".bat") == 0);
}

}  // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;
    std::thread outThread, errThread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool outEnded = false;
    std::string err;

    void ReadOut() {
        std::string partial;
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(stdoutRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            partial.append(buf, n);
            size_t start = 0;
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
                std::string line = partial.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
            partial.erase(0, start);
            cv.notify_all();
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!partial.empty()) lines.push_back(partial);
        outEnded = true;
        cv.notify_all();
    }

    void ReadErr() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(stderrRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (err.size() < 64 * 1024) err.append(buf, n);
        }
    }
};

ChildProcess::ChildProcess() : m_impl(new Impl()) {}

ChildProcess::~ChildProcess() {
    Kill();
    CloseStdin();
    if (m_impl->outThread.joinable()) m_impl->outThread.join();
    if (m_impl->errThread.joinable()) m_impl->errThread.join();
    if (m_impl->stdoutRead) CloseHandle(m_impl->stdoutRead);
    if (m_impl->stderrRead) CloseHandle(m_impl->stderrRead);
    if (m_impl->process) CloseHandle(m_impl->process);
    if (m_impl->job) CloseHandle(m_impl->job);
    delete m_impl;
}

bool ChildProcess::Start(const std::vector<std::string>& argv, const std::string& workingDir,
                         const std::vector<std::pair<std::string, std::string>>& env, std::string& error) {
    if (argv.empty()) {
        error = "No program given";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, outWrite = nullptr, errWrite = nullptr;
    if (!CreatePipe(&inRead, &m_impl->stdinWrite, &sa, 0) || !CreatePipe(&m_impl->stdoutRead, &outWrite, &sa, 0) ||
        !CreatePipe(&m_impl->stderrRead, &errWrite, &sa, 0)) {
        error = "Could not create pipes";
        return false;
    }
    SetHandleInformation(m_impl->stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(m_impl->stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(m_impl->stderrRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring program = Utf8ToWide(argv[0]);
    std::wstring cmd;
    if (IsBatch(program)) {
        // npm installs programs as .cmd files, which only cmd.exe can run.
        wchar_t comspec[MAX_PATH] = L"cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        std::wstring inner;
        AppendQuoted(inner, program);
        for (size_t i = 1; i < argv.size(); i++) {
            inner += L' ';
            AppendQuoted(inner, Utf8ToWide(argv[i]));
        }
        cmd = L"\"" + std::wstring(comspec) + L"\" /d /s /c \"" + inner + L"\"";
        program = comspec;
    } else {
        AppendQuoted(cmd, program);
        for (size_t i = 1; i < argv.size(); i++) {
            cmd += L' ';
            AppendQuoted(cmd, Utf8ToWide(argv[i]));
        }
    }

    // The environment: this process's, with `env` applied.
    std::vector<wchar_t> block;
    if (!env.empty()) {
        std::vector<std::wstring> entries;
        LPWCH current = GetEnvironmentStringsW();
        for (const wchar_t* e = current; e && *e; e += wcslen(e) + 1) {
            std::wstring entry(e);
            bool replaced = false;
            for (const auto& [name, value] : env) {
                std::wstring prefix = Utf8ToWide(name) + L"=";
                if (_wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0) replaced = true;
            }
            if (!replaced) entries.push_back(entry);
        }
        if (current) FreeEnvironmentStringsW(current);
        for (const auto& [name, value] : env) entries.push_back(Utf8ToWide(name) + L"=" + Utf8ToWide(value));
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const auto& e : entries) {
            block.insert(block.end(), e.begin(), e.end());
            block.push_back(0);
        }
        block.push_back(0);
    }

    m_impl->job = CreateJobObjectW(nullptr, nullptr);
    if (m_impl->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    std::wstring dir = Utf8ToWide(workingDir);
    BOOL ok = CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             block.empty() ? nullptr : block.data(), dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    DWORD lastError = GetLastError();
    CloseHandle(inRead);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!ok) {
        error = "Could not start " + argv[0] + " (error " + std::to_string(lastError) + ")";
        return false;
    }
    if (m_impl->job) AssignProcessToJobObject(m_impl->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    m_impl->process = pi.hProcess;
    m_impl->outThread = std::thread([this]() { m_impl->ReadOut(); });
    m_impl->errThread = std::thread([this]() { m_impl->ReadErr(); });
    return true;
}

bool ChildProcess::Write(const std::string& data) {
    if (!m_impl->stdinWrite) return false;
    DWORD written = 0;
    size_t done = 0;
    while (done < data.size()) {
        if (!WriteFile(m_impl->stdinWrite, data.data() + done, static_cast<DWORD>(data.size() - done), &written, nullptr)) {
            return false;
        }
        done += written;
    }
    return true;
}

void ChildProcess::CloseStdin() {
    if (m_impl->stdinWrite) {
        CloseHandle(m_impl->stdinWrite);
        m_impl->stdinWrite = nullptr;
    }
}

ChildProcess::Read ChildProcess::ReadLine(std::string& line, unsigned timeoutMs) {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this]() { return !m_impl->lines.empty() || m_impl->outEnded; });
    if (!m_impl->lines.empty()) {
        line = std::move(m_impl->lines.front());
        m_impl->lines.pop_front();
        return Read::Line;
    }
    return m_impl->outEnded ? Read::End : Read::Timeout;
}

std::string ChildProcess::Stderr() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->err;
}

void ChildProcess::Kill() {
    if (!m_impl->process) return;
    // The job holds anything it started too, which may still hold the pipes.
    if (m_impl->job) TerminateJobObject(m_impl->job, 1);
    else if (WaitForSingleObject(m_impl->process, 0) != WAIT_OBJECT_0) TerminateProcess(m_impl->process, 1);
    WaitForSingleObject(m_impl->process, 3000);
}

int ChildProcess::Wait(unsigned timeoutMs) {
    if (!m_impl->process) return -1;
    if (WaitForSingleObject(m_impl->process, timeoutMs) != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(m_impl->process, &code);
    return static_cast<int>(code);
}

std::string FindProgram(const std::string& name) {
    std::wstring wname = Utf8ToWide(name);
    for (const wchar_t* ext : {L".exe", L".cmd", L".bat", L""}) {
        wchar_t found[MAX_PATH * 2];
        if (SearchPathW(nullptr, wname.c_str(), ext, MAX_PATH * 2, found, nullptr)) return WideToUtf8(found);
    }
    // Where installers commonly put them, in case PATH has not caught up.
    wchar_t profile[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    wchar_t appdata[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    for (const std::wstring& candidate : {std::wstring(profile) + L"\\.local\\bin\\" + wname + L".exe",
                                          std::wstring(appdata) + L"\\npm\\" + wname + L".cmd"}) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return WideToUtf8(candidate);
    }
    return std::string();
}
