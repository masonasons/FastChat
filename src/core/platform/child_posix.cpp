// A child process with pipes on macOS and Linux, in its own process group so
// that Kill ends everything it started.

#include "core/platform.h"
#include "core/util.h"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

struct ChildProcess::Impl {
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;
    std::thread outThread, errThread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool outEnded = false;
    std::string errText;
    int exitCode = -1;
    bool reaped = false;
};

ChildProcess::ChildProcess() : m_impl(new Impl()) {}

ChildProcess::~ChildProcess() {
    Kill();
    CloseStdin();
    if (m_impl->outThread.joinable()) m_impl->outThread.join();
    if (m_impl->errThread.joinable()) m_impl->errThread.join();
    if (m_impl->out >= 0) close(m_impl->out);
    if (m_impl->err >= 0) close(m_impl->err);
    Wait(1000);
    delete m_impl;
}

bool ChildProcess::Start(const std::vector<std::string>& argv, const std::string& workingDir,
                         const std::vector<std::pair<std::string, std::string>>& env, std::string& error) {
    int inPipe[2], outPipe[2], errPipe[2];
    if (pipe(inPipe) || pipe(outPipe) || pipe(errPipe)) {
        error = "Could not create pipes";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        error = "Could not start " + argv[0];
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(inPipe[1]);
        close(outPipe[0]);
        close(errPipe[0]);
        if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(127);
        for (const auto& [name, value] : env) setenv(name.c_str(), value.c_str(), 1);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(argv[0].c_str(), args.data());
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    m_impl->pid = pid;
    m_impl->in = inPipe[1];
    m_impl->out = outPipe[0];
    m_impl->err = errPipe[0];
    m_impl->outThread = std::thread([this]() {
        std::string partial;
        char buf[8192];
        ssize_t n;
        while ((n = read(m_impl->out, buf, sizeof(buf))) > 0) {
            partial.append(buf, static_cast<size_t>(n));
            size_t start = 0;
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            for (size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
                m_impl->lines.push_back(partial.substr(start, nl - start));
            }
            partial.erase(0, start);
            m_impl->cv.notify_all();
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!partial.empty()) m_impl->lines.push_back(partial);
        m_impl->outEnded = true;
        m_impl->cv.notify_all();
    });
    m_impl->errThread = std::thread([this]() {
        char buf[4096];
        ssize_t n;
        while ((n = read(m_impl->err, buf, sizeof(buf))) > 0) {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (m_impl->errText.size() < 64 * 1024) m_impl->errText.append(buf, static_cast<size_t>(n));
        }
    });
    return true;
}

bool ChildProcess::Write(const std::string& data) {
    if (m_impl->in < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(m_impl->in, data.data() + done, data.size() - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

void ChildProcess::CloseStdin() {
    if (m_impl->in >= 0) {
        close(m_impl->in);
        m_impl->in = -1;
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
    return m_impl->errText;
}

void ChildProcess::Kill() {
    if (m_impl->pid > 0 && !m_impl->reaped) kill(-m_impl->pid, SIGKILL);
}

int ChildProcess::Wait(unsigned timeoutMs) {
    if (m_impl->pid <= 0) return -1;
    if (m_impl->reaped) return m_impl->exitCode;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        int status = 0;
        if (waitpid(m_impl->pid, &status, WNOHANG) == m_impl->pid) {
            m_impl->reaped = true;
            m_impl->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return m_impl->exitCode;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < end);
    return -1;
}

std::string FindProgram(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "";
    std::string home = GetHomeDir();
    // Where package managers put programs, in case the PATH FastChat was opened
    // with has not caught up (or was set before they were installed).
    dirs += ":" + home + "/.local/bin:" + home + "/.volta/bin:" + home + "/.bun/bin:" +
            home + "/.npm-global/bin:" + home + "/Library/pnpm:/usr/local/bin:/opt/homebrew/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t colon = dirs.find(':', start);
        std::string dir = dirs.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return std::string();
}
