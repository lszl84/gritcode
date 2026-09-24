#include "claude_agent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cwctype>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Callbacks live in a shared holder so CallAfter closures queued by the
// worker stay valid even after the ClaudeAgentProcess itself is replaced.
struct ClaudeCbHolder {
    ClaudeAgentProcess::DataFn data;
    ClaudeAgentProcess::DoneFn done;
};

struct ClaudeProcessImpl {
    std::atomic<bool> cancelRequested{false};  // Cancel(): graceful stop
    std::atomic<bool> killNow{false};          // destructor: kill immediately
    std::atomic<bool> active{true};
    std::thread worker;
    std::shared_ptr<ClaudeCbHolder> cb;
#ifdef _WIN32
    // Job object holding the CLI and everything it spawns. Guarded by jobMu
    // because Cancel() (GUI thread) races the worker closing it.
    std::mutex jobMu;
    HANDLE job = nullptr;
#endif
};

namespace {

using Clock = std::chrono::steady_clock;
using OutFn = std::function<void(const char*, size_t)>;

constexpr size_t kStderrTailMax = 8192;

void AppendTail(std::string& tail, const char* data, size_t n) {
    tail.append(data, n);
    if (tail.size() > kStderrTailMax) tail.erase(0, tail.size() - kStderrTailMax);
}

#ifndef _WIN32

bool IsExecutableFile(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0
           && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

int CloexecPipe(int fds[2]) {
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) < 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

void SetNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

// fork+exec with stdin/stdout/stderr pipes, in its own process group so a
// cancel can signal the CLI and every tool subprocess it started. Loops on
// poll() so stdin is fed while stdout is drained (a large image payload
// would otherwise deadlock against a full stdout pipe).
ClaudeProcessResult RunProcess(ClaudeProcessImpl* impl, const ClaudeRunSpec& spec,
                               const OutFn& onOut, Clock::time_point deadline) {
    // Writing to a CLI that already exited must fail with EPIPE instead of
    // killing gritcode.
    static std::once_flag sigpipeOnce;
    std::call_once(sigpipeOnce, [] { signal(SIGPIPE, SIG_IGN); });

    ClaudeProcessResult res;
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, ep[2] = {-1, -1};
    auto closeFd = [](int& fd) { if (fd >= 0) { close(fd); fd = -1; } };
    auto closeAll = [&] {
        for (int* p : {in, out, err, ep}) { closeFd(p[0]); closeFd(p[1]); }
    };
    if (CloexecPipe(in) < 0 || CloexecPipe(out) < 0 || CloexecPipe(err) < 0
        || CloexecPipe(ep) < 0) {
        closeAll();
        res.error = "pipe failed";
        return res;
    }

    // Everything the child touches is prepared before fork: after fork only
    // async-signal-safe calls are allowed.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(spec.executable.c_str()));
    for (const auto& a : spec.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char* cwd = spec.cwd.empty() ? nullptr : spec.cwd.c_str();

    pid_t pid = fork();
    if (pid < 0) {
        closeAll();
        res.error = "fork failed";
        return res;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(err[1], STDERR_FILENO);
        if (cwd) (void)chdir(cwd);
        signal(SIGPIPE, SIG_DFL);  // SIG_IGN would be inherited across exec
        execv(argv[0], argv.data());
        int e = errno;
        (void)!write(ep[1], &e, sizeof e);
        _exit(127);
    }

    setpgid(pid, pid);
    closeFd(in[0]);
    closeFd(out[1]);
    closeFd(err[1]);
    closeFd(ep[1]);

    // The error pipe is close-on-exec: EOF means exec succeeded, 4 bytes
    // carry the errno of a failed exec.
    int execErr = 0;
    ssize_t en;
    do { en = read(ep[0], &execErr, sizeof execErr); } while (en < 0 && errno == EINTR);
    closeFd(ep[0]);
    if (en == (ssize_t)sizeof execErr) {
        waitpid(pid, nullptr, 0);
        closeAll();
        res.error = std::strerror(execErr);
        return res;
    }
    res.started = true;

    SetNonBlocking(in[1]);
    SetNonBlocking(out[0]);
    SetNonBlocking(err[0]);

    const std::string& payload = spec.stdinPayload;
    size_t written = 0;
    if (payload.empty()) closeFd(in[1]);

    bool outEof = false, errEof = false, exited = false;
    bool sentInt = false, sentTerm = false, sentKill = false;
    int status = 0;
    Clock::time_point cancelAt{}, exitedAt{};
    std::vector<char> buf(64 * 1024);

    for (;;) {
        const auto now = Clock::now();
        if (!exited) {
            bool hardKill = impl->killNow.load() || now >= deadline;
            if (hardKill && !sentKill) {
                kill(-pid, SIGKILL);
                sentKill = true;
            } else if (impl->cancelRequested.load()) {
                // SIGINT ends the turn the way Ctrl+C does, so Claude Code
                // records it and a later --resume continues cleanly.
                if (!sentInt) {
                    kill(pid, SIGINT);
                    sentInt = true;
                    cancelAt = now;
                } else if (!sentTerm && now - cancelAt > std::chrono::seconds(3)) {
                    kill(-pid, SIGTERM);
                    sentTerm = true;
                } else if (!sentKill && now - cancelAt > std::chrono::seconds(6)) {
                    kill(-pid, SIGKILL);
                    sentKill = true;
                }
            }
        }

        pollfd fds[3];
        int nf = 0, iIn = -1, iOut = -1, iErr = -1;
        if (in[1] >= 0) { iIn = nf; fds[nf++] = {in[1], POLLOUT, 0}; }
        if (!outEof)    { iOut = nf; fds[nf++] = {out[0], POLLIN, 0}; }
        if (!errEof)    { iErr = nf; fds[nf++] = {err[0], POLLIN, 0}; }
        if (nf > 0) {
            poll(fds, nf, 100);
        } else {
            struct timespec ts{0, 50'000'000};
            nanosleep(&ts, nullptr);
        }

        if (iIn >= 0 && (fds[iIn].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in[1], payload.data() + written, payload.size() - written);
            if (w > 0) {
                written += (size_t)w;
                if (written == payload.size()) closeFd(in[1]);
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                closeFd(in[1]);  // EPIPE: the CLI stopped reading
            }
        }
        auto drain = [&](int fd, bool& eof, bool isOut) {
            for (;;) {
                ssize_t r = read(fd, buf.data(), buf.size());
                if (r > 0) {
                    if (isOut) onOut(buf.data(), (size_t)r);
                    else AppendTail(res.stderrTail, buf.data(), (size_t)r);
                    if ((size_t)r < buf.size()) break;
                } else if (r == 0) {
                    eof = true;
                    break;
                } else {
                    if (errno != EAGAIN && errno != EINTR) eof = true;
                    break;
                }
            }
        };
        if (iOut >= 0 && (fds[iOut].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(out[0], outEof, true);
        if (iErr >= 0 && (fds[iErr].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(err[0], errEof, false);

        if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            exitedAt = now;
        }
        if (exited) {
            if (outEof && errEof) break;
            // A background process the CLI left behind can hold the pipes
            // open; stop reading once the CLI itself is gone for a while.
            if (now - exitedAt > std::chrono::seconds(2)) break;
        }
    }
    closeAll();

    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    res.cancelled = impl->cancelRequested.load() || impl->killNow.load();
    return res;
}

#else  // _WIN32

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Quote one argument for CreateProcess using the MSVC runtime's parsing rules
// (backslashes only escape when they precede a double quote).
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring q = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            q.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            q.append(backslashes * 2 + 1, L'\\');
        } else {
            q.append(backslashes, L'\\');
        }
        q.push_back(*it);
    }
    q.push_back(L'"');
    return q;
}

bool IsExecutableFile(const std::string& path) {
    DWORD a = GetFileAttributesW(Widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void TerminateJob(ClaudeProcessImpl* impl) {
    std::lock_guard<std::mutex> lk(impl->jobMu);
    if (impl->job) TerminateJobObject(impl->job, 1);
}

// CreateProcess with stdin/stdout/stderr pipes. The CLI runs in a job object
// so a cancel (or the frame closing) takes down every tool subprocess too.
// Windows has no SIGINT for a windowless child, so cancel terminates the job;
// Claude Code then continues the unfinished turn on the next --resume.
ClaudeProcessResult RunProcess(ClaudeProcessImpl* impl, const ClaudeRunSpec& spec,
                               const OutFn& onOut, Clock::time_point deadline) {
    ClaudeProcessResult res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    HANDLE errR = nullptr, errW = nullptr;
    auto closeH = [](HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } };
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)
        || !CreatePipe(&errR, &errW, &sa, 0)) {
        for (HANDLE* h : {&inR, &inW, &outR, &outW, &errR, &errW}) closeH(*h);
        res.error = "CreatePipe failed";
        return res;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // npm installs a .cmd shim, which only cmd.exe can run.
    std::wstring exe = Widen(spec.executable);
    std::wstring lower = exe;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    bool viaCmd = lower.size() > 4
                  && (lower.compare(lower.size() - 4, 4, L".cmd") == 0
                      || lower.compare(lower.size() - 4, 4, L".bat") == 0);
    std::wstring cmd = QuoteArg(exe);
    for (const auto& a : spec.args) cmd += L" " + QuoteArg(Widen(a));
    if (viaCmd) cmd = L"cmd.exe /d /s /c \"" + cmd + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    std::wstring wcwd = Widen(spec.cwd);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED
                                 | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                             &si, &pi);
    closeH(inR);
    closeH(outW);
    closeH(errW);
    if (!ok) {
        res.error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        closeH(inW);
        closeH(outR);
        closeH(errR);
        if (job) CloseHandle(job);
        return res;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    {
        std::lock_guard<std::mutex> lk(impl->jobMu);
        impl->job = job;
    }
    ResumeThread(pi.hThread);
    res.started = true;
    // Cancel() may have landed before the job was published.
    if (impl->cancelRequested.load() || impl->killNow.load()) TerminateJob(impl);

    std::thread writer([inW, &spec]() mutable {
        const std::string& p = spec.stdinPayload;
        size_t off = 0;
        while (off < p.size()) {
            DWORD chunk = (DWORD)std::min<size_t>(p.size() - off, 1 << 20);
            DWORD w = 0;
            if (!WriteFile(inW, p.data() + off, chunk, &w, nullptr) || w == 0) break;
            off += w;
        }
        CloseHandle(inW);
    });

    std::mutex errMu;
    std::thread errReader([&]() {
        char b[4096];
        DWORD n = 0;
        while (ReadFile(errR, b, sizeof(b), &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lk(errMu);
            AppendTail(res.stderrTail, b, n);
        }
    });

    // Once the CLI exits, give its output a moment to drain, then end any
    // background process still holding the pipes (Claude Code stops those
    // itself a few seconds after a turn anyway). Also enforces `deadline`.
    std::atomic<bool> readerDone{false};
    std::thread watcher([&]() {
        while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
            if (readerDone.load()) return;
            if (Clock::now() >= deadline) TerminateJob(impl);
        }
        for (int i = 0; i < 40 && !readerDone.load(); ++i) Sleep(50);
        if (!readerDone.load()) TerminateJob(impl);
    });

    std::vector<char> buf(64 * 1024);
    DWORD n = 0;
    while (ReadFile(outR, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
        onOut(buf.data(), n);
    readerDone.store(true);

    WaitForSingleObject(pi.hProcess, INFINITE);
    watcher.join();
    // Unblock the stderr reader and stdin writer if a leftover process still
    // holds the other pipe ends.
    TerminateJob(impl);
    CancelSynchronousIo(writer.native_handle());
    writer.join();
    errReader.join();

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = (int)code;
    res.cancelled = impl->cancelRequested.load() || impl->killNow.load();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    closeH(outR);
    closeH(errR);
    {
        std::lock_guard<std::mutex> lk(impl->jobMu);
        if (impl->job) CloseHandle(impl->job);
        impl->job = nullptr;
    }
    return res;
}

#endif  // _WIN32

std::vector<std::string> SplitPath(const char* path) {
    std::vector<std::string> dirs;
    if (!path) return dirs;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char* p = path;; ++p) {
        if (*p == sep || *p == '\0') {
            if (!cur.empty()) dirs.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return dirs;
}

}  // namespace

ClaudeAgentProcess::ClaudeAgentProcess() noexcept = default;

ClaudeAgentProcess::ClaudeAgentProcess(wxEvtHandler* target, ClaudeRunSpec spec,
                                       DataFn onData, DoneFn onDone)
    : impl_(std::make_unique<ClaudeProcessImpl>()) {
    impl_->cb = std::make_shared<ClaudeCbHolder>();
    impl_->cb->data = std::move(onData);
    impl_->cb->done = std::move(onDone);

    ClaudeProcessImpl* impl = impl_.get();
    impl_->worker = std::thread([impl, target, spec = std::move(spec)]() {
        auto cb = impl->cb;
        OutFn onOut = [target, cb](const char* data, size_t n) {
            if (!target || !cb->data) return;
            target->CallAfter([cb, chunk = std::string(data, n)]() {
                if (cb->data) cb->data(chunk);
            });
        };
        ClaudeProcessResult res =
            RunProcess(impl, spec, onOut, Clock::time_point::max());
        impl->active.store(false);
        if (target && cb->done) {
            target->CallAfter([cb, res = std::move(res)]() mutable {
                if (cb->done) cb->done(std::move(res));
            });
        }
    });
}

ClaudeAgentProcess::~ClaudeAgentProcess() {
    if (impl_) {
        impl_->killNow.store(true);
#ifdef _WIN32
        TerminateJob(impl_.get());
#endif
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

ClaudeAgentProcess::ClaudeAgentProcess(ClaudeAgentProcess&&) noexcept = default;

ClaudeAgentProcess& ClaudeAgentProcess::operator=(ClaudeAgentProcess&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->killNow.store(true);
#ifdef _WIN32
            TerminateJob(impl_.get());
#endif
            if (impl_->worker.joinable()) impl_->worker.join();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool ClaudeAgentProcess::IsActive() const noexcept {
    return impl_ && impl_->active.load();
}

void ClaudeAgentProcess::Cancel() noexcept {
    if (!impl_) return;
    impl_->cancelRequested.store(true);
#ifdef _WIN32
    TerminateJob(impl_.get());
#endif
}

std::string FindClaudeExecutable() {
    std::vector<std::string> candidates;
#ifdef _WIN32
    for (const auto& d : SplitPath(std::getenv("PATH"))) {
        candidates.push_back(d + "\\claude.exe");
    }
    if (const char* profile = std::getenv("USERPROFILE"))
        candidates.push_back(std::string(profile) + "\\.local\\bin\\claude.exe");
    for (const auto& d : SplitPath(std::getenv("PATH"))) {
        candidates.push_back(d + "\\claude.cmd");
    }
    if (const char* appdata = std::getenv("APPDATA"))
        candidates.push_back(std::string(appdata) + "\\npm\\claude.cmd");
#else
    for (const auto& d : SplitPath(std::getenv("PATH")))
        candidates.push_back(d + "/claude");
    // A GUI launch may not see the user's shell PATH even after
    // ImportShellEnv (e.g. a 5 s shell timeout), so also try the places the
    // installers put it.
    if (const char* home = std::getenv("HOME")) {
        std::string h = home;
        candidates.push_back(h + "/.local/bin/claude");
        candidates.push_back(h + "/.claude/local/claude");
        candidates.push_back(h + "/.npm-global/bin/claude");
        candidates.push_back(h + "/.bun/bin/claude");
    }
    candidates.push_back("/opt/homebrew/bin/claude");
    candidates.push_back("/usr/local/bin/claude");
    candidates.push_back("/usr/bin/claude");
#endif
    for (const auto& c : candidates) {
        if (IsExecutableFile(c)) return c;
    }
    return std::string();
}

std::string ClaudeVersion(const std::string& executable) {
    if (executable.empty()) return std::string();
    ClaudeProcessImpl impl;
    ClaudeRunSpec spec;
    spec.executable = executable;
    spec.args = {"--version"};
    std::string out;
    ClaudeProcessResult res = RunProcess(
        &impl, spec, [&out](const char* d, size_t n) { out.append(d, n); },
        Clock::now() + std::chrono::seconds(5));
    if (!res.started || res.exitCode != 0) return std::string();
    size_t nl = out.find_first_of("\r\n");
    if (nl != std::string::npos) out.resize(nl);
    return out;
}
