#pragma once

// What FastChat asks of the operating system: where things live, HTTP, keeping
// API keys out of plain text, and running commands for code conversations.
// One implementation per system in src/core/platform/.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Paths (UTF-8, no trailing separator)
// ---------------------------------------------------------------------------

// The folder holding FastChat's executable.
std::string GetExecutableDir();
// Where settings and the database live. Beside the executable when a file named
// "portable.txt" is there; otherwise the user's application data folder
// (%APPDATA%\FastChat, ~/Library/Application Support/FastChat, ~/.config/fastchat).
// Created if needed.
std::string GetDataDir();
// The user's home / documents folder, for folder pickers.
std::string GetHomeDir();
// Joins with the system separator.
std::string JoinPath(const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // Called with each piece of the body as it arrives (for streaming). When set,
    // the body is not also collected in HttpResponse::body unless the status is an
    // error (>= 400), so error messages can still be read. Return false to stop.
    std::function<bool(const char* data, size_t size)> onData;
    // Checked while waiting; set it to abandon the request.
    const std::atomic<bool>* cancel = nullptr;
    unsigned connectTimeoutMs = 30000;
    // Longest wait for the next bytes once connected. Reasoning models can sit
    // silent for minutes before the first token.
    unsigned receiveTimeoutMs = 600000;
};

struct HttpResponse {
    bool completed = false;   // a response arrived (any status)
    bool cancelled = false;
    int status = 0;
    std::map<std::string, std::string> headers;  // names lowercased
    std::string body;
    std::string error;        // why it failed, when it did

    bool Ok() const { return completed && status >= 200 && status < 300; }
    std::string Header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
};

// Blocking; call from a worker thread.
HttpResponse HttpSend(const HttpRequest& request);

// "FastChat/0.1.0 (Windows)"
std::string UserAgent();

// ---------------------------------------------------------------------------
// Secrets
// ---------------------------------------------------------------------------

// Turn an API key into text that is safe to keep in the settings file, and back.
// Windows: encrypted for the current user (DPAPI). Elsewhere: obfuscated only
// (base64) for now - the settings file is created readable by the user alone.
std::string ProtectSecret(const std::string& plain);
std::string UnprotectSecret(const std::string& stored);

// ---------------------------------------------------------------------------
// Running commands (code conversations)
// ---------------------------------------------------------------------------

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    std::string output;   // stdout and stderr together, in order
    bool truncated = false;
    std::string error;
};

// Run `command` through the system shell (cmd.exe /c, or /bin/sh -c) in
// `workingDir`. Output past `maxOutputBytes` is dropped (and `truncated` set).
ProcessResult RunShellCommand(const std::string& command, const std::string& workingDir,
                              unsigned timeoutMs, size_t maxOutputBytes,
                              const std::atomic<bool>* cancel);

// A program FastChat talks to over its standard input and output (Claude Code,
// Codex). Its whole process tree ends when this object goes away.
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // argv[0] is the program's full path. `env` adds to (or replaces in) this
    // process's environment.
    bool Start(const std::vector<std::string>& argv, const std::string& workingDir,
               const std::vector<std::pair<std::string, std::string>>& env, std::string& error);
    bool Write(const std::string& data);
    void CloseStdin();

    enum class Read { Line, Timeout, End };
    // The next line of standard output, without its line ending.
    Read ReadLine(std::string& line, unsigned timeoutMs);
    // Everything written to standard error so far.
    std::string Stderr();
    void Kill();
    // The exit code, or -1 if it has not exited within `timeoutMs`.
    int Wait(unsigned timeoutMs);

    struct Impl;

private:
    Impl* m_impl;
};

// The full path of a program on the PATH (with .exe, .cmd or .bat on Windows),
// or empty.
std::string FindProgram(const std::string& name);

// Add a line, with the time, to fastchat.log in the data folder (problems that
// would otherwise go unseen, such as unexpected exceptions). Any thread.
void AppendLog(const std::string& text);

// Open a folder or file with the system's default handler.
void OpenWithSystem(const std::string& path);
