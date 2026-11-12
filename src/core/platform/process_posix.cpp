// Running shell commands for code conversations: /bin/sh -c in its own process
// group, so a timeout or Stop ends everything it started.

#include "core/platform.h"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

ProcessResult RunShellCommand(const std::string& command, const std::string& workingDir, unsigned timeoutMs,
                              size_t maxOutputBytes, const std::atomic<bool>* cancel) {
    ProcessResult result;
    int fds[2];
    if (pipe(fds) != 0) {
        result.error = "Could not create a pipe";
        return result;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.error = "Could not start the command";
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    result.started = true;
    auto start = std::chrono::steady_clock::now();
    char buffer[8192];
    for (;;) {
        struct pollfd p{fds[0], POLLIN, 0};
        int r = poll(&p, 1, 50);
        if (r > 0) {
            ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n <= 0) break;  // the write end closed: the command is done
            if (result.output.size() < maxOutputBytes) {
                result.output.append(buffer, std::min<size_t>(static_cast<size_t>(n), maxOutputBytes - result.output.size()));
            }
            if (result.output.size() >= maxOutputBytes) result.truncated = true;
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
    if (result.cancelled || result.timedOut) kill(-pid, SIGKILL);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
