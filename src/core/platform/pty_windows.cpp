// A pseudo-terminal on Windows (ConPTY), off screen: FreeBuff's interface is a
// full-screen terminal UI, and the only way to drive it is a real terminal.
//
// The ConPTY calls are looked up at run time rather than linked: FastChat still
// runs on Windows 7, where kernel32 has none of them. FreeBuff itself needs
// Windows 10, so the missing functions are reported as what they are.

#include "core/platform.h"
#include "core/util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE from winbase.h; spelled out here because
// the SDK's copy is only visible when targeting Windows 10.
const DWORD kPseudoConsoleAttribute = 0x00020016;

typedef HRESULT(WINAPI* CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, HANDLE*);
typedef void(WINAPI* ClosePseudoConsoleFn)(HANDLE);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    bool available() const { return create && close; }
};

ConPtyApi& PtyApi() {
    static ConPtyApi api = [] {
        ConPtyApi found;
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return found;
        found.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
        found.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
        return found;
    }();
    return api;
}

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
    return lower.size() > 4 &&
           (lower.compare(lower.size() - 4, 4, L".cmd") == 0 || lower.compare(lower.size() - 4, 4, L".bat") == 0);
}

// This process's environment, with `env` applied: npm's launchers need the PATH
// FastChat found FreeBuff on, not whatever PATH a bare CreateProcess starts with.
std::vector<wchar_t> EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& env) {
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
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

}  // namespace

struct PseudoTerminal::Impl {
    HANDLE pty = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD pid = 0;
    std::thread reader;
    std::mutex mutex;
    std::condition_variable cv;
    std::string buffer;
    bool ended = false;

    void ReadLoop() {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            buffer.append(buf, n);
            cv.notify_all();
        }
        std::lock_guard<std::mutex> lock(mutex);
        ended = true;
        cv.notify_all();
    }
};

PseudoTerminal::PseudoTerminal() : m_impl(new Impl()) {}

PseudoTerminal::~PseudoTerminal() {
    Kill();
    if (m_impl->reader.joinable()) m_impl->reader.join();
    if (m_impl->outRead) CloseHandle(m_impl->outRead);
    if (m_impl->inWrite) CloseHandle(m_impl->inWrite);
    if (m_impl->process) CloseHandle(m_impl->process);
    if (m_impl->job) CloseHandle(m_impl->job);
    delete m_impl;
}

bool PseudoTerminal::Start(const std::vector<std::string>& argv, const std::string& workingDir,
                           const std::vector<std::pair<std::string, std::string>>& env, int cols, int rows,
                           std::string& error) {
    if (argv.empty()) {
        error = "No program given";
        return false;
    }
    if (!PtyApi().available()) {
        error = "This Windows cannot run a terminal program (it needs Windows 10 version 1809 or later).";
        return false;
    }
    HANDLE ptyIn = nullptr, ptyOut = nullptr;
    if (!CreatePipe(&ptyIn, &m_impl->inWrite, nullptr, 0) || !CreatePipe(&m_impl->outRead, &ptyOut, nullptr, 0)) {
        error = "Could not create the terminal's pipes";
        return false;
    }
    COORD size{static_cast<SHORT>(std::max(1, cols)), static_cast<SHORT>(std::max(1, rows))};
    HRESULT created = PtyApi().create(size, ptyIn, ptyOut, 0, &m_impl->pty);
    if (FAILED(created)) {
        CloseHandle(ptyIn);
        CloseHandle(ptyOut);
        error = "Could not create a terminal for FreeBuff";
        return false;
    }
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

    SIZE_T attributes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes);
    std::vector<char> storage(attributes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    std::wstring dir = Utf8ToWide(workingDir);
    std::vector<wchar_t> block = EnvironmentBlock(env);
    bool started = false;
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attributes) ||
        !UpdateProcThreadAttribute(list, 0, kPseudoConsoleAttribute, &m_impl->pty, sizeof(m_impl->pty), nullptr,
                                   nullptr)) {
        error = "Could not prepare FreeBuff's terminal";
    } else {
        // The list is what tells Windows to run the child on this terminal;
        // built and left unassigned, the child quietly keeps this process's own
        // console and the terminal is never written to.
        si.lpAttributeList = list;
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, block.data(),
                                 dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
        DWORD lastError = GetLastError();
        if (!ok) {
            error = "Could not start " + argv[0] + " (error " + std::to_string(lastError) + ")";
        } else {
            m_impl->job = CreateJobObjectW(nullptr, nullptr);
            if (m_impl->job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(m_impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
                AssignProcessToJobObject(m_impl->job, pi.hProcess);
            }
            CloseHandle(pi.hThread);
            m_impl->process = pi.hProcess;
            m_impl->pid = pi.dwProcessId;
            m_impl->reader = std::thread([this]() { m_impl->ReadLoop(); });
            started = true;
        }
    }
    DeleteProcThreadAttributeList(list);
    // The terminal holds its own copies; these are only needed to create it,
    // and are let go once the client that uses them is running.
    CloseHandle(ptyIn);
    CloseHandle(ptyOut);
    if (!started) {
        if (m_impl->pty) {
            PtyApi().close(m_impl->pty);
            m_impl->pty = nullptr;
        }
        if (m_impl->job) {
            CloseHandle(m_impl->job);
            m_impl->job = nullptr;
        }
    }
    return started;
}

bool PseudoTerminal::Write(const std::string& data) {
    if (!m_impl->inWrite) return false;
    DWORD written = 0;
    size_t done = 0;
    while (done < data.size()) {
        if (!WriteFile(m_impl->inWrite, data.data() + done, static_cast<DWORD>(data.size() - done), &written,
                       nullptr)) {
            return false;
        }
        done += written;
    }
    return true;
}

bool PseudoTerminal::Read(std::string& out, unsigned timeoutMs) {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    m_impl->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this]() { return !m_impl->buffer.empty() || m_impl->ended; });
    if (m_impl->buffer.empty()) return false;
    out.swap(m_impl->buffer);
    return true;
}

bool PseudoTerminal::Ended() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->ended;
}

int PseudoTerminal::Pid() const { return static_cast<int>(m_impl->pid); }

void PseudoTerminal::Kill() {
    if (m_impl->process) {
        if (m_impl->job) TerminateJobObject(m_impl->job, 1);
        else if (WaitForSingleObject(m_impl->process, 0) != WAIT_OBJECT_0) TerminateProcess(m_impl->process, 1);
        WaitForSingleObject(m_impl->process, 3000);
    }
    if (m_impl->pty) {
        // FreeBuff's whole interface goes with the terminal; closing it also
        // ends the read on the output pipe.
        PtyApi().close(m_impl->pty);
        m_impl->pty = nullptr;
    }
    if (m_impl->inWrite) {
        CloseHandle(m_impl->inWrite);
        m_impl->inWrite = nullptr;
    }
}
