// FastCode Native — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "app.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <array>
#include <algorithm>
#include <filesystem>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <chrono>
#include "keysyms.h"

// pipe2(O_CLOEXEC) is Linux-only; on macOS fall back to pipe + fcntl.
static int CloexecPipe(int pfd[2]) {
#ifdef __linux__
    return pipe2(pfd, O_CLOEXEC);
#else
    if (pipe(pfd) < 0) return -1;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

// Resolve the user's login-shell environment in a background thread and
// apply the interesting vars to our own process env. Why: on macOS, GUI
// launches (Finder/Dock/open) inherit launchd's minimal env — no
// ~/.local/bin, no /opt/homebrew/bin, no nvm — so `claude`, `ffmpeg` and
// anything the agent tries to run can't be found. Linux GUI launches via
// .desktop miss rc-file PATH edits (nvm, pyenv, cargo, linuxbrew) for the
// same reason.
//
// Approach: same trick VS Code and t3code use — spawn `$SHELL -ilc` with a
// tiny printf that wraps the vars we care about in markers so rc-file
// noise (banners, MOTD) can be parsed around. Runs in a detached thread
// at Init, so window creation isn't blocked; setenv() happens on the main
// thread via the event queue to stay clear of the usual setenv/fork race
// warnings in libc.
static void ResolveShellEnvAsync(EventQueue& events) {
    std::thread([&events]() {
        const char* shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/zsh";

        // Vars worth importing. PATH is the headline; the rest help agent
        // tool calls and subprocess behavior.
        static const char* kVars[] = {
            "PATH", "SSH_AUTH_SOCK", "HOMEBREW_PREFIX", "MANPATH",
            "LANG", "LC_ALL", "NVM_DIR", "PYENV_ROOT", "RBENV_ROOT",
        };
        const int NVARS = (int)(sizeof(kVars) / sizeof(kVars[0]));

        // printf '__FCNENV_START__\n%s\n%s\n...\n__FCNENV_END__\n' "$PATH" "$SSH_AUTH_SOCK" ...
        std::string cmd = "printf '__FCNENV_START__\\n";
        for (int i = 0; i < NVARS; i++) cmd += "%s\\n";
        cmd += "__FCNENV_END__\\n'";
        for (int i = 0; i < NVARS; i++) {
            cmd += " \"$";
            cmd += kVars[i];
            cmd += "\"";
        }

        int pfd[2];
        if (CloexecPipe(pfd) < 0) return;

        pid_t pid = fork();
        if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }

        if (pid == 0) {
            setsid();
            dup2(pfd[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
            // Original pfd fds are CLOEXEC so they auto-close on exec.
            // Hint rc-files to skip heavy work (some users check this).
            setenv("FCN_RESOLVING_ENVIRONMENT", "1", 1);
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
            int ret = poll(&rpfd, 1, (int)std::min(remaining, (decltype(remaining))500));
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

        // Locate the last START/END pair — rc-files sometimes echo during
        // sourcing, and we want the final clean block.
        size_t start = output.rfind("__FCNENV_START__\n");
        size_t end = output.rfind("\n__FCNENV_END__");
        if (start == std::string::npos || end == std::string::npos || end <= start) {
            fprintf(stderr, "shell-env: no markers in %zu bytes of output\n", output.size());
            return;
        }
        start += strlen("__FCNENV_START__\n");
        std::string body = output.substr(start, end - start);

        std::vector<std::string> values;
        size_t pos = 0;
        while (pos <= body.size()) {
            size_t nl = body.find('\n', pos);
            if (nl == std::string::npos) { values.push_back(body.substr(pos)); break; }
            values.push_back(body.substr(pos, nl - pos));
            pos = nl + 1;
        }
        if ((int)values.size() < NVARS) {
            fprintf(stderr, "shell-env: expected %d vars, got %zu\n", NVARS, values.size());
            return;
        }

        std::vector<std::pair<std::string, std::string>> kvs;
        for (int i = 0; i < NVARS; i++) {
            if (!values[i].empty()) kvs.emplace_back(kVars[i], values[i]);
        }
        events.Push([kvs]() {
            for (auto& kv : kvs) setenv(kv.first.c_str(), kv.second.c_str(), 1);
            fprintf(stderr, "shell-env: applied %zu vars from login shell\n", kvs.size());
        });
    }).detach();
}

// Tool execution — uses O_CLOEXEC pipe so backgrounded processes (php -S ... &)
// don't inherit the pipe fd and block reads forever.  120s timeout via alarm().
static std::string RunCommand(const std::string& cmd) {
    int pfd[2];
    if (CloexecPipe(pfd) < 0) return "Error: pipe failed";

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return "Error: fork failed"; }

    if (pid == 0) {
        // Child: new session so we can kill the group on timeout
        setsid();
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    // Parent: read output with 120s timeout
    close(pfd[1]);

    std::string result;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);

    struct pollfd rpfd = {pfd[0], POLLIN, 0};
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            result += "\n[killed: command timed out after 120s]";
            kill(-pid, SIGKILL);  // Kill entire process group
            break;
        }
        int ret = poll(&rpfd, 1, (int)std::min(remaining, (decltype(remaining))1000));
        if (ret < 0) break;
        if (ret == 0) continue;
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, n);
        if (result.size() > 32768) { result += "\n...(truncated)"; break; }
    }

    close(pfd[0]);
    // Reap child (and kill if still running)
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        result += "\n[exit code: " + std::to_string(WEXITSTATUS(status)) + "]";
    return result;
}

static std::string ExpandTilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return home + path.substr(1);
    return path;
}

static std::string WriteFile(const std::string& path, const std::string& content) {
    std::string expanded = ExpandTilde(path);
    // Create parent directories if needed
    auto parent = std::filesystem::path(expanded).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream f(expanded);
    if (!f) return "Error: cannot open " + expanded + " for writing";
    f << content;
    f.close();
    return "Wrote " + std::to_string(content.size()) + " bytes to " + expanded;
}

static std::string EditFile(const std::string& path, const std::string& oldStr, const std::string& newStr) {
    std::string expanded = ExpandTilde(path);
    std::ifstream in(expanded);
    if (!in) return "Error: cannot read " + expanded;
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = contents.find(oldStr);
    if (pos == std::string::npos) return "Error: old_string not found in " + expanded;
    // Check uniqueness
    if (contents.find(oldStr, pos + 1) != std::string::npos)
        return "Error: old_string is not unique in " + expanded + " — provide more context";
    contents.replace(pos, oldStr.size(), newStr);

    std::ofstream out(expanded);
    if (!out) return "Error: cannot write " + expanded;
    out << contents;
    out.close();
    return "Applied edit to " + expanded;
}

static std::string ExecuteTool(const std::string& name, const std::string& argsJson) {
    try {
        auto args = json::parse(argsJson);
        if (name == "bash") return RunCommand(args.value("command", ""));
        if (name == "read_file") return RunCommand("cat -n -- '" + ExpandTilde(args.value("path", "")) + "'");
        if (name == "list_directory") return RunCommand("ls -la -- '" + ExpandTilde(args.value("path", ".")) + "'");
        if (name == "write_file") return WriteFile(args.value("path", ""), args.value("content", ""));
        if (name == "edit_file") return EditFile(args.value("path", ""), args.value("old_string", ""), args.value("new_string", ""));
    } catch (...) {}
    return "Error: unknown tool " + name;
}

static std::string StripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] >= 0x20 && s[i] <= 0x3F) ++i;
            continue;
        }
        if (s[i] == '\033') { ++i; continue; }
        out += s[i];
    }
    return out;
}

// Tool definitions JSON
static std::string ToolDefsJson() {
    json tools = json::array();
    tools.push_back({{"type","function"},{"function",{
        {"name","bash"},{"description","Execute a shell command and return stdout+stderr. Use for running tests, git, installing packages, etc."},
        {"parameters",{{"type","object"},{"properties",{{"command",{{"type","string"},{"description","The command to execute"}}}}},{"required",json::array({"command"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","read_file"},{"description","Read a file's contents with line numbers"},
        {"parameters",{{"type","object"},{"properties",{{"path",{{"type","string"},{"description","Absolute or relative file path"}}}}},{"required",json::array({"path"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","write_file"},{"description","Create or overwrite a file with the given content. Creates parent directories as needed."},
        {"parameters",{{"type","object"},{"properties",{
            {"path",{{"type","string"},{"description","File path to write"}}},
            {"content",{{"type","string"},{"description","Full file content to write"}}}
        }},{"required",json::array({"path","content"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","edit_file"},{"description","Replace a unique string in a file. The old_string must appear exactly once. Include enough surrounding context to make it unique."},
        {"parameters",{{"type","object"},{"properties",{
            {"path",{{"type","string"},{"description","File path to edit"}}},
            {"old_string",{{"type","string"},{"description","Exact text to find (must be unique in file)"}}},
            {"new_string",{{"type","string"},{"description","Replacement text"}}}
        }},{"required",json::array({"path","old_string","new_string"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","list_directory"},{"description","List directory contents with details"},
        {"parameters",{{"type","object"},{"properties",{{"path",{{"type","string"},{"description","Directory path (default: current directory)"}}}}},{"required",json::array()}}}
    }}});
    return tools.dump();
}

// ============================================================================
// Init
// ============================================================================

bool App::Init(bool sessionChooser) {
    chooserMode_ = sessionChooser;
    if (!window_.Init(1000, 750, "FastCode Native")) return false;

    if (!scrollView_.Init(window_.Width(), window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * window_.Scale()),
                          window_.Scale()))
        return false;

    scrollView_.SetAutoScroll(true);
    scrollView_.SetClipboardFunc([&](const std::string& t) {
#ifdef FCN_LINUX
        FILE* p = popen("wl-copy 2>/dev/null", "w");
#else
        FILE* p = popen("pbcopy 2>/dev/null", "w");
#endif
        if (p) { fwrite(t.c_str(), 1, t.size(), p); pclose(p); }
    });

    if (!renderer_.Init()) {
        fprintf(stderr, "GL renderer init failed\n");
        return false;
    }

    // Workspace dropdown
    workspaceDropdown_.onSelect = [&](int i, const std::string& id) { OnWorkspaceChanged(i, id); };

    // Provider dropdown
    providerDropdown_.items = {{"zen", "OpenCode Zen"}, {"claude", "Claude (ACP)"}};
    providerDropdown_.selectedIndex = 0;
    providerDropdown_.onSelect = [&](int i, const std::string& id) { OnProviderChanged(i, id); };

    // Model dropdown (populated after connect)
    modelDropdown_.items = {};
    modelDropdown_.onSelect = [&](int i, const std::string& id) { OnModelChanged(i, id); };

    // Send button
    sendButton_.text = "Send";
    sendButton_.onClick = [&]() { SendMessage(); };

    // API Key button + inline editor
    apiKeyButton_.text = "API Key...";
    apiKeyButton_.onClick = [&]() { ShowApiKeyDialog(); };

    apiKeyInput_.placeholder = "Paste API key...";
    apiKeyInput_.onSubmit = [&](const std::string&) {
        if (!apiKeyInput_.text.empty()) {
            keychain::SaveApiKey(apiKeyInput_.text);
            Connect(apiKeyInput_.text);
        }
        apiKeyInput_.Clear();
        apiKeyEditing_ = false;
        LayoutWidgets();
        MarkDirty();
    };

    apiKeyAccept_.text = "OK";
    apiKeyAccept_.onClick = [&]() {
        if (apiKeyInput_.onSubmit) apiKeyInput_.onSubmit(apiKeyInput_.text);
    };

    apiKeyCancel_.text = "X";
    apiKeyCancel_.onClick = [&]() {
        apiKeyInput_.Clear();
        apiKeyEditing_ = false;
        LayoutWidgets();
        MarkDirty();
    };

    // Clipboard paste helper (GLFW can't read cross-app clipboard on Wayland)
    auto pasteFromClipboard = [&]() -> std::string {
#ifdef FCN_LINUX
        FILE* p = popen("wl-paste --no-newline 2>/dev/null", "r");
#else
        FILE* p = popen("pbpaste 2>/dev/null", "r");
#endif
        if (!p) return "";
        std::string result;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) result += buf;
        pclose(p);
        return result;
    };

    apiKeyInput_.onPaste = pasteFromClipboard;

    // Message input
    messageInput_.placeholder = "Type a message...";
    messageInput_.onSubmit = [&](const std::string&) { SendMessage(); };
    messageInput_.onPaste = pasteFromClipboard;

    statusLabel_.text = "Disconnected";
    versionLabel_.text = "v" FCN_VERSION;
    versionLabel_.color = {0.35f, 0.35f, 0.37f};

    // Window callbacks
    float currentScale = window_.Scale();
    window_.OnResize([&, currentScale](int w, int h, float scale) mutable {
        int viewH = h - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * scale);
        if (viewH < 1) viewH = 1;
        if (scale != currentScale) {
            currentScale = scale;
            scrollView_.Init(w, viewH, scale);
        } else {
            scrollView_.OnResize(w, viewH);
        }
        LayoutWidgets();
        MarkDirty();
    });
    window_.OnMouseButton([&](float x, float y, bool pressed, bool shift) {
        if (pressed) OnMouseDown(x, y, shift);
        else OnMouseUp(x, y);
    });
    window_.OnMouseMove([&](float x, float y, bool left) { OnMouseMove(x, y, left); });
    window_.OnScrollEvent([&](float delta) { OnScroll(delta); });
    window_.OnKeyEvent([&](int key, int mods, bool pressed) {
        if (pressed) OnKey(key, mods, pressed);
    });
    window_.OnCharEvent([&](uint32_t cp) {
        OnChar(cp);
    });

    LayoutWidgets();

    // Kick off async shell env resolution — PATH and friends from the
    // user's login shell, needed so GUI-launched fcn can find claude,
    // ffmpeg, etc. Runs in a detached thread; result is applied on the
    // main thread via events_ before any user-triggered subprocess is
    // likely to run.
    ResolveShellEnvAsync(events_);

    window_.Show();
    messageInput_.focused = true;

    if (chooserMode_) {
        chooserSessions_ = SessionManager::ListSessions();
        std::sort(chooserSessions_.begin(), chooserSessions_.end(),
                  [](const SessionInfo& a, const SessionInfo& b) { return a.lastUsed > b.lastUsed; });
        chooserHovered_ = chooserSessions_.empty() ? 0 : 0;  // Pre-select first item
    } else {
        // Session: load for current working directory (deferred to main loop)
        char cwdBuf[4096];
        std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
        session_.SetCwd(cwd);
        session_.LoadForCwd(cwd);
        PopulateWorkspaceDropdown();
        RestoreSessionToView();
        StartConnect();
    }

    StartMCP();
    return true;
}

void App::RestoreSessionToView() {
    if (!session_.History().empty()) {
        events_.Push([this]() {
            MarkdownRenderer mdRenderer(14);
            scrollView_.BeginBatch();
            for (auto& m : session_.History()) {
                if (m.role == "user")
                    scrollView_.AppendStream(BlockType::USER_PROMPT, m.content);
                else if (m.role == "assistant" && !m.content.empty())
                    scrollView_.AddBlocks(mdRenderer.Render(m.content, true));
            }
            scrollView_.EndBatch();
            AppendSystem("Session restored (" + std::to_string(session_.History().size()) + " messages)");

            // Sync UI state to the restored provider. Without this, dropdowns
            // stay on the Init() defaults while activeProvider_/activeModel_
            // point somewhere else — every send goes to the wrong backend.
            std::string savedProvider = session_.Provider();
            std::string savedModel = session_.Model();
            if (!savedProvider.empty() &&
                (savedProvider == "zen" || savedProvider == "claude")) {
                // Select in dropdown so the UI reflects reality.
                for (size_t i = 0; i < providerDropdown_.items.size(); i++) {
                    if (providerDropdown_.items[i].id == savedProvider) {
                        providerDropdown_.selectedIndex = (int)i;
                        break;
                    }
                }
                // Remember the restored model so the async zen model-fetch
                // (or the claude dropdown install) doesn't clobber it.
                restoredModelPref_ = savedModel;
                OnProviderChanged(0, savedProvider);
            } else if (!savedModel.empty()) {
                activeModel_ = savedModel;
            }
            MarkDirty();
        });
    } else {
        AppendSystem("Starting...");
    }
}

void App::StartConnect() {
    std::thread([this]() {
        std::string savedKey = keychain::LoadApiKey();
        events_.Push([this, savedKey]() {
            if (!savedKey.empty()) {
                AppendSystem("Connecting with saved API key...");
                Connect(savedKey);
            } else {
                AppendSystem("Connecting anonymously...");
                Connect("");
            }
        });
    }).detach();
}

void App::StartMCP() {
    MCPCallbacks cb;

    cb.sendMessage = [this](const std::string& msg) {
        events_.Push([this, msg]() {
            messageInput_.text = msg;
            SendMessage();
        });
    };

    cb.getStatus = [this]() -> json {
        return {
            {"connected", connected_},
            {"requestInProgress", requestInProgress_},
            {"provider", activeProvider_},
            {"model", activeModel_},
            {"toolRound", toolRound_},
            {"messageCount", (int)session_.History().size()}
        };
    };

    cb.getConversation = [this]() -> json {
        json msgs = json::array();
        for (auto& m : session_.History()) {
            json entry = {{"role", m.role}, {"content", m.content}};
            if (!m.toolCalls.empty()) {
                json tcs = json::array();
                for (auto& tc : m.toolCalls)
                    tcs.push_back({{"name", tc.value("name", "")}, {"arguments", tc.value("arguments", "")}});
                entry["tool_calls"] = tcs;
            }
            if (!m.toolCallId.empty()) entry["tool_call_id"] = m.toolCallId;
            msgs.push_back(entry);
        }
        return msgs;
    };

    cb.getLastAssistant = [this]() -> json {
        for (int i = (int)session_.History().size() - 1; i >= 0; i--) {
            auto& m = session_.History()[i];
            if (m.role == "assistant" && !m.content.empty())
                return {{"text", m.content}, {"index", i}};
        }
        return {{"text", ""}, {"index", -1}};
    };

    cb.setProvider = [this](const std::string& provider, const std::string& model) {
        events_.Push([this, provider, model]() {
            OnProviderChanged(0, provider);
            if (!model.empty()) {
                activeModel_ = model;
                for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
                    if (modelDropdown_.items[i].id == model) {
                        modelDropdown_.selectedIndex = (int)i;
                        break;
                    }
                }
            }
            MarkDirty();
        });
    };

    mcpServer_.Start(std::move(cb));
}

// ============================================================================
// Layout
// ============================================================================

void App::LayoutWidgets() {
    // All coordinates in physical pixels (matching WaylandWindow and ScrollView)
    float w = window_.Width();
    float s = window_.Scale();
    // Leave a small gutter below the bottom bar so the rounded controls
    // don't kiss the window edge.
    float bottomMargin = 2 * s;
    float h = window_.Height() - bottomMargin;
    float bar = barHeight_ * s;
    float inp = inputHeight_ * s;

    float barY = h - bar;
    float inputY = barY - inp;

    messageInput_.bounds = {8, inputY + 5, w - 103, inp - 10};
    sendButton_.bounds = {w - 88, inputY + 5, 80, inp - 10};

    float bx = 8;
    workspaceDropdown_.bounds = {bx, barY + 5, 200 * s, bar - 10}; bx += 205 * s;
    providerDropdown_.bounds = {bx, barY + 5, 140 * s, bar - 10}; bx += 145 * s;
    modelDropdown_.bounds = {bx, barY + 5, 150 * s, bar - 10}; bx += 155 * s;
    statusLabel_.bounds = {bx, barY + 5, 180 * s, bar - 10}; bx += 185 * s;

    // Right-edge of the bottom bar matches sendButton right edge above (w - 8).
    float rightEdge = w - 8;
    bool showApiKey = (activeProvider_ == "zen");
    if (showApiKey && apiKeyEditing_) {
        apiKeyButton_.visible = false;
        float btnW = 40 * s;
        float inputW = 220 * s;
        float totalW = inputW + btnW * 2 + 8;
        float startX = rightEdge - totalW;
        apiKeyInput_.bounds = {startX, barY + 5, inputW, bar - 10};
        apiKeyAccept_.bounds = {startX + inputW + 4, barY + 5, btnW, bar - 10};
        apiKeyCancel_.bounds = {startX + inputW + btnW + 8, barY + 5, btnW, bar - 10};
        versionLabel_.bounds = {startX - 80 * s, barY + 5, 70 * s, bar - 10};
    } else {
        float apiW = 100 * s;
        apiKeyButton_.bounds = {rightEdge - apiW, barY + 5, apiW, bar - 10};
        apiKeyButton_.visible = showApiKey;
        versionLabel_.bounds = {rightEdge - apiW - 80 * s, barY + 5, 70 * s, bar - 10};
    }
}

// ============================================================================
// Paint
// ============================================================================

void App::PaintBottomBar() {
    float s = window_.Scale();
    float w = window_.Width();
    float h = window_.Height();
    auto& fm = scrollView_.Fonts();

    // Single unified dark chrome behind both the message row and the
    // dropdown bar, extending a few pixels above the message row so
    // there's breathing space between the scroll content and the input.
    Color chromeBg{0.10f, 0.10f, 0.11f};

    float bar = barHeight_ * s;
    float inp = inputHeight_ * s;
    float barY = h - bar;
    float inputY = barY - inp;
    float topPad = chromeTopPad_ * s;

    renderer_.DrawRect(0, inputY - topPad, w, inp + bar + topPad, chromeBg);

    // Scale transform: widgets use logical coords, renderer uses physical
    // For now, widgets draw at 1:1 since renderer works in physical pixels
    // and fonts are already scaled. Just offset coordinates.
    // TODO: proper scaling
    workspaceDropdown_.Paint(renderer_, fm);
    providerDropdown_.Paint(renderer_, fm);
    modelDropdown_.Paint(renderer_, fm);
    statusLabel_.Paint(renderer_, fm);
    versionLabel_.Paint(renderer_, fm);
    if (apiKeyEditing_) {
        apiKeyInput_.Paint(renderer_, fm, 0);
        apiKeyAccept_.Paint(renderer_, fm);
        apiKeyCancel_.Paint(renderer_, fm);
    } else {
        apiKeyButton_.Paint(renderer_, fm);
    }
    messageInput_.Paint(renderer_, fm, 0);
    sendButton_.Paint(renderer_, fm);

    // Dropdown popups last (z-order: on top of everything)
    workspaceDropdown_.PaintPopup(renderer_, fm);
    providerDropdown_.PaintPopup(renderer_, fm);
    modelDropdown_.PaintPopup(renderer_, fm);
}

// ============================================================================
// Connection & Chat
// ============================================================================

void App::Connect(const std::string& apiKey) {
    httpClient_.SetBaseUrl("https://opencode.ai/zen/v1");
    httpClient_.SetApiKey(apiKey);
    connected_ = true;
    statusLabel_.text = apiKey.empty() ? "Zen (Anonymous)" : "Validating key...";
    MarkDirty();

    httpClient_.FetchModels([this, apiKey](std::vector<net::ModelInfo> models, int httpStatus) {
        // If we have an API key, validate it against an authenticated endpoint
        int authStatus = 0;
        if (!apiKey.empty()) {
            std::string testModel = models.empty() ? "kimi-k2.5" : models[0].id;
            authStatus = httpClient_.ValidateKey(testModel);
            if (authStatus == 401) {
                events_.Push([this, apiKey]() {
                    AppendSystem("Invalid API key (" + apiKey + "). Please set a new key.");
                    keychain::ClearApiKey();
                    httpClient_.SetApiKey("");
                    statusLabel_.text = "Invalid key";
                    connected_ = false;
                    MarkDirty();
                });
                return;
            }
        }
        events_.Push([this, models, httpStatus]() { OnModelsReceived(models, httpStatus); });
    });
}

void App::OnModelsReceived(std::vector<net::ModelInfo> models, int httpStatus) {
    if (httpStatus == 401) {
        AppendSystem("Invalid API key. Please check your key and try again.");
        statusLabel_.text = "Unauthorized";
        keychain::ClearApiKey();
        httpClient_.SetApiKey("");
        connected_ = false;
        MarkDirty();
        return;
    }
    if (httpStatus == 0) {
        AppendSystem("Connection failed. Check your network.");
        statusLabel_.text = "Disconnected";
        connected_ = false;
        MarkDirty();
        return;
    }

    modelDropdown_.items.clear();
    int defaultIdx = 0;

    for (size_t i = 0; i < models.size(); i++) {
        bool isFree = models[i].allowAnonymous ||
                      models[i].id.find("free") != std::string::npos ||
                      models[i].id.find("big-pickle") != std::string::npos;
        if (httpClient_.IsAnonymous() && !isFree) continue;
        modelDropdown_.items.push_back({models[i].id, models[i].name});
        if (models[i].id == "kimi-k2.5") defaultIdx = modelDropdown_.items.size() - 1;
    }

    if (modelDropdown_.items.empty()) {
        modelDropdown_.items = {
            {"big-pickle", "Big Pickle"},
            {"kimi-k2.5", "Kimi K2.5"},
        };
    }

    // Prefer a restored session model if it's in the fetched list.
    if (!restoredModelPref_.empty()) {
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id == restoredModelPref_) {
                defaultIdx = (int)i;
                break;
            }
        }
        restoredModelPref_.clear();
    }

    modelDropdown_.selectedIndex = defaultIdx;
    activeModel_ = modelDropdown_.SelectedId();
    // Success: move status label off "Validating key..." — without this it
    // sticks forever because Connect() set it and OnModelsReceived never did.
    statusLabel_.text = httpClient_.IsAnonymous() ? "Zen (Anonymous)" : "Zen";
    AppendSystem("Models loaded. Active: " + activeModel_);
    MarkDirty();
}

void App::PopulateWorkspaceDropdown() {
    auto sessions = SessionManager::ListSessions();
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionInfo& a, const SessionInfo& b) { return a.lastUsed > b.lastUsed; });

    workspaceDropdown_.items.clear();
    std::string currentCwd = session_.SessionId().empty() ? "" : session_.SessionId();
    int sel = 0;

    char cwdBuf[4096];
    std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";

    for (int i = 0; i < (int)sessions.size(); i++) {
        auto& si = sessions[i];
        // Shorten path: ~/Developer/fastcode-native → .../Developer/fastcode-native
        std::string label = si.cwd;
        const char* home = getenv("HOME");
        if (home && label.find(home) == 0)
            label = "~" + label.substr(strlen(home));
        // Keep last two path components if still long
        if (label.size() > 30) {
            size_t last = label.rfind('/');
            if (last != std::string::npos && last > 0) {
                size_t prev = label.rfind('/', last - 1);
                if (prev != std::string::npos)
                    label = "..." + label.substr(prev);
            }
        }
        workspaceDropdown_.items.push_back({si.cwd, label});
        if (si.cwd == cwd) sel = i;
    }
    workspaceDropdown_.selectedIndex = sel;
}

void App::OnWorkspaceChanged(int idx, const std::string& id) {
    // Save current session first
    session_.Save();

    // Switch to new workspace
    if (!std::filesystem::is_directory(id)) {
        AppendSystem("Directory not found: " + id);
        return;
    }
    chdir(id.c_str());

    // Clear current conversation
    scrollView_.Clear();

    session_.SetCwd(id);
    session_.LoadForCwd(id);
    window_.SetTitle(("FCN — " + id).c_str());
    RestoreSessionToView();

    // Re-connect with current provider
    StartConnect();
    MarkDirty();
}

void App::OnProviderChanged(int, const std::string& id) {
    activeProvider_ = id;
    apiKeyButton_.visible = (id == "zen");
    AppendSystem("Switched to " + id);
    LayoutWidgets();

    if (id == "zen") {
        std::string key = keychain::LoadApiKey();
        Connect(key);
    } else {
        statusLabel_.text = "Claude (ACP)";
        connected_ = true;
        modelDropdown_.items = {
            {"claude-opus-4-6", "Claude Opus 4.6"},
            {"claude-sonnet-4-6", "Claude Sonnet 4.6"},
            {"claude-haiku-4-5", "Claude Haiku 4.5"},
        };
        int sel = 1;  // default sonnet
        if (!restoredModelPref_.empty()) {
            for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
                if (modelDropdown_.items[i].id == restoredModelPref_) { sel = (int)i; break; }
            }
            restoredModelPref_.clear();
        }
        modelDropdown_.selectedIndex = sel;
        activeModel_ = modelDropdown_.items[sel].id;
    }
    MarkDirty();
}

void App::OnModelChanged(int, const std::string& id) {
    activeModel_ = id;
    MarkDirty();
}

void App::ShowApiKeyDialog() {
    apiKeyEditing_ = true;
    apiKeyInput_.Clear();
    apiKeyInput_.focused = true;
    messageInput_.focused = false;
    LayoutWidgets();
    MarkDirty();
}

void App::AppendSystem(const std::string& text) {
    scrollView_.AppendStream(BlockType::THINKING, text);
    MarkDirty();
}

// ============================================================================
// Sending messages
// ============================================================================

std::string App::BuildRequestJson() {
    json j;
    j["model"] = activeModel_;
    j["stream"] = true;
    j["max_tokens"] = 32000;

    // Prune old tool results to prevent context overflow.
    // Same approach as opencode: walk backwards, protect ~40K tokens (~120K chars)
    // of recent tool output, then replace older results with a placeholder.
    auto& hist = session_.History();
    int histSize = (int)hist.size();

    const size_t PROTECT_CHARS = 120000;  // ~40K tokens worth of tool output to keep
    size_t toolCharsAccum = 0;
    int pruneBeforeIdx = -1;  // tool results at indices <= this get cleared
    for (int i = histSize - 1; i >= 0; i--) {
        if (hist[i].role == "tool") {
            toolCharsAccum += hist[i].content.size();
            if (toolCharsAccum > PROTECT_CHARS) {
                pruneBeforeIdx = i;
                break;
            }
        }
    }

    json msgs = json::array();

    // System prompt — tells the model what it is and how to behave
    {
        char cwdBuf[4096];
        std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
        std::string sysPrompt =
            "You are an AI coding assistant. You help the user with software engineering tasks "
            "by reading, writing, and editing files, and running shell commands.\n\n"
            "Working directory: " + cwd + "\n\n"
            "## Tools\n"
            "You have these tools: bash, read_file, write_file, edit_file, list_directory.\n"
            "- Use write_file to create new files or overwrite existing ones.\n"
            "- Use edit_file to make targeted changes — provide enough context in old_string to be unique.\n"
            "- Use read_file to read files before editing them.\n"
            "- Use bash to run commands, tests, install packages, etc.\n\n"
            "## Behavior\n"
            "- When given a task, work through it step by step using your tools.\n"
            "- Do not just describe what you would do — actually do it by calling tools.\n"
            "- After each tool result, assess whether the task is complete and continue if not.\n"
            "- When making changes, verify they work (e.g., run tests or the build).\n"
            "- Keep responses concise. Lead with actions, not explanations.\n";
        msgs.push_back({{"role", "system"}, {"content", sysPrompt}});
    }

    for (int i = 0; i < histSize; i++) {
        auto& m = hist[i];
        json msg;
        msg["role"] = m.role;
        if (m.role == "tool") {
            msg["tool_call_id"] = m.toolCallId;
            if (i <= pruneBeforeIdx) {
                msg["content"] = "[Old tool result content cleared]";
            } else {
                msg["content"] = m.content;
            }
        } else if (m.role == "assistant" && !m.toolCalls.empty()) {
            msg["content"] = m.content.empty() ? json(nullptr) : json(m.content);
            json tcs = json::array();
            for (auto& tc : m.toolCalls) {
                tcs.push_back({{"id", tc["id"]}, {"type", "function"},
                               {"function", {{"name", tc["name"]}, {"arguments", tc["arguments"]}}}});
            }
            msg["tool_calls"] = tcs;
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(msg);
    }
    j["messages"] = msgs;

    // Tools
    j["tools"] = json::parse(ToolDefsJson());
    j["tool_choice"] = "auto";

    return j.dump();
}

void App::SendMessage() {
    std::string msg = messageInput_.text;
    if (msg.empty() || !connected_ || requestInProgress_) return;

    // Show in UI
    scrollView_.AppendStream(BlockType::USER_PROMPT, msg);
    messageInput_.Clear();

    session_.History().push_back({"user", msg, {}, {}});
    requestInProgress_ = true;
    sendButton_.enabled = false;
    toolRound_ = 0;
    requestStartTime_ = GetMonotonicTime();
    waitingForResponse_ = true;
    waitingDotAnim_ = 0;
    MarkDirty();

    DoSendToProvider();
}

void App::DoSendToProvider() {
    if (activeProvider_ == "claude") {
        // Claude ACP: spawn `claude --print` with stream-json output.
        //
        // popen() is unsafe here: the read pipe fd gets inherited by every
        // descendant claude spawns, so if a tool backgrounds a process
        // (php -S ... &, dev servers, etc.) the pipe's write end stays open
        // forever and fgets blocks — UI stuck in "thinking" with no way to
        // send more messages. Same class of bug as RunCommand had before
        // commit 17f0bec fixed it.
        //
        // Fix: pipe2(O_CLOEXEC) + fork + exec claude directly in a new
        // session, write prompt via a second pipe to its stdin, read stdout
        // with a buffered line reader that correctly handles lines longer
        // than any single read() chunk. Track pid so Escape can kill the
        // whole process group. A per-request generation counter prevents a
        // stale finalizer from clobbering state of a newer request.
        std::string prompt;
        if (session_.History().size() > 1) {
            prompt = "<conversation_history>\n";
            for (size_t i = 0; i < session_.History().size() - 1; i++) {
                prompt += "<" + session_.History()[i].role + ">\n" + session_.History()[i].content + "\n</" + session_.History()[i].role + ">\n";
            }
            prompt += "</conversation_history>\n\n";
        }
        if (!session_.History().empty() && session_.History().back().role == "user")
            prompt += session_.History().back().content;

        uint64_t gen = ++requestGen_;
        std::string model = activeModel_;

        std::thread([this, prompt, model, gen]() {
            int inPipe[2];   // parent -> child stdin
            int outPipe[2];  // child stdout -> parent
            if (CloexecPipe(inPipe) < 0) {
                events_.Push([this, gen]() {
                    if (gen != requestGen_.load()) return;
                    AppendSystem("Error: pipe failed");
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    MarkDirty();
                });
                return;
            }
            if (CloexecPipe(outPipe) < 0) {
                close(inPipe[0]); close(inPipe[1]);
                events_.Push([this, gen]() {
                    if (gen != requestGen_.load()) return;
                    AppendSystem("Error: pipe failed");
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    MarkDirty();
                });
                return;
            }

            pid_t pid = fork();
            if (pid < 0) {
                close(inPipe[0]); close(inPipe[1]);
                close(outPipe[0]); close(outPipe[1]);
                events_.Push([this, gen]() {
                    if (gen != requestGen_.load()) return;
                    AppendSystem("Error: fork failed");
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    MarkDirty();
                });
                return;
            }

            if (pid == 0) {
                // Child
                setsid();  // New process group so we can SIGTERM everything
                dup2(inPipe[0], STDIN_FILENO);
                dup2(outPipe[1], STDOUT_FILENO);
                dup2(outPipe[1], STDERR_FILENO);
                // Original fds are O_CLOEXEC so they auto-close on execvp
                execlp("claude", "claude",
                       "--print", "--verbose",
                       "--output-format", "stream-json",
                       "--include-partial-messages",
                       "--dangerously-skip-permissions",
                       "--model", model.c_str(),
                       (char*)nullptr);
                _exit(127);
            }

            // Parent
            claudePid_.store(pid);
            close(inPipe[0]);
            close(outPipe[1]);

            // Write the prompt to child's stdin, then close it so claude
            // sees EOF and starts processing. Use a blocking write loop.
            const char* pbuf = prompt.data();
            size_t premaining = prompt.size();
            while (premaining > 0) {
                ssize_t w = write(inPipe[1], pbuf, premaining);
                if (w <= 0) break;
                pbuf += w; premaining -= w;
            }
            close(inPipe[1]);

            // Read stdout with a buffered line reader. read() chunks of
            // ~8KB and split on newlines — lines can be arbitrarily long
            // (tool_use inputs or tool_result outputs easily exceed 4KB).
            std::string fullResponse;
            std::string lineBuf;
            char chunk[8192];

            while (true) {
                ssize_t n = read(outPipe[0], chunk, sizeof(chunk));
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;  // EOF — child closed stdout
                lineBuf.append(chunk, n);

                size_t start = 0;
                while (true) {
                    size_t nl = lineBuf.find('\n', start);
                    if (nl == std::string::npos) break;
                    std::string line = lineBuf.substr(start, nl - start);
                    start = nl + 1;
                    if (line.empty()) continue;

                    json j;
                    try { j = json::parse(line); } catch (...) { continue; }

                    std::string type = j.value("type", "");
                    if (type == "stream_event" && j.contains("event")) {
                        auto& evt = j["event"];
                        std::string evtType = evt.value("type", "");

                        if (evtType == "content_block_stop") {
                            // End of a thinking block — clear thinking state
                            // so the next text_delta starts a fresh text run
                            // without animating the old thinking block.
                            events_.Push([this, gen]() {
                                if (gen != requestGen_.load()) return;
                                if (receivingThinking_) {
                                    receivingThinking_ = false;
                                    scrollView_.StopAllAnimations();
                                }
                                MarkDirty();
                            });
                        } else if (evtType == "content_block_delta" && evt.contains("delta")) {
                            auto& delta = evt["delta"];
                            std::string deltaType = delta.value("type", "");

                            if (deltaType == "text_delta") {
                                std::string text = delta.value("text", "");
                                if (!text.empty()) {
                                    fullResponse += text;
                                    events_.Push([this, text, gen]() {
                                        if (gen != requestGen_.load()) return;
                                        waitingForResponse_ = false;
                                        if (receivingThinking_) {
                                            receivingThinking_ = false;
                                            scrollView_.StopAllAnimations();
                                            responseBuffer_.clear();
                                            lastMarkdownLen_ = 0;
                                        }
                                        if (responseBuffer_.empty())
                                            responseStartBlock_ = scrollView_.BlockCount();
                                        responseBuffer_ += text;

                                        double now = GetMonotonicTime();
                                        if (now - lastMarkdownTime_ > 0.5 &&
                                            responseBuffer_.size() > lastMarkdownLen_ + 20) {
                                            RenderMarkdownToBlocks();
                                            lastMarkdownTime_ = now;
                                            lastMarkdownLen_ = responseBuffer_.size();
                                        }
                                        MarkDirty();
                                    });
                                }
                            } else if (deltaType == "thinking_delta") {
                                std::string thinking = delta.value("thinking", "");
                                if (!thinking.empty()) {
                                    events_.Push([this, thinking, gen]() {
                                        if (gen != requestGen_.load()) return;
                                        waitingForResponse_ = false;
                                        if (!receivingThinking_) {
                                            receivingThinking_ = true;
                                            scrollView_.AppendStream(BlockType::THINKING, thinking);
                                            scrollView_.StartThinking(scrollView_.BlockCount() - 1);
                                        } else {
                                            scrollView_.ContinueStream(thinking);
                                        }
                                        MarkDirty();
                                    });
                                }
                            }
                        }
                    } else if (type == "result") {
                        bool isError = j.value("is_error", false);
                        if (isError) {
                            std::string err = j.value("error", "Unknown error");
                            events_.Push([this, err, gen]() {
                                if (gen != requestGen_.load()) return;
                                AppendSystem("Error: " + err);
                            });
                        }
                        std::string result = j.value("result", "");
                        if (!result.empty() && fullResponse.empty()) fullResponse = result;
                    }
                }
                if (start > 0) lineBuf.erase(0, start);
            }

            close(outPipe[0]);

            // Reap child. If still running (e.g. we were cancelled and
            // SIGTERM'd the group), give it a moment then force kill.
            int status = 0;
            for (int attempt = 0; attempt < 50; attempt++) {
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid || r < 0) break;
                if (attempt == 10) kill(-pid, SIGTERM);
                if (attempt == 40) kill(-pid, SIGKILL);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            claudePid_.store(-1);

            events_.Push([this, fullResponse, gen]() {
                if (gen != requestGen_.load()) return;
                if (receivingThinking_) {
                    receivingThinking_ = false;
                }
                scrollView_.StopAllAnimations();
                if (!responseBuffer_.empty()) {
                    RenderMarkdownToBlocks(true);
                    responseBuffer_.clear();
                }
                if (!fullResponse.empty())
                    session_.History().push_back({"assistant", fullResponse, {}, {}});
                requestInProgress_ = false;
                sendButton_.enabled = true;
                session_.SetProvider(activeProvider_);
                session_.SetModel(activeModel_);
                session_.Save();
                MarkDirty();
            });
        }).detach();
        return;
    }

    // Zen provider: HTTP streaming
    std::string requestJson = BuildRequestJson();

    responseBuffer_.clear();
    receivingThinking_ = false;
    lastMarkdownLen_ = 0;
    lastMarkdownTime_ = GetMonotonicTime();

    httpClient_.SendStreaming(requestJson,
        // onChunk (bg thread)
        [this](const std::string& chunk, bool isThinking) {
            events_.Push([this, chunk, isThinking]() {
                // First chunk received — stop waiting dots
                waitingForResponse_ = false;

                if (isThinking) {
                    if (!receivingThinking_) {
                        receivingThinking_ = true;
                        scrollView_.AppendStream(BlockType::THINKING, chunk);
                        scrollView_.StartThinking(scrollView_.BlockCount() - 1);
                    } else {
                        scrollView_.ContinueStream(chunk);
                    }
                } else {
                    // Content — finalize thinking if transitioning
                    if (receivingThinking_) {
                        receivingThinking_ = false;
                        scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                        responseStartBlock_ = scrollView_.BlockCount();
                        responseBuffer_.clear();
                        lastMarkdownLen_ = 0;
                    }

                    // Set start block on first content chunk
                    if (responseBuffer_.empty()) {
                        responseStartBlock_ = scrollView_.BlockCount();
                    }

                    responseBuffer_ += chunk;

                    // Progressive markdown: re-render every 500ms if enough new text
                    double now = GetMonotonicTime();
                    if (now - lastMarkdownTime_ > 0.5 &&
                        responseBuffer_.size() > lastMarkdownLen_ + 20) {
                        RenderMarkdownToBlocks();
                        lastMarkdownTime_ = now;
                        lastMarkdownLen_ = responseBuffer_.size();
                    }
                }
                MarkDirty();
            });
        },
        // onComplete (bg thread)
        [this](bool ok, const std::string& content, const std::string& error,
               const std::vector<json>& toolCalls, const std::string& finishReason,
               int, int) {
            events_.Push([this, ok, content, error, toolCalls, finishReason]() {
                waitingForResponse_ = false;

                if (!ok) {
                    if (!session_.History().empty() && session_.History().back().role == "user")
                        session_.History().pop_back();
                    if (error.find("401") != std::string::npos) {
                        AppendSystem("API key is invalid or expired (" + httpClient_.ApiKey() + "). Please set a new key.");
                        keychain::ClearApiKey();
                        httpClient_.SetApiKey("");
                        statusLabel_.text = "Unauthorized";
                    } else {
                        AppendSystem("Error: " + error);
                    }
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    MarkDirty();
                    return;
                }

                if (!toolCalls.empty() && toolRound_ < 40) {
                    // Render any accumulated text before switching to tool display
                    if (!responseBuffer_.empty()) {
                        RenderMarkdownToBlocks(true);
                        responseBuffer_.clear();
                    }
                    if (receivingThinking_) {
                        receivingThinking_ = false;
                        scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                    }
                    ExecuteToolCalls(toolCalls, content);
                    return;
                }

                // Model was cut off by token limit — it may have wanted to call tools.
                // Add what we got to history and auto-continue so it can keep going.
                if (finishReason == "length" && toolRound_ < 40) {
                    if (!content.empty())
                        session_.History().push_back({"assistant", content, {}, {}});
                    // Ask it to continue
                    session_.History().push_back({"user", "[system: your previous response was truncated due to length. Continue where you left off.]", {}, {}});
                    toolRound_++;

                    // Render what we have so far
                    if (!responseBuffer_.empty()) {
                        RenderMarkdownToBlocks(true);
                        responseBuffer_.clear();
                    }

                    DoSendToProvider();
                    MarkDirty();
                    return;
                }

                // Finalize thinking
                if (receivingThinking_) {
                    receivingThinking_ = false;
                    scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                }

                // Final markdown render (complete buffer, no truncation)
                if (!responseBuffer_.empty()) {
                    RenderMarkdownToBlocks(true);
                    responseBuffer_.clear();
                }

                if (!content.empty())
                    session_.History().push_back({"assistant", content, {}, {}});
                requestInProgress_ = false;
                sendButton_.enabled = true;
                scrollView_.StopAllAnimations();
                session_.SetProvider(activeProvider_);
                session_.SetModel(activeModel_);
                session_.Save();
                MarkDirty();
            });
        }
    );
}

void App::RenderMarkdownToBlocks(bool isFinal) {
    if (responseBuffer_.empty()) return;

    // During streaming: only render up to the last paragraph boundary
    // to avoid cutting off incomplete markdown constructs (headings, fences)
    std::string toRender = responseBuffer_;
    if (!isFinal) {
        size_t lastBoundary = toRender.rfind("\n\n");
        if (lastBoundary == std::string::npos || lastBoundary < 10) {
            // Not enough complete paragraphs yet — skip this render
            return;
        }
        toRender = toRender.substr(0, lastBoundary);
    }

    // Remove old content blocks from responseStartBlock_ onwards
    scrollView_.RemoveBlocksFrom(responseStartBlock_);

    // Render markdown
    MarkdownRenderer mdRenderer(14);
    auto mdBlocks = mdRenderer.Render(toRender, true);

    scrollView_.BeginBatch();
    scrollView_.AddBlocks(std::move(mdBlocks));
    scrollView_.EndBatch();
}

void App::ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content) {
    toolRound_++;

    // Add assistant message with tool calls
    session_.History().push_back({"assistant", content, toolCalls, {}});

    // Show tool info in thinking block
    std::string info;
    for (auto& tc : toolCalls) {
        info += "Tool: " + tc.value("name", "") + "\n";
        try {
            auto args = json::parse(tc.value("arguments", "{}"));
            for (auto& [k, v] : args.items()) info += "  " + k + ": " + v.dump() + "\n";
        } catch (...) {}
    }
    scrollView_.AppendStream(BlockType::THINKING, info);
    scrollView_.StartThinking(scrollView_.BlockCount() - 1);
    MarkDirty();

    // Execute in background
    auto tcCopy = toolCalls;
    std::thread([this, tcCopy]() {
        struct Result { std::string id, output; };
        std::vector<Result> results;
        for (auto& tc : tcCopy) {
            results.push_back({
                tc.value("id", ""),
                ExecuteTool(tc.value("name", ""), tc.value("arguments", "{}"))
            });
        }

        events_.Push([this, results, tcCopy]() {
            // Show results
            std::string resultText;
            for (size_t i = 0; i < results.size(); i++) {
                resultText += "\n" + tcCopy[i].value("name", "") + ":\n" + StripAnsi(results[i].output) + "\n";
            }
            scrollView_.ContinueStream(resultText);

            // Add to history
            for (size_t i = 0; i < results.size(); i++) {
                session_.History().push_back({"tool", results[i].output, {}, results[i].id});
            }

            scrollView_.StopThinking(scrollView_.BlockCount() - 1);
            DoSendToProvider();  // Continue the loop
            MarkDirty();
        });
    }).detach();
}

// ============================================================================
// Input handling
// ============================================================================

void App::OnMouseDown(float x, float y, bool shift) {
    if (chooserMode_) {
        float s = window_.Scale();
        float lh = scrollView_.Fonts().LineHeight(FontStyle::Regular);
        float rowH = lh * 2.8f;
        float pad = 16 * s;
        float titleH = scrollView_.Fonts().LineHeight(FontStyle::Heading2) + pad;
        float startY = titleH + pad;
        int idx = (int)((y + chooserScroll_ - startY) / rowH);
        if (idx >= 0 && idx < (int)chooserSessions_.size()) {
            ChooserSelect(idx);
        } else if (idx == (int)chooserSessions_.size()) {
            // "New workspace" - use message input as path input
            chooserMode_ = false;
            char cwdBuf[4096];
            std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
            session_.SetCwd(cwd);
            session_.LoadForCwd(cwd);
            window_.SetTitle(("FCN — " + cwd).c_str());
            RestoreSessionToView();
            StartConnect();
        }
        MarkDirty();
        return;
    }

    // All coords are physical pixels from WaylandWindow

    // Check dropdowns first (they have popups)
    if (workspaceDropdown_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (providerDropdown_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (modelDropdown_.OnMouseDown(x, y)) { MarkDirty(); return; }

    // Close any open dropdowns
    workspaceDropdown_.Close();
    providerDropdown_.Close();
    modelDropdown_.Close();

    if (sendButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (apiKeyEditing_) {
        if (apiKeyAccept_.OnMouseDown(x, y)) { MarkDirty(); return; }
        if (apiKeyCancel_.OnMouseDown(x, y)) { MarkDirty(); return; }
    } else {
        if (apiKeyButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    }

    // Try each text input — clicking one unfocuses the other
    TextInput* inputs[] = {&apiKeyInput_, &messageInput_};
    for (auto* inp : inputs) {
        if (inp->OnMouseDown(x, y, scrollView_.Fonts())) {
            for (auto* other : inputs)
                if (other != inp) other->focused = false;
            if (inp == &messageInput_) scrollView_.ClearSelection();
            MarkDirty();
            return;
        }
    }

    // Scroll view
    float s = window_.Scale();
    float viewH = window_.Height() - (barHeight_ + inputHeight_ + chromeTopPad_) * s;
    if (y < viewH) {
        scrollView_.OnMouseDown(x, y, shift);
        UnfocusAllInputs();
        MarkDirty();
    }
}

void App::OnMouseUp(float x, float y) {
    sendButton_.OnMouseUp(x, y);
    if (apiKeyEditing_) { apiKeyAccept_.OnMouseUp(x, y); apiKeyCancel_.OnMouseUp(x, y); }
    else apiKeyButton_.OnMouseUp(x, y);
    scrollView_.OnMouseUp(x, y);
    MarkDirty();
}

void App::OnMouseMove(float x, float y, bool leftDown) {
    if (chooserMode_) {
        float s = window_.Scale();
        float lh = scrollView_.Fonts().LineHeight(FontStyle::Regular);
        float rowH = lh * 2.8f;
        float pad = 16 * s;
        float titleH = scrollView_.Fonts().LineHeight(FontStyle::Heading2) + pad;
        float startY = titleH + pad;
        int idx = (int)((y + chooserScroll_ - startY) / rowH);
        int maxIdx = (int)chooserSessions_.size(); // includes "New" row
        int newHov = (idx >= 0 && idx <= maxIdx) ? idx : -1;
        if (newHov != chooserHovered_) { chooserHovered_ = newHov; MarkDirty(); }
        return;
    }
    // Mouse drag in text input
    auto* focused = FocusedInput();
    if (leftDown && focused) {
        focused->OnMouseDrag(x, y, scrollView_.Fonts());
        MarkDirty();
        return;
    }

    sendButton_.OnMouseMove(x, y);
    if (apiKeyEditing_) { apiKeyAccept_.OnMouseMove(x, y); apiKeyCancel_.OnMouseMove(x, y); }
    else apiKeyButton_.OnMouseMove(x, y);
    providerDropdown_.OnMouseMove(x, y);
    modelDropdown_.OnMouseMove(x, y);

    float s = window_.Scale();
    float viewH = window_.Height() - (barHeight_ + inputHeight_ + chromeTopPad_) * s;
    if (y < viewH) {
        scrollView_.OnMouseMove(x, y, leftDown);
    }
    MarkDirty();
}

void App::OnScroll(float delta) {
    if (chooserMode_) {
        chooserScroll_ -= delta * 40;
        if (chooserScroll_ < 0) chooserScroll_ = 0;
        MarkDirty();
        return;
    }
    scrollView_.OnScroll(delta);
    MarkDirty();
}

void App::OnKey(int key, int mods, bool pressed) {
    if (!pressed) return;

    if (chooserMode_) {
        int maxIdx = (int)chooserSessions_.size(); // includes "New" row
        if (key == XKB_KEY_Down || key == XKB_KEY_j) {
            chooserHovered_ = std::min(chooserHovered_ + 1, maxIdx);
            MarkDirty();
        } else if (key == XKB_KEY_Up || key == XKB_KEY_k) {
            chooserHovered_ = std::max(chooserHovered_ - 1, 0);
            MarkDirty();
        } else if (key == XKB_KEY_Return || key == XKB_KEY_KP_Enter) {
            if (chooserHovered_ >= 0 && chooserHovered_ < (int)chooserSessions_.size()) {
                ChooserSelect(chooserHovered_);
            } else if (chooserHovered_ == (int)chooserSessions_.size()) {
                // "New workspace"
                chooserMode_ = false;
                char cwdBuf[4096];
                std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
                session_.SetCwd(cwd);
                session_.LoadForCwd(cwd);
                window_.SetTitle(("FCN — " + cwd).c_str());
                PopulateWorkspaceDropdown();
                RestoreSessionToView();
                StartConnect();
            }
            MarkDirty();
        } else if (key == XKB_KEY_Escape) {
            glfwSetWindowShouldClose(window_.Handle(), GLFW_TRUE);
        }
        return;
    }

    // Escape closes API key editor
    if (key == XKB_KEY_Escape && apiKeyEditing_) {
        apiKeyInput_.Clear();
        apiKeyEditing_ = false;
        LayoutWidgets();
        MarkDirty();
        return;
    }

    // Escape cancels request
    if (key == XKB_KEY_Escape && requestInProgress_) {
        // Bump generation so any finalizer from the cancelled request is
        // dropped when it eventually arrives.
        ++requestGen_;
        httpClient_.Abort();
        // Claude ACP: signal the child process group so fgets unblocks and
        // the reader thread can finish and reap. Otherwise the detached
        // thread lives on holding a pipe to an orphaned claude child.
        pid_t cpid = claudePid_.exchange(-1);
        if (cpid > 0) kill(-cpid, SIGTERM);
        requestInProgress_ = false;
        sendButton_.enabled = true;
        if (!session_.History().empty() && session_.History().back().role == "user") {
            // Keep message but mark as cancelled
            session_.History().push_back({"assistant", "[cancelled]", {}, {}});
        }
        scrollView_.StopAllAnimations();
        receivingThinking_ = false;
        responseBuffer_.clear();
        AppendSystem("Cancelled");
        MarkDirty();
        return;
    }

    // Ctrl+C: copy from focused input, or scroll view
    if ((mods & Mod::Ctrl) && key == Key::C) {
        auto* inp = FocusedInput();
        if (inp && inp->selStart != inp->selEnd) {
            std::string sel = inp->GetSelectedText();
            if (!sel.empty()) {
#ifdef FCN_LINUX
                FILE* p = popen("wl-copy 2>/dev/null", "w");
#else
                FILE* p = popen("pbcopy 2>/dev/null", "w");
#endif
                if (p) { fwrite(sel.c_str(), 1, sel.size(), p); pclose(p); }
            }
        } else {
            scrollView_.OnKey(key, mods);
        }
        MarkDirty();
        return;
    }

    // Ctrl+A: select all in scroll view (unless text input focused)
    if ((mods & Mod::Ctrl) && key == Key::A && !FocusedInput()) {
        scrollView_.OnKey(key, mods);
        MarkDirty();
        return;
    }

    // Forward to focused text input
    auto* inp = FocusedInput();
    if (inp) {
        inp->OnKey(key, mods, scrollView_.Fonts());
        MarkDirty();
        return;
    }

    // Scroll view keyboard
    scrollView_.OnKey(key, mods);
    MarkDirty();
}

void App::OnChar(uint32_t codepoint) {
    auto* inp = FocusedInput();
    if (inp) {
        inp->OnChar(codepoint, scrollView_.Fonts());
        MarkDirty();
    }
}

// ============================================================================
// Main loop
// ============================================================================

// ============================================================================
// Session Chooser
// ============================================================================

void App::PaintChooser() {
    auto& fm = scrollView_.Fonts();
    float s = window_.Scale();
    float w = window_.Width();
    float h = window_.Height();
    float lh = fm.LineHeight(FontStyle::Regular);
    float rowH = lh * 2.8f;
    float pad = 16 * s;
    float titleH = fm.LineHeight(FontStyle::Heading2) + pad;

    // Background
    renderer_.DrawRect(0, 0, w, h, {0.1f, 0.1f, 0.12f, 1});

    // Title
    auto titleRun = fm.Shape("Select a workspace", FontStyle::Heading2);
    float titleX = (w - titleRun.totalWidth) / 2;
    renderer_.DrawShapedRun(fm, titleRun, titleX, pad, fm.Ascent(FontStyle::Heading2),
                            {0.9f, 0.9f, 0.9f, 1});

    float startY = titleH + pad;
    float listW = std::min(w - pad * 2, 700 * s);
    float listX = (w - listW) / 2;

    for (int i = 0; i < (int)chooserSessions_.size(); i++) {
        float y = startY + i * rowH - chooserScroll_;
        if (y + rowH < 0 || y > h) continue;

        auto& si = chooserSessions_[i];
        bool hov = (chooserHovered_ == i);

        // Row background
        Color bg = hov ? Color{0.2f, 0.25f, 0.35f, 1} : Color{0.14f, 0.14f, 0.16f, 1};
        renderer_.DrawRoundedRect(listX, y, listW, rowH - 4 * s, 6 * s, bg);

        // CWD path (bold)
        auto pathRun = fm.Shape(si.cwd, FontStyle::Bold);
        renderer_.DrawShapedRun(fm, pathRun, listX + pad, y + lh * 0.3f,
                                fm.Ascent(FontStyle::Bold), {0.85f, 0.85f, 0.9f, 1});

        // Secondary info
        std::string info = si.model;
        if (si.messageCount > 0) info += "  ·  " + std::to_string(si.messageCount) + " messages";
        if (!si.lastUsed.empty()) info += "  ·  " + si.lastUsed.substr(0, 16);
        auto infoRun = fm.Shape(info, FontStyle::Regular);
        renderer_.DrawShapedRun(fm, infoRun, listX + pad, y + lh * 1.5f,
                                fm.Ascent(FontStyle::Regular), {0.5f, 0.5f, 0.55f, 1});
    }

    // "New workspace..." row at the bottom
    int newIdx = (int)chooserSessions_.size();
    float newY = startY + newIdx * rowH - chooserScroll_;
    if (newY + rowH > 0 && newY < h) {
        bool hov = (chooserHovered_ == newIdx);
        Color bg = hov ? Color{0.2f, 0.3f, 0.25f, 1} : Color{0.14f, 0.14f, 0.16f, 1};
        renderer_.DrawRoundedRect(listX, newY, listW, rowH - 4 * s, 6 * s, bg);
        auto newRun = fm.Shape("+ New workspace...", FontStyle::Bold);
        renderer_.DrawShapedRun(fm, newRun, listX + pad, newY + lh * 0.8f,
                                fm.Ascent(FontStyle::Bold), {0.5f, 0.8f, 0.5f, 1});
    }
}

void App::ChooserSelect(int idx) {
    if (idx < 0 || idx >= (int)chooserSessions_.size()) return;
    std::string dir = chooserSessions_[idx].cwd;
    if (!std::filesystem::exists(dir)) {
        AppendSystem("Directory not found: " + dir);
        return;
    }
    chdir(dir.c_str());
    session_.SetCwd(dir);
    session_.LoadForCwd(dir);
    chooserMode_ = false;
    window_.SetTitle(("FCN — " + dir).c_str());
    PopulateWorkspaceDropdown();
    RestoreSessionToView();
    StartConnect();
    MarkDirty();
}

void App::ChooserSelectPath(const std::string& path) {
    std::string dir = path;
    // Expand ~ to HOME
    if (!dir.empty() && dir[0] == '~') {
        const char* home = getenv("HOME");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    if (!std::filesystem::is_directory(dir)) {
        AppendSystem("Not a directory: " + dir);
        return;
    }
    chdir(dir.c_str());
    session_.SetCwd(dir);
    session_.LoadForCwd(dir);
    chooserMode_ = false;
    window_.SetTitle(("FCN — " + dir).c_str());
    RestoreSessionToView();
    StartConnect();
    MarkDirty();
}

void App::Run() {
    double lastTime = GetMonotonicTime();

    while (!window_.ShouldClose()) {
        // Block until something happens (poll during active requests or animations)
        bool showingDots = waitingForResponse_ && (GetMonotonicTime() - requestStartTime_ > 1.5);
        if (!dirty_ && events_.Empty() && !scrollView_.NeedsRedraw() && !showingDots && !requestInProgress_) {
            window_.WaitEvents();
        } else {
            window_.PollEvents();
        }

        // Drain bg thread events
        events_.Drain();

        double now = GetMonotonicTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        if (!chooserMode_) {
            messageInput_.Update(dt, scrollView_.Fonts());
            if (apiKeyEditing_)
                apiKeyInput_.Update(dt, scrollView_.Fonts());
            scrollView_.Update(dt);
        }

        // Animate waiting dots
        if (waitingForResponse_) {
            waitingDotAnim_ += dt;
            double elapsed = now - requestStartTime_;
            if (elapsed > 1.5) MarkDirty();  // Keep redrawing for animation
        }

        if (!dirty_ && !scrollView_.NeedsRedraw()) continue;
        dirty_ = false;
        scrollView_.ClearDirty();

        // Drain again in case events arrived during update
        events_.Drain();

        // Render
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        renderer_.BeginFrame(window_.Width(), window_.Height(), scrollView_.Fonts());

        if (chooserMode_) {
            PaintChooser();
        } else {
            // Scroll view (top portion)
            scrollView_.Paint(renderer_);

            // Waiting dots (plain, below last block, after 3s)
            if (waitingForResponse_ && (now - requestStartTime_ > 1.5)) {
                auto& fm = scrollView_.Fonts();
                float dotR = fm.LineHeight(FontStyle::Regular) * 0.15f;
                float spacing = dotR * 3;
                float baseX = 20;
                float baseY = scrollView_.ContentBottom() + fm.LineHeight(FontStyle::Regular);
                int frame = (int)(waitingDotAnim_ * 4) % 4;  // 0-3 animation frame
                for (int d = 0; d < 3; d++) {
                    float alpha = (d == (frame % 3)) ? 1.0f : 0.3f;
                    Color c{0.5f, 0.5f, 0.5f, alpha};
                    float cx = baseX + d * spacing;
                    renderer_.DrawRect(cx - dotR, baseY - dotR, dotR * 2, dotR * 2, c);
                }
            }

            // Bottom bar + input
            PaintBottomBar();
        }

        renderer_.EndFrame();
        window_.SwapBuffers();

        // Throttle to ~60fps since vsync is off (Wayland frame callback issue)
        struct timespec sleepTime = {0, 8000000};  // 8ms (~120fps headroom)
        nanosleep(&sleepTime, nullptr);

        if (!events_.Empty()) dirty_ = true;

        // Perf tracking
        clock_gettime(CLOCK_MONOTONIC, &t1);
        static int frameCount = 0;
        static long totalUs = 0;
        static double perfStart = now;
        long frameUs = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        totalUs += frameUs;
        frameCount++;
        if (now - perfStart > 2.0 && frameCount > 0) {
            FILE* pf = fopen("/tmp/fcn-native-perf.log", "a");
            if (pf) {
                fprintf(pf, "PERF: %d frames in %.1fs (%.0f/s) | avg %.0fus (%.2fms)\n",
                        frameCount, now - perfStart,
                        frameCount / (now - perfStart),
                        (double)totalUs / frameCount,
                        (double)totalUs / frameCount / 1000.0);
                fclose(pf);
            }
            frameCount = 0; totalUs = 0; perfStart = now;
        }
    }
}
