#include "shell_env.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Vars worth importing. PATH is the headline; the rest help agent tool calls
// and subprocess behavior (ssh-agent forwarding, locale, version managers).
constexpr const char* kVars[] = {
    "PATH", "SSH_AUTH_SOCK", "HOMEBREW_PREFIX", "MANPATH",
    "LANG", "LC_ALL", "NVM_DIR", "PYENV_ROOT", "RBENV_ROOT",
};
constexpr int kNVars = sizeof(kVars) / sizeof(kVars[0]);

constexpr const char* kStartMarker = "__WXGRIT_ENV_START__\n";
constexpr const char* kEndMarker   = "\n__WXGRIT_ENV_END__";

int CloexecPipe(int fds[2]) {
#ifdef O_CLOEXEC
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) < 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

}  // namespace

void ImportShellEnv() {
    const char* shell = std::getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";

    // printf '__WXGRIT_ENV_START__\n%s\n%s\n...\n__WXGRIT_ENV_END__\n' "$PATH" ...
    std::string cmd = "printf '__WXGRIT_ENV_START__\\n";
    for (int i = 0; i < kNVars; i++) cmd += "%s\\n";
    cmd += "__WXGRIT_ENV_END__\\n'";
    for (int i = 0; i < kNVars; i++) {
        cmd += " \"$";
        cmd += kVars[i];
        cmd += "\"";
    }

    int pfd[2];
    if (CloexecPipe(pfd) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }

    if (pid == 0) {
        // Child: own session group so the parent can SIGKILL the whole tree
        // on timeout (rc files sometimes spawn background helpers).
        setsid();
        dup2(pfd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        // Hint to rc-files that this is a non-interactive bootstrap so heavy
        // banners/MOTDs can be skipped if the user checks the var.
        setenv("WXGRIT_RESOLVING_ENVIRONMENT", "1", 1);
        execl(shell, shell, "-ilc", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(pfd[1]);

    std::string output;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    struct pollfd rpfd = {pfd[0], POLLIN, 0};
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { kill(-pid, SIGKILL); break; }
        int ret = poll(&rpfd, 1, (int)std::min<long>(remaining, 500));
        if (ret < 0) break;
        if (ret == 0) continue;
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n <= 0) break;
        output.append(buf, n);
        if (output.size() > 65536) break;  // sanity cap
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    // Locate the last START/END pair — rc-files sometimes echo during sourcing
    // and we want the final clean block.
    size_t start = output.rfind(kStartMarker);
    size_t end = output.rfind(kEndMarker);
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return;
    }
    start += std::strlen(kStartMarker);
    std::string body = output.substr(start, end - start);

    std::vector<std::string> values;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) { values.push_back(body.substr(pos)); break; }
        values.push_back(body.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if ((int)values.size() < kNVars) return;

    for (int i = 0; i < kNVars; i++) {
        if (!values[i].empty()) setenv(kVars[i], values[i].c_str(), 1);
    }
}
