// A pseudo-terminal on macOS and Linux, in its own process group so that Kill
// ends everything FreeBuff started.

#include "core/platform.h"
#include "core/util.h"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

struct PseudoTerminal::Impl {
    pid_t pid = -1;
    int fd = -1;
    std::thread reader;
    std::mutex mutex;
    std::condition_variable cv;
    std::string buffer;
    bool ended = false;
    int exitCode = -1;
    bool reaped = false;

    void ReadLoop() {
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            buffer.append(buf, static_cast<size_t>(n));
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
    if (m_impl->fd >= 0) close(m_impl->fd);
    if (m_impl->pid > 0 && !m_impl->reaped) {
        int status = 0;
        waitpid(m_impl->pid, &status, WNOHANG);
    }
    delete m_impl;
}

bool PseudoTerminal::Start(const std::vector<std::string>& argv, const std::string& workingDir,
                           const std::vector<std::pair<std::string, std::string>>& env, int cols, int rows,
                           std::string& error) {
    if (argv.empty()) {
        error = "No program given";
        return false;
    }
    struct winsize size{};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &size);
    if (pid < 0) {
        error = "Could not create a terminal for FreeBuff";
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(127);
        // A full-screen interface needs to know what sort of terminal it is in;
        // without these it draws nothing at all.
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("PAGER", "cat", 1);
        for (const auto& [name, value] : env) setenv(name.c_str(), value.c_str(), 1);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(argv[0].c_str(), args.data());
        _exit(127);
    }
    setpgid(pid, pid);
    m_impl->pid = pid;
    m_impl->fd = fd;
    m_impl->reader = std::thread([this]() { m_impl->ReadLoop(); });
    return true;
}

bool PseudoTerminal::Write(const std::string& data) {
    if (m_impl->fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(m_impl->fd, data.data() + done, data.size() - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
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
    if (m_impl->pid > 0 && !m_impl->reaped) {
        kill(-m_impl->pid, SIGKILL);
        int status = 0;
        if (waitpid(m_impl->pid, &status, WNOHANG) == m_impl->pid) {
            m_impl->reaped = true;
            m_impl->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }
    if (m_impl->fd >= 0) {
        close(m_impl->fd);
        m_impl->fd = -1;
    }
}
