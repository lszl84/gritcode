#pragma once

#include <wx/event.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Runs Claude turns through the user's own, unmodified Claude Code CLI
// (`claude -p` with stream-json on stdin and stdout). Gritcode never touches
// Claude credentials: the CLI uses whatever sign-in the user set up in a
// terminal (`claude` then /login, or ANTHROPIC_API_KEY in their environment).

// How one run of the CLI ended.
struct ClaudeProcessResult {
    // False when the executable could not be launched at all; `error` says why.
    bool started = false;
    std::string error;
    // Exit status of the CLI (-1 when it was killed by a signal).
    int exitCode = -1;
    // True when Cancel() was called before the process ended.
    bool cancelled = false;
    // Last few KB of stderr, for error reporting.
    std::string stderrTail;
};

struct ClaudeRunSpec {
    std::string executable;          // absolute path (FindClaudeExecutable)
    std::vector<std::string> args;   // argv[1..]
    std::string cwd;                 // working directory for the session
    std::string stdinPayload;        // written in full, then stdin is closed
};

struct ClaudeProcessImpl;

// Async runner. Construction spawns the CLI and a worker thread that feeds its
// stdin and forwards raw stdout bytes; callbacks run on the target's event
// loop via CallAfter, like StreamingWebRequest.
//
// RAII: destruction (or move-assign onto a live run) kills the process tree
// and joins the worker before returning.
class ClaudeAgentProcess {
public:
    using DataFn = std::function<void(std::string_view)>;
    using DoneFn = std::function<void(ClaudeProcessResult)>;

    ClaudeAgentProcess() noexcept;
    ClaudeAgentProcess(wxEvtHandler* target, ClaudeRunSpec spec,
                       DataFn onData, DoneFn onDone);
    ~ClaudeAgentProcess();

    ClaudeAgentProcess(const ClaudeAgentProcess&) = delete;
    ClaudeAgentProcess& operator=(const ClaudeAgentProcess&) = delete;
    ClaudeAgentProcess(ClaudeAgentProcess&& other) noexcept;
    ClaudeAgentProcess& operator=(ClaudeAgentProcess&& other) noexcept;

    // True from construction until the worker has finished.
    bool IsActive() const noexcept;

    // Ends the turn gracefully (SIGINT, so Claude Code records it), then
    // escalates to SIGTERM and SIGKILL if the CLI doesn't exit. Idempotent,
    // safe from any thread.
    void Cancel() noexcept;

private:
    std::unique_ptr<ClaudeProcessImpl> impl_;
};

// Locates the `claude` executable: PATH first, then the usual install
// locations (~/.local/bin, ~/.claude/local, Homebrew, npm global). Returns an
// absolute path, or empty when Claude Code isn't installed.
std::string FindClaudeExecutable();

// First line of `claude --version`, or empty on failure. Blocks for up to a
// few seconds; call off the GUI thread or only from user actions.
std::string ClaudeVersion(const std::string& executable);
