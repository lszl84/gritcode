// Gritcode — GPU-rendered AI coding harness
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
#include "debug_log.h"
#include "version.h"
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
#include "clipboard.h"
#ifdef GRIT_MACOS
#include <mach-o/dyld.h>
#endif

// pipe2(O_CLOEXEC) is available on Linux and FreeBSD; on macOS fall back to pipe + fcntl.
static int CloexecPipe(int pfd[2]) {
#if defined(__linux__) || defined(__FreeBSD__)
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
            setenv("GRIT_RESOLVING_ENVIRONMENT", "1", 1);
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
            DLOG("shell-env: no markers in %zu bytes of output", output.size());
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
            DLOG("shell-env: expected %d vars, got %zu", NVARS, values.size());
            return;
        }

        std::vector<std::pair<std::string, std::string>> kvs;
        for (int i = 0; i < NVARS; i++) {
            if (!values[i].empty()) kvs.emplace_back(kVars[i], values[i]);
        }
        events.Push([kvs]() {
            for (auto& kv : kvs) setenv(kv.first.c_str(), kv.second.c_str(), 1);
            DLOG("shell-env: applied %zu vars from login shell", kvs.size());
        });
    }).detach();
}

// Tool execution — uses O_CLOEXEC pipe so backgrounded processes (php -S ... &)
// don't inherit the pipe fd and block reads forever.  120s timeout via alarm().
// Process-group id of the currently-executing bash tool, published by
// RunCommand for the duration of the child's lifetime. The Escape handler
// reads this to deliver SIGKILL to the whole group, which unblocks the read
// loop fast and lets the tool thread drop its (now-stale) result.
// File scope so RunCommand can stay a free function (simpler than making it
// a method for one atomic).
static std::atomic<pid_t> g_toolPgid{0};

static std::string RunCommand(const std::string& cmd) {
    int pfd[2];
    if (CloexecPipe(pfd) < 0) return "Error: pipe failed";

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return "Error: fork failed"; }

    if (pid == 0) {
        // Child: new session so we can kill the group on timeout or cancel.
        setsid();
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    // Parent.
    close(pfd[1]);
    g_toolPgid.store(pid);

    // Read loop. The old version waited for pipe EOF, which meant any
    // orphaned grandchild that kept the stdout fd (e.g. `hugo serve -D &`)
    // would hold the pipe open and stall the tool for the full 120 s
    // timeout. New version treats bash's own exit as the primary "return
    // now" signal — same behaviour Bun's spawn gives opencode for free —
    // and drains any remaining buffered bytes before returning. Orphan
    // holders don't matter once bash is gone.
    std::string result;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    bool timedOut = false;
    int status = 0;
    bool reaped = false;

    while (true) {
        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) reaped = true;
            else if (w < 0 && errno != EINTR) break;
        }

        auto now = std::chrono::steady_clock::now();
        auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        if (!reaped && remainingMs <= 0) {
            timedOut = true;
            kill(-pid, SIGKILL);
            // Fall through to keep draining / let waitpid catch the exit.
            remainingMs = 200;
        }

        // Short poll: lets us detect bash exit within ~100 ms of it happening
        // even if the pipe has nothing to read, and lets a SIGKILL from
        // Escape or timeout take effect fast.
        int pollMs = reaped ? 50 : 100;
        if (!reaped && remainingMs >= 0 && remainingMs < pollMs)
            pollMs = (int)remainingMs;
        if (pollMs < 1) pollMs = 1;

        struct pollfd rpfd = {pfd[0], POLLIN, 0};
        int pret = poll(&rpfd, 1, pollMs);
        if (pret > 0 && (rpfd.revents & POLLIN)) {
            ssize_t n = read(pfd[0], buf, sizeof(buf));
            if (n > 0) {
                result.append(buf, n);
                if (result.size() > 32768) { result += "\n...(truncated)"; break; }
                continue;
            }
            if (n == 0) break;  // Pipe EOF (no holders left).
            if (errno == EINTR) continue;
            break;
        }
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // poll timed out with no data this iteration.
        if (reaped) break;  // bash done, pipe idle → nothing more to wait for.
    }

    close(pfd[0]);

    // Final reap (may have been killed by Escape; in that case it's usually
    // already exited by the time we get here).
    if (!reaped) {
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    g_toolPgid.store(0);

    if (timedOut) result += "\n[killed: command timed out after 120s]";
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        result += "\n[exit code: " + std::to_string(WEXITSTATUS(status)) + "]";
    return result;
}

// Path to the currently-running grit binary. Used by the ACP path to point
// Claude's --mcp-config at the same binary (for `grit --mcp-stdio`), so the
// memory MCP server always matches the instance that spawned it.
static std::string SelfExePath() {
#ifdef GRIT_MACOS
    uint32_t sz = 0;
    _NSGetExecutablePath(nullptr, &sz);
    std::string buf(sz, '\0');
    if (_NSGetExecutablePath(buf.data(), &sz) != 0) return {};
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return std::string(buf);
#endif
}

// Writes a per-user MCP config that registers `grit --mcp-stdio` as a stdio
// MCP server named "grit-history". Returned path is passed to claude via
// --mcp-config on each ACP spawn, so the grit_history_search tool is available to
// Claude without the user ever touching their own claude MCP config.
static std::string WriteClaudeMcpConfig() {
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else if (home) base = std::string(home) + "/.cache";
    else base = "/tmp";
    std::string dir = base + "/gritcode";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/claude-mcp.json";

    std::string self = SelfExePath();
    if (self.empty()) return {};

    json cfg;
    cfg["mcpServers"]["grit-history"] = {
        {"command", self},
        {"args", json::array({"--mcp-stdio"})}
    };

    std::string tmp = path + ".tmp";
    std::ofstream f(tmp);
    if (!f) return {};
    f << cfg.dump(2);
    f.close();
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) return {};
    return path;
}

static std::string ExpandTilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return home + path.substr(1);
    return path;
}

static std::string CollapseHome(const std::string& path) {
    const char* home = getenv("HOME");
    if (!home || !*home) return path;
    std::string h(home);
    if (path == h) return "~";
    if (path.size() > h.size() && path.compare(0, h.size(), h) == 0 &&
        path[h.size()] == '/') {
        return "~" + path.substr(h.size());
    }
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

// Wrap a string in single quotes for /bin/sh, escaping embedded single quotes.
static std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Case-insensitive find for HTML tag stripping.
static size_t FindICase(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || needle.size() > hay.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            char a = (char)std::tolower((unsigned char)hay[i + k]);
            char b = (char)std::tolower((unsigned char)needle[k]);
            if (a != b) { match = false; break; }
        }
        if (match) return i;
    }
    return std::string::npos;
}

// Delete <tag ...>...</tag> blocks from s (case-insensitive, handles attrs).
static void RemoveHtmlBlock(std::string& s, const std::string& tag) {
    std::string openPrefix = "<" + tag;
    std::string closeTag = "</" + tag + ">";
    size_t pos = 0;
    while (true) {
        size_t p = FindICase(s, openPrefix, pos);
        if (p == std::string::npos) break;
        // Ensure the char after prefix is space, '>', or '/' — else skip.
        char after = (p + openPrefix.size() < s.size()) ? s[p + openPrefix.size()] : '>';
        if (after != ' ' && after != '>' && after != '/' && after != '\t' && after != '\n') {
            pos = p + 1;
            continue;
        }
        size_t endOpen = s.find('>', p);
        if (endOpen == std::string::npos) { s.erase(p); break; }
        size_t r = FindICase(s, closeTag, endOpen + 1);
        if (r == std::string::npos) { s.erase(p); break; }
        s.erase(p, r + closeTag.size() - p);
        pos = p;
    }
}

// Encode a unicode codepoint as UTF-8.
static void AppendUtf8(std::string& out, int cp) {
    if (cp <= 0) return;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Very small HTML → plain-text converter. Strips script/style/head, drops
// tags (inserting newlines at tag boundaries), decodes the common entities,
// and collapses runs of whitespace. Not a full parser — enough to turn a
// search result or doc page into something a model can read.
static std::string StripHtml(const std::string& html) {
    std::string s = html;
    RemoveHtmlBlock(s, "script");
    RemoveHtmlBlock(s, "style");
    RemoveHtmlBlock(s, "head");
    RemoveHtmlBlock(s, "noscript");
    RemoveHtmlBlock(s, "svg");

    // Drop tags; leave a newline behind each closing '>' so block structure survives.
    std::string notags;
    notags.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (inTag) {
            if (c == '>') { inTag = false; notags += '\n'; }
        } else {
            if (c == '<') inTag = true;
            else notags += c;
        }
    }

    // Decode entities.
    std::string decoded;
    decoded.reserve(notags.size());
    for (size_t i = 0; i < notags.size(); ++i) {
        if (notags[i] == '&') {
            size_t semi = notags.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = notags.substr(i + 1, semi - i - 1);
                bool handled = true;
                if (ent == "amp") decoded += '&';
                else if (ent == "lt") decoded += '<';
                else if (ent == "gt") decoded += '>';
                else if (ent == "quot") decoded += '"';
                else if (ent == "apos") decoded += '\'';
                else if (ent == "nbsp") decoded += ' ';
                else if (!ent.empty() && ent[0] == '#') {
                    int code = 0;
                    try {
                        if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                            code = std::stoi(ent.substr(2), nullptr, 16);
                        else
                            code = std::stoi(ent.substr(1));
                    } catch (...) { code = 0; handled = false; }
                    if (handled) AppendUtf8(decoded, code);
                } else {
                    handled = false;
                }
                if (handled) { i = semi; continue; }
            }
        }
        decoded += notags[i];
    }

    // Collapse whitespace: single spaces within a line, cap blank lines at 1.
    std::string out;
    out.reserve(decoded.size());
    int nl = 0;
    bool spaceQueued = false;
    for (char c : decoded) {
        if (c == '\n' || c == '\r') {
            ++nl;
            if (nl <= 2) { if (!out.empty() && out.back() != '\n') out += '\n'; else if (nl == 2) out += '\n'; }
            spaceQueued = false;
        } else if (c == ' ' || c == '\t') {
            spaceQueued = true;
        } else {
            if (spaceQueued && !out.empty() && out.back() != '\n') out += ' ';
            spaceQueued = false;
            nl = 0;
            out += c;
        }
    }
    // Trim leading/trailing whitespace.
    size_t a = out.find_first_not_of(" \n\t");
    size_t b = out.find_last_not_of(" \n\t");
    if (a == std::string::npos) return "";
    return out.substr(a, b - a + 1);
}

// Chrome UA — many sites (Cloudflare, Mediawiki, etc.) 403 anything that
// looks like a bot. Matches what opencode ships.
static constexpr const char* kBrowserUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

static std::string WebFetch(const std::string& url) {
    if (url.empty()) return "Error: url required";
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
        return "Error: url must start with http:// or https://";
    std::string cmd = "curl -sSL --max-time 30 "
                      "-A " + ShellQuote(kBrowserUA) + " "
                      "-H 'Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8' "
                      "-H 'Accept-Language: en-US,en;q=0.9' "
                      "-- " + ShellQuote(url);
    std::string raw = RunCommand(cmd);
    if (raw.empty()) return "Error: empty response from " + url;
    std::string text = StripHtml(raw);
    if (text.size() > 30000) {
        text.resize(30000);
        text += "\n...(truncated at 30000 chars)";
    }
    return text;
}

// Parse the SSE stream returned by https://mcp.exa.ai/mcp. Each event is
// "event: <name>\ndata: <json>\n\n". We care about data lines whose JSON has
// result.content[] entries of type "text" — Exa packs each search result as
// a preformatted "Title: ... URL: ... Highlights: ..." string in that field.
static std::string ParseExaSse(const std::string& sse) {
    std::string combined;
    size_t i = 0;
    while (i < sse.size()) {
        size_t eol = sse.find('\n', i);
        if (eol == std::string::npos) eol = sse.size();
        std::string line = sse.substr(i, eol - i);
        i = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 6, "data: ") != 0) continue;
        std::string payload = line.substr(6);
        try {
            auto j = json::parse(payload);
            if (j.contains("result") && j["result"].contains("content")) {
                for (auto& c : j["result"]["content"]) {
                    if (c.value("type", "") == "text") {
                        if (!combined.empty()) combined += "\n\n";
                        combined += c.value("text", "");
                    }
                }
            } else if (j.contains("error")) {
                return "Error: " + j["error"].dump();
            }
        } catch (...) {
            // Not JSON or malformed — skip.
        }
    }
    return combined;
}

// Search the web via Exa's public MCP endpoint (no API key). Same mechanism
// opencode uses; avoids DuckDuckGo's aggressive captcha wall.
static std::string WebSearch(const std::string& query) {
    if (query.empty()) return "Error: query required";
    json body = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {
            {"name", "web_search_exa"},
            {"arguments", {
                {"query", query},
                {"numResults", 5}
            }}
        }}
    };
    std::string cmd = "curl -sS --max-time 30 -X POST "
                      "-H 'accept: application/json, text/event-stream' "
                      "-H 'content-type: application/json' "
                      "-d " + ShellQuote(body.dump()) + " "
                      "-- 'https://mcp.exa.ai/mcp'";
    std::string raw = RunCommand(cmd);
    if (raw.empty()) return "Error: empty response from Exa MCP";
    std::string text = ParseExaSse(raw);
    if (text.empty()) return "Error: no results parsed from Exa response:\n" + raw.substr(0, 1000);
    if (text.size() > 20000) {
        text.resize(20000);
        text += "\n...(truncated at 20000 chars)";
    }
    return text;
}

static std::string ExecuteTool(const std::string& name, const std::string& argsJson,
                               MemoryDB* memory, const std::string& currentSessionId) {
    try {
        auto args = json::parse(argsJson);
        if (name == "bash") return RunCommand(args.value("command", ""));
        if (name == "read_file") return RunCommand("cat -n -- '" + ExpandTilde(args.value("path", "")) + "'");
        if (name == "list_directory") return RunCommand("ls -la -- '" + ExpandTilde(args.value("path", ".")) + "'");
        if (name == "write_file") return WriteFile(args.value("path", ""), args.value("content", ""));
        if (name == "edit_file") return EditFile(args.value("path", ""), args.value("old_string", ""), args.value("new_string", ""));
        if (name == "web_fetch") return WebFetch(args.value("url", ""));
        if (name == "web_search") return WebSearch(args.value("query", ""));
        if (name == "grit_history_search") {
            if (!memory || !memory->IsOpen()) return "Memory index unavailable.";
            std::string query = args.value("query", "");
            int limit = args.value("limit", 5);
            if (limit < 1) limit = 1;
            if (limit > 20) limit = 20;
            if (query.empty()) return "Error: 'query' is required.";
            auto hits = memory->Search(query, limit, currentSessionId);
            return MemoryDB::FormatSearchHits(hits);
        }
        if (name == "grit_history_fetch") {
            if (!memory || !memory->IsOpen()) return "Memory index unavailable.";
            std::string sid = args.value("session_id", "");
            int ti = args.value("turn_index", -1);
            int before = args.value("before", 2);
            int after  = args.value("after", 2);
            if (sid.empty() || ti < 0)
                return "Error: 'session_id' and 'turn_index' are required.";
            auto hits = memory->Fetch(sid, ti, before, after);
            return MemoryDB::FormatFetchHits(hits);
        }
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
    tools.push_back({{"type","function"},{"function",{
        {"name","web_fetch"},{"description","Fetch a web page by URL and return its readable text content (HTML tags stripped). Use for reading docs, articles, or any HTTP(S) resource. Output is truncated to 30000 chars."},
        {"parameters",{{"type","object"},{"properties",{
            {"url",{{"type","string"},{"description","Absolute http:// or https:// URL to fetch"}}}
        }},{"required",json::array({"url"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","web_search"},{"description","Search the web via DuckDuckGo and return result titles, URLs, and snippets as plain text. Use to find current information, documentation, or pages to follow up on with web_fetch."},
        {"parameters",{{"type","object"},{"properties",{
            {"query",{{"type","string"},{"description","Search query"}}}
        }},{"required",json::array({"query"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","grit_history_search"},
        {"description","Search the transcripts of past gritcode conversations across every project the user has worked on (excludes the current session). This is the AUTHORITATIVE source of prior-conversation recall in gritcode — use it first whenever the user references any past work (\"last time\", \"once again\", \"we had\", \"how did we\", \"in <project>\", \"that thing from <name>\"). Do NOT try to list any memory directory under ~/.claude — that is unrelated and unused here. Query budget: typically 1-3 searches per user question. Start with ONE broad keyword query; only run more if the first returned strong hits that suggest specific follow-ups. If a search returns no clearly relevant hits, STOP searching and answer from what you have (or ask the user) — do not fire speculative variant queries hoping one will match. Query syntax: 2-5 keywords, FTS5 (phrases in quotes, AND/OR/NOT, prefix*). Returns short snippets (~20-token window) with session_id, turn_index, and full_chars. Follow up with grit_history_fetch for full context when a snippet looks load-bearing. Prefer this over guessing or asking the user to re-explain context they've already provided."},
        {"parameters",{{"type","object"},{"properties",{
            {"query",{{"type","string"},{"description","2-5 keywords. FTS5 MATCH expression."}}},
            {"limit",{{"type","integer"},{"description","Max results (default 5, max 20)."}}}
        }},{"required",json::array({"query"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","grit_history_fetch"},
        {"description","Read the full text of a specific past turn plus a window of surrounding turns. Use after grit_history_search when a snippet looks relevant and you need the surrounding conversation for context. session_id + turn_index come directly from a grit_history_search hit. Default window is 2 turns before + 2 after (5 total)."},
        {"parameters",{{"type","object"},{"properties",{
            {"session_id",{{"type","string"},{"description","Opaque session id from grit_history_search result."}}},
            {"turn_index",{{"type","integer"},{"description","Turn index from grit_history_search result."}}},
            {"before",{{"type","integer"},{"description","How many prior turns to include (default 2, max 10)."}}},
            {"after",{{"type","integer"},{"description","How many following turns to include (default 2, max 10)."}}}
        }},{"required",json::array({"session_id","turn_index"})}}}
    }}});
    return tools.dump();
}

// ============================================================================
// Init
// ============================================================================

bool App::Init(bool sessionChooser) {
    chooserMode_ = sessionChooser;
    if (!window_.Init(1000, 750, "Gritcode")) return false;
    Clipboard::Init(&window_);

    // Memory DB — opened opportunistically. If the open fails we just won't
    // index or search; the rest of the app keeps working.
    memory_.Open(MemoryDB::DefaultPath());

    // Write the claude --mcp-config JSON that registers grit-history as a
    // stdio MCP server. The path is passed to every ACP spawn below so the
    // grit_history_search tool surfaces under Claude without the user editing
    // their own MCP config. Empty on failure → we just skip the flag.
    claudeMcpConfigPath_ = WriteClaudeMcpConfig();

    if (!scrollView_.Init(window_.Width(), window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * window_.Scale()),
                          window_.Scale()))
        return false;

    scrollView_.SetAutoScroll(true);
    scrollView_.SetClipboardFunc([&](const std::string& t) {
        Clipboard::Copy(t);
    });

    if (!renderer_.Init()) {
        DLOG("GL renderer init failed");
        return false;
    }

    // CSD (Wayland) needs the app's FontManager to rasterize the titlebar title.
    window_.SetFontManager(&scrollView_.Fonts());

    // Workspace dropdown
    workspaceDropdown_.onSelect = [&](int i, const std::string& id) { OnWorkspaceChanged(i, id); };

    // Provider dropdown
    providerDropdown_.items = {
        {"zen", "OpenCode Zen"},
        {"opencode-go", "OpenCode Go"},
        {"claude", "Claude (ACP)"},
    };
    providerDropdown_.selectedIndex = 0;
    providerDropdown_.onSelect = [&](int i, const std::string& id) { OnProviderChanged(i, id); };

    // Model dropdown (populated after connect)
    modelDropdown_.items = {};
    modelDropdown_.onSelect = [&](int i, const std::string& id) { OnModelChanged(i, id); };

    // Send button
    sendButton_.text = "Send";
    sendButton_.onClick = [&]() { SendMessage(); };

    // Queue-mode buttons — visible only when the agent is idle AND the queue
    // is non-empty (see LayoutWidgets). Continue dispatches the queued
    // messages; Clear drops them all and restores the normal input.
    continueQueueButton_.text = "Continue";
    continueQueueButton_.visible = false;
    continueQueueButton_.onClick = [&]() {
        DispatchNextQueued();
    };
    clearQueueButton_.text = "Clear queue";
    clearQueueButton_.visible = false;
    clearQueueButton_.onClick = [&]() {
        pendingQueue_.clear();
        LayoutWidgets();
        MarkDirty();
    };

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

    // Clipboard paste helper (works on Wayland, X11, and macOS)
    auto pasteFromClipboard = [&]() -> std::string {
        return Clipboard::Paste();
    };

    apiKeyInput_.onPaste = pasteFromClipboard;

    // Message input
    messageInput_.placeholder = "Type a message...";
    messageInput_.onSubmit = [&](const std::string&) { SendMessage(); };
    messageInput_.onPaste = pasteFromClipboard;

    statusLabel_.text = "Disconnected";
    versionLabel_.text = "v" GRIT_VERSION;
    versionLabel_.color = {0.35f, 0.35f, 0.37f};
    versionLabel_.rightAlign = true;

    // Window callbacks
    window_.OnResize([&](int w, int h, float scale) {
        // LayoutWidgets updates chipRowHeight_ (which depends on queue state),
        // so call it first and then derive viewH from the freshly-computed
        // chrome size.
        LayoutWidgets();
        int viewH = h - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * scale) - (int)chipRowHeight_;
        if (viewH < 1) viewH = 1;
        scrollView_.OnResize(w, viewH);
        MarkDirty();
    });
    // Scale changes (HiDPI: monitor swap, compositor sending preferred_buffer_scale
    // after the initial render) need a font-atlas re-rasterization, which is what
    // ScrollView::Init does. Without this the first frame paints with fonts baked
    // at the wrong scale and only normalizes when the user triggers a manual
    // resize — confusing on HiDPI launch.
    window_.OnScaleChange([&](float scale) {
        LayoutWidgets();
        int w = window_.Width();
        int h = window_.Height();
        int viewH = h - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * scale) - (int)chipRowHeight_;
        if (viewH < 1) viewH = 1;
        scrollView_.Init(w, viewH, scale);
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
    window_.OnFocusChange([&](bool focused) {
        // Drop input focus on deactivate so the blinking cursor + bright focus
        // border don't keep drawing in a clearly-inactive window (libadwaita
        // apps behave the same way). On reactivate, jump focus back to the
        // message box — if the API key dialog is open, that one wins instead.
        //
        // Debounce: the Wayland compositor sends a transient keyboard
        // leave+enter pair during xdg_toplevel_move (clicking the titlebar
        // starts a move grab). Applying those immediately makes the focus
        // border flash. Apply focus=true instantly; defer focus=false by
        // ~60ms and cancel it if a focus=true arrives within that window.
        focusCbGen_++;
        uint64_t gen = focusCbGen_;
        if (focused) {
            if (windowFocused_) {
                MarkDirty();
                return;
            }
            windowFocused_ = true;
            if (apiKeyEditing_) {
                apiKeyInput_.focused = true;
                messageInput_.focused = false;
            } else {
                messageInput_.focused = true;
                apiKeyInput_.focused = false;
            }
            redirectNextClickToInput_ = true;
            MarkDirty();
        } else {
            if (!windowFocused_) return;
            std::thread([this, gen]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                events_.Push([this, gen]() {
                    if (gen != focusCbGen_) return;
                    windowFocused_ = false;
                    UnfocusAllInputs();
                    MarkDirty();
                });
            }).detach();
        }
    });

    LayoutWidgets();

    // Kick off async shell env resolution — PATH and friends from the
    // user's login shell, needed so GUI-launched grit can find claude,
    // ffmpeg, etc. Runs in a detached thread; result is applied on the
    // main thread via events_ before any user-triggered subprocess is
    // likely to run.
    ResolveShellEnvAsync(events_);

    window_.Show();
    messageInput_.focused = true;

    // Registry fetch is independent of which session (or none) we end up on —
    // kick it off unconditionally so the chooser-path sessions also get their
    // model list. Previously this only ran in the direct-launch branch, so
    // picking opencode-go from the chooser left Connect() parked forever on
    // "Go (loading models...)" with an empty dropdown.
    StartFetchRegistry();

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
        window_.SetTitle(CollapseHome(cwd).c_str());
        debug::SetSessionId(session_.SessionId());
        PopulateWorkspaceDropdown();
        RestoreSessionToView();
        StartConnect();
    }

    StartMCP();
    return true;
}

void App::RestoreSessionToView() {
    AppendSystem("\xe2\x9a\xa0\xef\xb8\x8f No permission system — agents can execute any command on your machine. Press Escape to cancel a running request.");

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
                (savedProvider == "zen" || savedProvider == "opencode-go" ||
                 savedProvider == "claude")) {
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
            // Claude (ACP) doesn't go through the Zen HTTP layer — the
            // restore event already called OnProviderChanged("claude"),
            // which set activeProvider_ and populated the model dropdown.
            // Connect() would turn around and call
            // PopulateModelsFromRegistry("claude"), which errors out and
            // empties the dropdown.
            if (activeProvider_ == "claude") return;
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
#ifdef GRIT_ENABLE_MCP
    MCPCallbacks cb;

    cb.sendMessage = [this](const std::string& msg) {
        events_.Push([this, msg]() {
            messageInput_.text = msg;
            SendMessage();
        });
    };

    cb.getStatus = [this]() -> json {
        json queue = json::array();
        for (auto& q : pendingQueue_) queue.push_back(q);
        return {
            {"connected", connected_},
            {"requestInProgress", requestInProgress_},
            {"provider", activeProvider_},
            {"model", activeModel_},
            {"toolRound", toolRound_},
            {"messageCount", (int)session_.History().size()},
            {"waitingDotsVisible", waitingDotFrame_ >= 0},
            {"thinkingDotsActive", scrollView_.HasActiveThinking()},
            {"sendButtonEnabled", sendButton_.enabled},
            {"pendingQueue", queue},
            {"continueQueueVisible", continueQueueButton_.visible}
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

    cb.setWorkspace = [this](const std::string& cwd) {
        events_.Push([this, cwd]() {
            OnWorkspaceChanged(0, cwd);
        });
    };

    cb.cancelRequest = [this]() {
        events_.Push([this]() {
            CancelInFlight();
        });
    };

    cb.selectAllText = [this]() -> std::string {
        // Runs on the MCP thread, but SelectAll and GetSelectedText only
        // touch ScrollView state that Paint also touches. The MCP server
        // only calls this between Paint rebuilds, and test usage is
        // serialized with sendMessage/wait cycles, so a quick touch here
        // is safe enough for the test path.
        scrollView_.SelectAll();
        return scrollView_.GetSelectedText();
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
#endif
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

    // Chip layout (sets chipRowHeight_ for viewport math below).
    ComputeQueueChipLayout();

    float barY = h - bar;
    float inputY = barY - inp;

    // Idle-queue mode: agent idle but queue non-empty — swap input/Send for
    // Continue/Clear so the user has to explicitly resume or clear.
    bool idleQueueMode = !requestInProgress_ && !pendingQueue_.empty();
    messageInput_.visible = !idleQueueMode;
    sendButton_.visible = !idleQueueMode;
    continueQueueButton_.visible = idleQueueMode;
    clearQueueButton_.visible = idleQueueMode;
    if (idleQueueMode) messageInput_.focused = false;

    if (requestInProgress_) {
        sendButton_.text = "Add";
        sendButton_.enabled = pendingQueue_.size() < kMaxQueue_;
    } else {
        sendButton_.text = "Send";
        sendButton_.enabled = true;
    }

    messageInput_.bounds = {8, inputY + 5, w - 103, inp - 10};
    sendButton_.bounds = {w - 88, inputY + 5, 80, inp - 10};

    if (idleQueueMode) {
        float contW = 120 * s;
        float clearW = 140 * s;
        float gap = 8 * s;
        float totalW = contW + clearW + gap;
        float startX = (w - totalW) * 0.5f;
        continueQueueButton_.bounds = {startX, inputY + 5, contW, inp - 10};
        clearQueueButton_.bounds = {startX + contW + gap, inputY + 5, clearW, inp - 10};
    }

    // Bottom bar — responsive. Three dropdowns are always visible. As width
    // shrinks, hide labels/buttons in priority order: version → status → API
    // key. If even the dropdowns at full size don't fit, shrink them
    // proportionally (same idea as the message input above).
    const float leftPad = 8.0f;
    const float rightPad = 8.0f;
    const float gap = 5.0f * s;
    const float groupGap = 16.0f * s;
    float wsW = 200 * s;
    float provW = 140 * s;
    float modW = 150 * s;
    const float statusW = 180 * s;
    const float versionW = 120 * s;
    const float apiBtnW = 100 * s;
    const float apiInpW = 220 * s;
    const float keyBtnW = 40 * s;

    bool showApiKey = (activeProvider_ == "zen" || activeProvider_ == "opencode-go");
    bool editingApiKey = showApiKey && apiKeyEditing_;
    bool wantApiBtn = showApiKey && !editingApiKey;

    auto totalWidth = [&](float dropW, bool stat, bool ver, bool apiBtn) {
        float t = leftPad + dropW;
        if (stat) t += gap + statusW;
        float right = 0;
        if (editingApiKey) {
            right = apiInpW + keyBtnW * 2 + 8 * s;
            if (ver) right += versionW + 10 * s;
        } else {
            if (apiBtn) right += apiBtnW;
            if (ver) right += (right > 0 ? 10 * s : 0) + versionW;
        }
        if (right > 0) t += groupGap + right;
        return t + rightPad;
    };

    float dropdownsW = wsW + provW + modW + 2 * gap;
    bool showStatus = true;
    bool showVersion = true;
    bool showApiKeyBtn = wantApiBtn;
    if (totalWidth(dropdownsW, showStatus, showVersion, showApiKeyBtn) > w) showVersion = false;
    if (totalWidth(dropdownsW, showStatus, showVersion, showApiKeyBtn) > w) showStatus = false;
    if (!editingApiKey &&
        totalWidth(dropdownsW, showStatus, showVersion, showApiKeyBtn) > w) showApiKeyBtn = false;

    if (totalWidth(dropdownsW, showStatus, showVersion, showApiKeyBtn) > w) {
        // Even bare-bones doesn't fit — shrink the three dropdowns to share
        // whatever's left after reserving space for the API-key edit row
        // (the only right-side group we can't hide).
        float reservedRight = editingApiKey
            ? groupGap + apiInpW + keyBtnW * 2 + 8 * s
            : 0;
        float availForDropdowns = w - leftPad - rightPad - reservedRight - 2 * gap;
        if (availForDropdowns < 0) availForDropdowns = 0;
        const float origTotal = 200.0f + 140.0f + 150.0f;
        wsW = availForDropdowns * (200.0f / origTotal);
        provW = availForDropdowns * (140.0f / origTotal);
        modW = availForDropdowns * (150.0f / origTotal);
    }

    float bx = leftPad;
    workspaceDropdown_.bounds = {bx, barY + 5, wsW, bar - 10}; bx += wsW + gap;
    providerDropdown_.bounds = {bx, barY + 5, provW, bar - 10}; bx += provW + gap;
    modelDropdown_.bounds = {bx, barY + 5, modW, bar - 10}; bx += modW + gap;

    statusLabel_.visible = showStatus;
    if (showStatus) {
        statusLabel_.bounds = {bx, barY + 5, statusW, bar - 10};
    }

    float rightEdge = w - rightPad;
    if (editingApiKey) {
        apiKeyButton_.visible = false;
        float editStart = rightEdge - (apiInpW + keyBtnW * 2 + 8 * s);
        apiKeyInput_.bounds = {editStart, barY + 5, apiInpW, bar - 10};
        apiKeyAccept_.bounds = {editStart + apiInpW + 4, barY + 5, keyBtnW, bar - 10};
        apiKeyCancel_.bounds = {editStart + apiInpW + keyBtnW + 8, barY + 5, keyBtnW, bar - 10};
        versionLabel_.visible = showVersion;
        if (showVersion) {
            versionLabel_.bounds = {editStart - versionW - 10 * s, barY + 5, versionW, bar - 10};
        }
    } else {
        apiKeyButton_.visible = showApiKeyBtn;
        float vlx = rightEdge;
        if (showApiKeyBtn) {
            apiKeyButton_.bounds = {rightEdge - apiBtnW, barY + 5, apiBtnW, bar - 10};
            vlx = rightEdge - apiBtnW - 10 * s;
        }
        versionLabel_.visible = showVersion;
        if (showVersion) {
            versionLabel_.bounds = {vlx - versionW, barY + 5, versionW, bar - 10};
        }
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

    // Chrome background includes the chip row above the input when queue is non-empty.
    float chromeY = inputY - topPad - chipRowHeight_;
    float chromeH = inp + bar + topPad + chipRowHeight_;
    renderer_.DrawRect(0, chromeY, w, chromeH, chromeBg);

    if (chipRowHeight_ > 0) {
        PaintQueueChips();
    }

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
        apiKeyInput_.Paint(renderer_, fm);
        apiKeyAccept_.Paint(renderer_, fm);
        apiKeyCancel_.Paint(renderer_, fm);
    } else {
        apiKeyButton_.Paint(renderer_, fm);
    }
    messageInput_.Paint(renderer_, fm);
    sendButton_.Paint(renderer_, fm);
    continueQueueButton_.Paint(renderer_, fm);
    clearQueueButton_.Paint(renderer_, fm);

    // Dropdown popups last (z-order: on top of everything)
    workspaceDropdown_.PaintPopup(renderer_, fm);
    providerDropdown_.PaintPopup(renderer_, fm);
    modelDropdown_.PaintPopup(renderer_, fm);
}

// Build a one-line display label for a queued message: collapse whitespace,
// truncate to ~48 codepoints with an ellipsis.
static std::string ChipLabelFor(const std::string& raw) {
    std::string label;
    label.reserve(raw.size());
    for (char c : raw) label += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    const size_t maxCp = 48;
    size_t cp = 0, byteEnd = 0;
    while (byteEnd < label.size() && cp < maxCp) {
        unsigned char c = (unsigned char)label[byteEnd];
        if (c < 0x80) byteEnd++;
        else if (c < 0xE0) byteEnd += 2;
        else if (c < 0xF0) byteEnd += 3;
        else byteEnd += 4;
        cp++;
    }
    if (byteEnd < label.size()) return label.substr(0, byteEnd) + "…";
    return label;
}

void App::ComputeQueueChipLayout() {
    queueChipHits_.clear();
    chipRowHeight_ = 0;
    if (pendingQueue_.empty()) return;

    float s = window_.Scale();
    float w = window_.Width();
    float bottomMargin = 2 * s;
    float h = window_.Height() - bottomMargin;
    float bar = barHeight_ * s;
    float inp = inputHeight_ * s;
    float inputY = h - bar - inp;

    float chipH = 28 * s;
    float rowGap = 6 * s;
    float hPad = 12 * s;
    float closeSize = chipH - 8 * s;
    float closePad = 4 * s;
    float startX = 8.0f * s;
    float endX = w - 8.0f * s;
    float chipW = std::max(closeSize * 3.0f, endX - startX);

    size_t numRows = pendingQueue_.size();
    chipRowHeight_ = (float)numRows * chipH + (float)(numRows + 1) * rowGap;

    float chipsTopY = inputY - chipRowHeight_ + rowGap;

    for (size_t i = 0; i < pendingQueue_.size(); i++) {
        float rowY = chipsTopY + (float)i * (chipH + rowGap);

        QueueChipHit hit;
        hit.chip = {startX, rowY, chipW, chipH};
        hit.bodyHit = {startX, rowY, chipW - closeSize - closePad, chipH};
        float closeX = startX + chipW - hPad - closeSize;
        hit.closeBtn = {closeX - closePad, rowY, closeSize + closePad * 2, chipH};
        hit.label = ChipLabelFor(pendingQueue_[i]);
        hit.idx = i;
        queueChipHits_.push_back(hit);
    }
}

void App::PaintQueueChips() {
    if (queueChipHits_.empty()) return;

    float s = window_.Scale();
    auto& fm = scrollView_.Fonts();
    Color chipBg{0.18f, 0.18f, 0.20f};
    Color chipText{0.82f, 0.82f, 0.84f};
    Color closeBg{0.28f, 0.14f, 0.14f};
    Color closeIcon{0.92f, 0.72f, 0.72f};

    float hPad = 12 * s;
    float chipH = 28 * s;
    float closeSize = chipH - 8 * s;

    // Text clip reserves room for the close button on the right.
    float textRightPad = closeSize + hPad * 2;

    for (const auto& hit : queueChipHits_) {
        float x = hit.chip.x;
        float y = hit.chip.y;
        float cw = hit.chip.w;

        renderer_.DrawRoundedRect(x, y, cw, chipH, chipH * 0.5f, chipBg);

        renderer_.PushClip(x + hPad, y, cw - textRightPad, chipH);
        auto run = fm.Shape(hit.label, FontStyle::Regular);
        float ty = y + (chipH - fm.VisibleHeight(FontStyle::Regular)) / 2;
        renderer_.DrawShapedRun(fm, run, x + hPad, ty, fm.Ascent(FontStyle::Regular), chipText);
        renderer_.PopClip();

        float cxIcon = x + cw - hPad - closeSize;
        float cyIcon = y + (chipH - closeSize) / 2;
        renderer_.DrawRoundedRect(cxIcon, cyIcon, closeSize, closeSize, closeSize * 0.5f, closeBg);

        auto xrun = fm.Shape("\xc3\x97", FontStyle::Regular);
        float xw = xrun.totalWidth;
        float xVisH = fm.VisibleHeight(FontStyle::Regular);
        float xAscent = fm.Ascent(FontStyle::Regular);
        float xx = cxIcon + (closeSize - xw) / 2.0f;
        float xy = cyIcon + (closeSize - xVisH) / 2.0f;
        renderer_.DrawShapedRun(fm, xrun, xx, xy, xAscent, closeIcon);
    }
}

// ============================================================================
// Connection & Chat
// ============================================================================

void App::Connect(const std::string& apiKey) {
    // Base URL depends on the active registry-backed provider. OpenCode Go is
    // a different subscription tier served at /zen/go/v1; both use the same
    // OPENCODE_API_KEY.
    std::string baseUrl = (activeProvider_ == "opencode-go")
        ? "https://opencode.ai/zen/go/v1"
        : "https://opencode.ai/zen/v1";
    httpClient_.SetBaseUrl(baseUrl);
    httpClient_.SetApiKey(apiKey);
    connected_ = true;
    const char* providerLabel = (activeProvider_ == "opencode-go") ? "Go" : "Zen";
    if (apiKey.empty() && activeProvider_ == "opencode-go") {
        statusLabel_.text = "Go (no key — add one)";
    } else {
        statusLabel_.text = apiKey.empty()
            ? std::string(providerLabel) + " (Anonymous)"
            : "Validating key...";
    }
    MarkDirty();

    // Registry is the canonical source — prefer it whenever it's loaded,
    // for both Zen and Go. Fall back to /models only for Zen if the registry
    // hasn't arrived yet (OpenCode Go has no /models endpoint — 404).
    if (registryLoaded_) {
        PopulateModelsFromRegistry(activeProvider_);
        statusLabel_.text = apiKey.empty()
            ? (activeProvider_ == "opencode-go" ? "Go (no key — add one)" : "Zen (Anonymous)")
            : (activeProvider_ == "opencode-go" ? "Go" : "Zen");
        return;
    }

    if (activeProvider_ == "opencode-go") {
        // No legacy discovery endpoint — wait for the registry fetch.
        statusLabel_.text = "Go (loading models...)";
        return;
    }

    httpClient_.FetchModels([this, apiKey](std::vector<net::ModelInfo> models, int httpStatus) {
        // If we have an API key, validate it against an authenticated endpoint
        int authStatus = 0;
        if (!apiKey.empty()) {
            std::string testModel = models.empty() ? "minimax-m2.5-free" : models[0].id;
            authStatus = httpClient_.ValidateKey(testModel);
            if (authStatus == 401) {
                events_.Push([this, apiKey]() {
                    // Mirror the streaming-path fix: 401 does not nuke a saved
                    // key anymore. Surface the error, leave the key alone.
                    AppendSystem("Unauthorized (HTTP 401). Key was not cleared — retry, or replace it manually via the key button if it's really invalid.");
                    statusLabel_.text = "Unauthorized";
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
        // 401 on the /models probe used to wipe the saved key — same bug as
        // the streaming-path handler. Don't. Surface, let the user decide.
        AppendSystem("Unauthorized (HTTP 401) while listing models. Key was not cleared.");
        statusLabel_.text = "Unauthorized";
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
        if (models[i].id == "minimax-m2.5-free") defaultIdx = modelDropdown_.items.size() - 1;
    }

    if (modelDropdown_.items.empty()) {
        modelDropdown_.items = {
            {"minimax-m2.5-free", "MiniMax M2.5 Free"},
        };
    }

    // Selection priority mirrors the registry path: restoredModelPref_ >
    // activeModel_ > minimax-m2.5-free > 0. Preserving activeModel_ keeps a second
    // Connect() from clobbering a session's restored selection.
    if (!activeModel_.empty()) {
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id == activeModel_) {
                defaultIdx = (int)i;
                break;
            }
        }
    }
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

std::string App::GetRegistryCachePath() {
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else if (home) base = std::string(home) + "/.cache";
    else base = "/tmp";
    return base + "/gritcode/models.json";
}

void App::StartFetchRegistry() {
    // Fast-path: hydrate from disk cache synchronously so the dropdown has
    // something to show before the network comes back. Mirrors what the
    // opencode CLI does with its ~/.cache/opencode/models.json.
    std::string cachePath = GetRegistryCachePath();
    try {
        std::ifstream f(cachePath);
        if (f) {
            json cached;
            f >> cached;
            if (cached.is_object() && !cached.empty()) {
                modelsRegistry_ = std::move(cached);
                registryLoaded_ = true;
            }
        }
    } catch (...) {}

    // Always refresh in the background so a stale cache doesn't pin us to
    // an out-of-date model list.
    net::CurlHttpClient::FetchJson("https://models.dev/api.json",
        [this](json body, int status) {
            events_.Push([this, body, status]() { OnRegistryReceived(body, status); });
        });
}

void App::OnRegistryReceived(json registry, int httpStatus) {
    if (httpStatus != 200 || !registry.is_object() || registry.empty()) {
        // Don't surface this as an error if we already have a cache loaded —
        // the user can keep working against the stale copy.
        if (!registryLoaded_) {
            AppendSystem("Could not load models.dev registry (HTTP " +
                         std::to_string(httpStatus) + "). Zen will fall back to /models; OpenCode Go will be unavailable until the registry loads.");
        }
        return;
    }

    modelsRegistry_ = std::move(registry);
    registryLoaded_ = true;

    // Persist to disk for next launch.
    try {
        std::string cachePath = GetRegistryCachePath();
        std::filesystem::create_directories(
            std::filesystem::path(cachePath).parent_path());
        std::ofstream(cachePath) << modelsRegistry_.dump();
    } catch (...) {}

    // If the user is already on a registry-backed provider, refresh the
    // dropdown so they see the canonical list instead of whatever /models
    // or the cache had.
    if (activeProvider_ == "claude") {
        PopulateClaudeModels();
    } else if (activeProvider_ == "zen" || activeProvider_ == "opencode-go") {
        PopulateModelsFromRegistry(activeProvider_);
        // Connect() parked the status label at "Go (loading models...)" while
        // waiting for the registry. Now that it's here, move it to the normal
        // connected state so the user isn't stuck staring at a loading string.
        if (connected_) {
            bool anon = httpClient_.IsAnonymous();
            if (activeProvider_ == "opencode-go") {
                statusLabel_.text = anon ? "Go (no key — add one)" : "Go";
            } else {
                statusLabel_.text = anon ? "Zen (Anonymous)" : "Zen";
            }
            MarkDirty();
        }
    }
}

void App::PopulateModelsFromRegistry(const std::string& providerId) {
    if (!registryLoaded_) return;

    // grit calls the Zen subscription "zen", but models.dev keys it under
    // "opencode" (the CLI vendor namespace). Translate so the registry path
    // actually resolves — without this, Zen on a warm cache silently falls
    // through to an empty dropdown.
    std::string registryKey = (providerId == "zen") ? "opencode" : providerId;

    if (!modelsRegistry_.contains(registryKey) ||
        !modelsRegistry_[registryKey].is_object()) {
        AppendSystem("Provider '" + providerId + "' not found in models.dev registry.");
        modelDropdown_.items.clear();
        return;
    }

    const auto& p = modelsRegistry_[registryKey];
    std::string providerNpm = p.value("npm", "");

    modelDropdown_.items.clear();

    if (p.contains("models") && p["models"].is_object()) {
        for (auto it = p["models"].begin(); it != p["models"].end(); ++it) {
            const std::string& mid = it.key();
            const auto& m = it.value();

            // Filter to the two wire protocols grit supports:
            //   @ai-sdk/openai-compatible → /chat/completions
            //   @ai-sdk/anthropic         → /messages
            // Any other shape (e.g. @ai-sdk/google) is still unsupported and
            // gets skipped so we don't surface broken-on-click models.
            std::string npm = providerNpm;
            if (m.contains("provider") && m["provider"].is_object() &&
                m["provider"].contains("npm") && m["provider"]["npm"].is_string()) {
                npm = m["provider"]["npm"].get<std::string>();
            }
            if (npm != "@ai-sdk/openai-compatible" && npm != "@ai-sdk/openai" && npm != "@ai-sdk/anthropic") continue;

            std::string name = m.value("name", mid);
            modelDropdown_.items.push_back({mid, name});
        }
    }

    // Registry iteration order is arbitrary (JSON object), so sort by display
    // name to get a stable, predictable dropdown.
    std::sort(modelDropdown_.items.begin(), modelDropdown_.items.end(),
              [](const DropdownItem& a, const DropdownItem& b) {
                  return a.label < b.label;
              });

    if (modelDropdown_.items.empty()) {
        AppendSystem("No OpenAI-compatible models available for " + providerId +
                     " (all are routed through a protocol grit doesn't support yet).");
        activeModel_.clear();
        MarkDirty();
        return;
    }

    // Selection priority:
    //   1. restoredModelPref_ — explicit session-restore hint.
    //   2. activeModel_        — whatever is already selected (keeps a
    //      second Connect() from clobbering the first one's session restore).
    //   3. minimax-m2.5-free   — default free model.
    //   4. index 0.
    int defaultIdx = 0;
    for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
        if (modelDropdown_.items[i].id == "minimax-m2.5-free") {
            defaultIdx = (int)i;
            break;
        }
    }
    if (!activeModel_.empty()) {
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id == activeModel_) {
                defaultIdx = (int)i;
                break;
            }
        }
    }
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
    AppendSystem("Models loaded from registry (" +
                 std::to_string(modelDropdown_.items.size()) +
                 " for " + providerId + "). Active: " + activeModel_);
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
        // Shorten path: ~/Developer/gritcode → .../Developer/gritcode
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

void App::OnWorkspaceChanged(int, const std::string& id) {
    // A request in flight owns scrollView_ and session_.History() via the
    // events queue — swapping workspaces under it corrupts the old session
    // (response lost) and the new one (response lands in the wrong history).
    // Block the switch until the user stops or finishes the current turn.
    // Future work: detach the in-flight request into a per-session context
    // so switches are non-destructive.
    if (requestInProgress_) {
        AppendSystem("Can't switch workspaces while a request is in progress. "
                     "Wait for the current response to finish, or stop it first.");
        // Revert the dropdown to the current workspace so the UI reflects
        // reality (it already updated selectedIndex when the user clicked).
        const std::string& currentCwd = session_.SessionId();
        for (size_t i = 0; i < workspaceDropdown_.items.size(); i++) {
            if (workspaceDropdown_.items[i].id == currentCwd) {
                workspaceDropdown_.selectedIndex = (int)i;
                break;
            }
        }
        MarkDirty();
        return;
    }

    // Save current session first
    SaveAndIndex();

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
    window_.SetTitle(CollapseHome(id).c_str());
    RestoreSessionToView();

    // Re-connect with current provider
    StartConnect();
    MarkDirty();
}

void App::OnProviderChanged(int, const std::string& id) {
    activeProvider_ = id;
    apiKeyButton_.visible = (id == "zen" || id == "opencode-go");
    AppendSystem("Switched to " + id);
    LayoutWidgets();

    if (id == "zen" || id == "opencode-go") {
        std::string key = keychain::LoadApiKey();
        Connect(key);
    } else {
        statusLabel_.text = "Claude (ACP)";
        connected_ = true;
        PopulateClaudeModels();
    }
    MarkDirty();
}

void App::PopulateClaudeModels() {
    // Populate Claude models from the registry (filtered to Anthropic
    // protocol). Keeps the list in sync as Anthropic adds/removes models.
    modelDropdown_.items.clear();
    const std::string registryKey = "opencode";
    bool fromRegistry = false;
    if (registryLoaded_ && modelsRegistry_.contains(registryKey) &&
        modelsRegistry_[registryKey].is_object()) {
        const auto& p = modelsRegistry_[registryKey];
        if (p.contains("models") && p["models"].is_object()) {
            for (auto it = p["models"].begin(); it != p["models"].end(); ++it) {
                const std::string& mid = it.key();
                const auto& m = it.value();
                std::string npm = p.value("npm", "");
                if (m.contains("provider") && m["provider"].is_object() &&
                    m["provider"].contains("npm") && m["provider"]["npm"].is_string()) {
                    npm = m["provider"]["npm"].get<std::string>();
                }
                if (npm != "@ai-sdk/anthropic") continue;
                // models.dev tags some third-party models (MiniMax, Qwen)
                // with the Anthropic wire protocol because they expose a
                // compatible API. Require a `claude-` id prefix to keep
                // them out of the ACP dropdown.
                if (mid.rfind("claude-", 0) != 0) continue;
                std::string name = m.value("name", mid);
                modelDropdown_.items.push_back({mid, name});
            }
        }
        std::sort(modelDropdown_.items.begin(), modelDropdown_.items.end(),
                  [](const DropdownItem& a, const DropdownItem& b) {
                      return a.label < b.label;
                  });
        fromRegistry = !modelDropdown_.items.empty();
    }

    if (modelDropdown_.items.empty()) {
        // Fallback if registry hasn't loaded yet. restoredModelPref_ is
        // preserved so the real registry-backed refresh can honor it once
        // OnRegistryReceived fires.
        modelDropdown_.items = {
            {"claude-opus-4-7", "Claude Opus 4.7"},
            {"claude-sonnet-4-6", "Claude Sonnet 4.6"},
            {"claude-haiku-4-5", "Claude Haiku 4.5"},
        };
    }

    // Selection priority: restoredModelPref_ > activeModel_ > default sonnet.
    int sel = -1;
    if (!restoredModelPref_.empty()) {
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id == restoredModelPref_) { sel = (int)i; break; }
        }
        if (sel >= 0 && fromRegistry) restoredModelPref_.clear();
    }
    if (sel < 0 && !activeModel_.empty()) {
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id == activeModel_) { sel = (int)i; break; }
        }
    }
    if (sel < 0) {
        sel = 0;
        for (size_t i = 0; i < modelDropdown_.items.size(); i++) {
            if (modelDropdown_.items[i].id.find("sonnet") != std::string::npos) {
                sel = (int)i; break;
            }
        }
    }
    modelDropdown_.selectedIndex = sel;
    activeModel_ = modelDropdown_.items[sel].id;
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

void App::AppendSystemExpandable(const std::string& summary, const std::string& detail) {
    if (detail.empty()) {
        AppendSystem(summary);
        return;
    }
    scrollView_.AppendStream(BlockType::THINKING, summary);
    // Set the expand text on the newly created block
    auto& last = scrollView_.Blocks().back();
    // Try to pretty-print JSON detail
    if (!detail.empty()) {
        try {
            auto j = json::parse(detail);
            std::string dumped = j.dump(2);
            if (dumped.size() > 10000) dumped.resize(10000);
            last->expandText = dumped;
        } catch (...) {
            last->expandText = detail.size() > 10000 ? detail.substr(0, 10000) : detail;
        }
    }
    // Force expandable + collapsed so user can click to expand
    last->isExpandable = true;
    last->isCollapsed = true;
    scrollView_.RequestRebuild();
    MarkDirty();
}

// ============================================================================
// Sending messages
// ============================================================================

// Return the model's context window and max output tokens from the registry.
// Defaults to 128K context, 32K output — safe for most models.
App::ModelLimits App::GetModelLimits() {
    ModelLimits lim;
    // Default: safe 128K context, 16K max output response
    lim.contextWindow = 128000;
    lim.maxOutput = 65536;

    std::string registryKey = (activeProvider_ == "zen" || activeProvider_ == "opencode-go")
        ? "opencode" : activeProvider_;
    if (registryLoaded_ && modelsRegistry_.contains(registryKey) &&
        modelsRegistry_[registryKey].is_object()) {
        const auto& p = modelsRegistry_[registryKey];
        if (p.contains("models") && p["models"].is_object() &&
            p["models"].contains(activeModel_) &&
            p["models"][activeModel_].is_object()) {
            const auto& m = p["models"][activeModel_];
            if (m.contains("limit") && m["limit"].is_object()) {
                const auto& l = m["limit"];
                if (l.contains("context") && l["context"].is_number())
                    lim.contextWindow = l["context"].get<int>();
                if (l.contains("output") && l["output"].is_number())
                    lim.maxOutput = l["output"].get<int>();
                // Some models specify input limit separately
                if (l.contains("input") && l["input"].is_number())
                    lim.contextWindow = l["input"].get<int>();
            }
        }
    }
    return lim;
}

std::string App::BuildRequestJson() {
    ModelLimits limits = GetModelLimits();
    json j;
    j["model"] = activeModel_;
    j["stream"] = true;
    j["max_tokens"] = limits.maxOutput;

    // Context management: estimate how many tokens the full history would
    // consume, and if it exceeds the model's context budget, compact it.
    //
    // Strategy (inspired by opencode but more generous):
    //   1. Compute token budget = context_window - max_output - 4000 buffer
    //   2. Add system prompt + tool defs overhead (~1500 tokens)
    //   3. Walk backwards through history, keeping as many recent messages
    //      as fit within the budget.
    //   4. For messages that don't fit, replace their tool_result content
    //      with "[Old tool result content cleared]" and their
    //      reasoning_content with null, then check if they now fit.
    //   5. If still over budget, drop the oldest messages entirely and
    //      insert a compaction summary.
    //
    // This is more generous than opencode's PRUNE_PROTECT (which keeps
    // ~10K tokens of tool output). We protect ~30K tokens of recent
    // tool output and only clear older tool results.

    int tokenBudget = limits.contextWindow - limits.maxOutput - 4000;
    if (tokenBudget < 8000) tokenBudget = 8000;  // floor

    auto& hist = session_.History();
    int histSize = (int)hist.size();

    // --- Phase 1: Prune old tool results (content-heavy, low info value) ---
    // Walk backwards protecting ~30K tokens of tool output, then clear
    // older results.
    const size_t PROTECT_CHARS = 120000;  // ~30K tokens worth of tool output
    size_t toolCharsAccum = 0;
    int pruneBeforeIdx = -1;
    for (int i = histSize - 1; i >= 0; i--) {
        if (hist[i].role == "tool") {
            toolCharsAccum += hist[i].content.size();
            if (toolCharsAccum > PROTECT_CHARS) {
                pruneBeforeIdx = i;
                break;
            }
        }
    }

    // --- Phase 2: Estimate total message tokens and compact if needed ---
    // First build all messages as-is (with pruned tool results), then
    // check if we're over budget and drop oldest messages if so.
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
            "You have these tools: bash, read_file, write_file, edit_file, list_directory, "
            "web_fetch, web_search, grit_history_search, grit_history_fetch.\n"
            "- Use write_file to create new files or overwrite existing ones.\n"
            "- Use edit_file to make targeted changes — provide enough context in old_string to be unique.\n"
            "- Use read_file to read files before editing them.\n"
            "- Use bash to run commands, tests, install packages, etc.\n"
            "- Use grit_history_search whenever the user references any prior work in this or another "
            "project (\"last time\", \"once again\", \"we had\", \"in <project>\", \"how did we\"). It "
            "searches the transcripts of past gritcode conversations across all projects and returns "
            "short snippets. Pass 2-5 keywords. This is the authoritative source of prior-conversation "
            "recall — do not fall back to listing any ~/.claude/... memory directory.\n"
            "- Follow up with grit_history_fetch(session_id, turn_index) to read the full turn and "
            "surrounding context when a snippet looks relevant.\n\n"
            "## Behavior\n"
            "- When given a task, work through it step by step using your tools.\n"
            "- Do not just describe what you would do — actually do it by calling tools.\n"
            "- NEVER say 'Let me...' or 'I will...' without immediately calling the tools to do it.\n"
            "  Actions must ALWAYS be tool calls, never text descriptions of planned actions.\n"
            "- After each tool result, assess whether the task is complete and continue if not.\n"
            "  If you need to do more work, immediately call more tools. Do NOT stop and wait.\n"
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
            // Only send reasoning_content for models/providers that support it.
            // The zen provider rejects null values in unknown fields with 400.
            if (!m.reasoningContent.empty()) {
                msg["reasoning_content"] = m.reasoningContent;
            }
        } else if (m.role == "assistant") {
            msg["content"] = m.content;
            if (!m.reasoningContent.empty()) {
                msg["reasoning_content"] = m.reasoningContent;
            }
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(msg);
    }

    // OpenAI requires alternating user/assistant roles. Consecutive user
    // messages can happen when the model returns empty and the user types
    // "continue" multiple times. Merge them into single user messages.
    {
        json merged = json::array();
        for (auto& m : msgs) {
            if (!merged.empty() && merged.back()["role"] == "user" && m["role"] == "user") {
                std::string& prev = merged.back()["content"].get_ref<std::string&>();
                prev += "\n\n" + m["content"].get_ref<std::string&>();
            } else {
                merged.push_back(m);
            }
        }
        msgs = std::move(merged);
    }

    j["messages"] = msgs;

    // Tools
    j["tools"] = json::parse(ToolDefsJson());
    j["tool_choice"] = "auto";

    return j.dump();
}

net::CurlHttpClient::Protocol App::ProtocolForActiveModel() {
    // Look up the active (provider, model) pair in the models.dev registry
    // and decide which wire protocol to use. Maps "zen"/"opencode-go" to
    // the "opencode" key in the registry where the actual model data lives.
    if (!registryLoaded_) return net::CurlHttpClient::Protocol::OpenAI;
    std::string registryKey = (activeProvider_ == "zen" || activeProvider_ == "opencode-go")
        ? "opencode" : activeProvider_;
    if (!modelsRegistry_.contains(registryKey) ||
        !modelsRegistry_[registryKey].is_object()) {
        return net::CurlHttpClient::Protocol::OpenAI;
    }
    const auto& p = modelsRegistry_[registryKey];
    std::string npm = p.value("npm", "");
    if (p.contains("models") && p["models"].is_object() &&
        p["models"].contains(activeModel_) &&
        p["models"][activeModel_].is_object()) {
        const auto& m = p["models"][activeModel_];
        if (m.contains("provider") && m["provider"].is_object() &&
            m["provider"].contains("npm") && m["provider"]["npm"].is_string()) {
            npm = m["provider"]["npm"].get<std::string>();
        }
    }
    return (npm == "@ai-sdk/anthropic")
        ? net::CurlHttpClient::Protocol::Anthropic
        : net::CurlHttpClient::Protocol::OpenAI;
}

std::string App::BuildAnthropicRequestJson() {
    ModelLimits limits = GetModelLimits();
    json j;
    j["model"] = activeModel_;
    j["stream"] = true;
    j["max_tokens"] = limits.maxOutput;

    // Same tool-result pruning rule as the OpenAI path: protect ~30K tokens
    // of recent tool output, replace older results with a placeholder.
    auto& hist = session_.History();
    int histSize = (int)hist.size();
    const size_t PROTECT_CHARS = 120000;
    size_t toolCharsAccum = 0;
    int pruneBeforeIdx = -1;
    for (int i = histSize - 1; i >= 0; i--) {
        if (hist[i].role == "tool") {
            toolCharsAccum += hist[i].content.size();
            if (toolCharsAccum > PROTECT_CHARS) { pruneBeforeIdx = i; break; }
        }
    }

    // System prompt — Anthropic takes it at the top level, not as a
    // messages[0] with role=system.
    {
        char cwdBuf[4096];
        std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
        j["system"] =
            "You are an AI coding assistant. You help the user with software engineering tasks "
            "by reading, writing, and editing files, and running shell commands.\n\n"
            "Working directory: " + cwd + "\n\n"
            "## Tools\n"
            "You have these tools: bash, read_file, write_file, edit_file, list_directory, "
            "web_fetch, web_search, grit_history_search, grit_history_fetch.\n"
            "- Use write_file to create new files or overwrite existing ones.\n"
            "- Use edit_file to make targeted changes — provide enough context in old_string to be unique.\n"
            "- Use read_file to read files before editing them.\n"
            "- Use bash to run commands, tests, install packages, etc.\n"
            "- Use grit_history_search whenever the user references any prior work in this or another "
            "project (\"last time\", \"once again\", \"we had\", \"in <project>\", \"how did we\"). It "
            "searches the transcripts of past gritcode conversations across all projects and returns "
            "short snippets. Pass 2-5 keywords. This is the authoritative source of prior-conversation "
            "recall — do not fall back to listing any ~/.claude/... memory directory.\n"
            "- Follow up with grit_history_fetch(session_id, turn_index) to read the full turn and "
            "surrounding context when a snippet looks relevant.\n\n"
            "## Behavior\n"
            "- When given a task, work through it step by step using your tools.\n"
            "- Do not just describe what you would do — actually do it by calling tools.\n"
            "- NEVER say 'Let me...' or 'I will...' without immediately calling the tools to do it.\n"
            "  Actions must ALWAYS be tool calls, never text descriptions of planned actions.\n"
            "- After each tool result, assess whether the task is complete and continue if not.\n"
            "  If you need to do more work, immediately call more tools. Do NOT stop and wait.\n"
            "- When making changes, verify they work (e.g., run tests or the build).\n"
            "- Keep responses concise. Lead with actions, not explanations.\n";
    }

    // Reshape grit's OpenAI-flavored history into Anthropic content-block form.
    // The mapping:
    //   role=user    → role=user, content = string
    //   role=assistant + text   → role=assistant, content = string
    //   role=assistant + tools  → role=assistant, content = [text?, tool_use...]
    //   role=tool               → role=user, content = [tool_result]
    // Anthropic requires alternating user/assistant roles, so consecutive
    // user-role messages (two tool_result turns back-to-back, or a
    // tool_result immediately followed by a real user prompt) must be merged
    // into a single user message with multiple content blocks.
    json msgs = json::array();
    auto pushOrMerge = [&](json msg) {
        if (!msgs.empty() && msgs.back()["role"] == msg["role"] &&
            msg["role"] == "user") {
            auto& last = msgs.back();
            // Normalize both sides to array form before concatenating.
            auto toArray = [](json& c) {
                if (c.is_string()) {
                    json arr = json::array();
                    arr.push_back({{"type","text"},{"text",c.get<std::string>()}});
                    c = std::move(arr);
                }
            };
            toArray(last["content"]);
            toArray(msg["content"]);
            for (auto& blk : msg["content"]) last["content"].push_back(blk);
        } else {
            msgs.push_back(std::move(msg));
        }
    };

    for (int i = 0; i < histSize; i++) {
        auto& m = hist[i];
        json msg;
        if (m.role == "user") {
            msg["role"] = "user";
            msg["content"] = m.content;

        } else if (m.role == "assistant") {
            msg["role"] = "assistant";
            if (m.toolCalls.empty()) {
                // Anthropic rejects empty assistant content — skip entirely.
                if (m.content.empty()) continue;
                msg["content"] = m.content;
            } else {
                json blocks = json::array();
                if (!m.content.empty()) {
                    blocks.push_back({{"type","text"},{"text",m.content}});
                }
                for (auto& tc : m.toolCalls) {
                    json tu;
                    tu["type"] = "tool_use";
                    tu["id"] = tc.value("id", "");
                    tu["name"] = tc.value("name", "");
                    try {
                        tu["input"] = json::parse(tc.value("arguments", std::string("{}")));
                    } catch (...) {
                        tu["input"] = json::object();
                    }
                    blocks.push_back(std::move(tu));
                }
                msg["content"] = std::move(blocks);
            }

        } else if (m.role == "tool") {
            msg["role"] = "user";
            json tr;
            tr["type"] = "tool_result";
            tr["tool_use_id"] = m.toolCallId;
            tr["content"] = (i <= pruneBeforeIdx)
                ? std::string("[Old tool result content cleared]")
                : m.content;
            json blocks = json::array();
            blocks.push_back(std::move(tr));
            msg["content"] = std::move(blocks);

        } else {
            continue;  // unknown role, drop
        }
        pushOrMerge(std::move(msg));
    }
    j["messages"] = std::move(msgs);

    // Tools — convert the canonical OpenAI-flavored ToolDefsJson() into
    // Anthropic's { name, description, input_schema } shape. Keeping one
    // source of truth avoids the two tool lists drifting.
    json openaiTools = json::parse(ToolDefsJson());
    json anthTools = json::array();
    for (auto& t : openaiTools) {
        if (!t.contains("function")) continue;
        json at;
        at["name"] = t["function"].value("name", "");
        at["description"] = t["function"].value("description", "");
        at["input_schema"] = t["function"].value("parameters", json::object());
        anthTools.push_back(std::move(at));
    }
    j["tools"] = std::move(anthTools);
    j["tool_choice"] = {{"type","auto"}};

    return j.dump();
}

void App::SaveAndIndex() {
    session_.Save();
    if (!memory_.IsOpen()) return;
    // RebuildSession: drop all existing rows for this session_id and re-insert
    // the current history. Cheaper than tracking deltas, idempotent across
    // compaction (where the head of history is replaced by a summary), and
    // cheap in absolute terms (a 500-turn rebuild is sub-100ms).
    auto now = []{
        time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return std::string(buf);
    }();
    memory_.RebuildSession(session_.SessionId(), session_.Cwd(),
                           session_.History(), now);
}

void App::SendMessage(const std::string& overrideText) {
    const bool fromOverride = !overrideText.empty();
    std::string msg = fromOverride ? overrideText : messageInput_.text;
    if (msg.empty() || !connected_) return;

    // Agent busy → append to the pending queue as a chip. The turn-completion
    // paths (Zen onComplete and ACP subprocess finalizer) auto-dispatch the
    // front of the queue. Escape cancel preserves the queue and flips to
    // idle-queue mode (Continue / Clear).
    if (requestInProgress_) {
        if (pendingQueue_.size() >= kMaxQueue_) return;
        pendingQueue_.push_back(msg);
        if (!fromOverride) messageInput_.Clear();
        LayoutWidgets();
        // Resize the scroll view since the chip row steals vertical space.
        float s = window_.Scale();
        int viewH = window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * s) - (int)chipRowHeight_;
        if (viewH < 1) viewH = 1;
        scrollView_.OnResize(window_.Width(), viewH);
        MarkDirty();
        return;
    }

    // Show in UI
    scrollView_.AppendStream(BlockType::USER_PROMPT, msg);
    if (!fromOverride) messageInput_.Clear();

    session_.History().push_back({"user", msg, {}, {}});
    SaveAndIndex();
    requestInProgress_ = true;
    toolRound_ = 0;

    waitingDotTimer_ = 0;
    waitingDotFrame_ = -1;
    // Recompute layout in case we transitioned out of idle-queue mode.
    LayoutWidgets();
    MarkDirty();

    retryCount_ = 0;  // user-initiated send resets retry counter
    DoSendToProvider();
}

void App::DispatchNextQueued() {
    if (pendingQueue_.empty()) return;
    std::string next = std::move(pendingQueue_.front());
    pendingQueue_.erase(pendingQueue_.begin());
    // Route through SendMessage() with an override so the user's in-progress
    // typing in messageInput_ is preserved — previously we used
    // messageInput_.text as a scratch variable, which clobbered whatever the
    // user was mid-typing when the turn ended.
    // Save and restore focus across the LayoutWidgets call below — if there
    // are still queued items left after the pop, LayoutWidgets would flip to
    // idleQueueMode (reqInProgress=false && queue non-empty) and unfocus the
    // input. SendMessage() flips reqInProgress=true so idle-queue mode is
    // gone for the actual request, but the focus side effect sticks.
    bool wasFocused = messageInput_.focused;
    LayoutWidgets();
    messageInput_.focused = wasFocused;
    float s = window_.Scale();
    int viewH = window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * s) - (int)chipRowHeight_;
    if (viewH < 1) viewH = 1;
    scrollView_.OnResize(window_.Width(), viewH);
    SendMessage(next);
}

void App::RemoveQueuedAt(size_t idx) {
    if (idx >= pendingQueue_.size()) return;
    pendingQueue_.erase(pendingQueue_.begin() + idx);
    LayoutWidgets();
    float s = window_.Scale();
    int viewH = window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * s) - (int)chipRowHeight_;
    if (viewH < 1) viewH = 1;
    scrollView_.OnResize(window_.Width(), viewH);
}

void App::ClickQueueChip(size_t idx) {
    if (idx >= pendingQueue_.size()) return;
    // Pull the chip's text into the input for editing. If the input already
    // has text, appending the existing text to the back of the queue is more
    // surprising than just replacing, so replace — the user clicked the chip
    // intentionally.
    std::string text = pendingQueue_[idx];
    pendingQueue_.erase(pendingQueue_.begin() + idx);
    messageInput_.text = text;
    messageInput_.cursorPos = (int)text.size();
    messageInput_.selStart = messageInput_.selEnd = 0;
    messageInput_.focused = true;
    LayoutWidgets();
    float s = window_.Scale();
    int viewH = window_.Height() - (int)((barHeight_ + inputHeight_ + chromeTopPad_) * s) - (int)chipRowHeight_;
    if (viewH < 1) viewH = 1;
    scrollView_.OnResize(window_.Width(), viewH);
}

// ---------------------------------------------------------------------------
// Context compaction
//
// Goal: when the full history would overflow the model's context window,
// replace the oldest portion with a real LLM-generated summary — the same
// strategy opencode uses (~/Developer/opencode/packages/opencode/src/session/
// compaction.ts). The previous grit implementation rewrote only the wire
// request while leaving session_.History() untouched; since history only
// grows, every subsequent request re-hit the same over-budget condition and
// re-"compacted" the same messages, spamming the UI with "📦 Context
// compacted" lines on every turn and never actually shrinking the working
// set. The fix persists the compaction in session_.History() and gates
// re-entry on a growth threshold.
//
// Flow:
//   1. MaybeCompactThenSend() estimates tokens. If within budget, calls
//      DoSendActualRequest() synchronously and returns.
//   2. If over budget, picks the most recent non-summary user message as the
//      split point. Head = history[0..split), tail = history[split..] stays
//      intact (we never break tool_use/tool_result pairs).
//   3. Fires an async summary request through the same httpClient_ with a
//      system prompt instructing the model to produce a faithful summary.
//      The current user sees a "📦 Compacting context…" notice in the UI so
//      the extra round-trip doesn't look like the app is hung.
//   4. ApplyCompaction() (on the main thread, after the summary returns)
//      replaces the head with a single user-role ChatMessage carrying
//      isSummary=true, persists to disk, and calls DoSendActualRequest().
//   5. historyCompactBaseCount_ is bumped to the post-compaction size so the
//      hysteresis check in step 1 won't re-compact until enough new messages
//      have accumulated.
// ---------------------------------------------------------------------------
void App::MaybeCompactThenSend() {
    auto& hist = session_.History();
    int histSize = (int)hist.size();

    // Hysteresis: require some minimum growth since last compaction before
    // we try again. Without this, repeated over-budget requests would each
    // fire a new compaction even though the post-compaction history hasn't
    // meaningfully grown.
    int growth = histSize - historyCompactBaseCount_;
    if (growth < 5 && historyCompactBaseCount_ > 0) {
        DoSendActualRequest();
        return;
    }

    // Rough token estimate: 4 chars ≈ 1 token. Same heuristic BuildRequestJson
    // used to carry — kept here so the compaction decision doesn't depend on
    // first serializing the whole wire body.
    size_t totalChars = 0;
    for (auto& m : hist) {
        totalChars += m.content.size() + m.reasoningContent.size();
        for (auto& tc : m.toolCalls) {
            if (tc.contains("arguments") && tc["arguments"].is_string())
                totalChars += tc["arguments"].get_ref<const std::string&>().size();
        }
    }
    // Add a fixed overhead for the system prompt + tool definitions.
    totalChars += 8000;
    int estTokens = (int)(totalChars / 4);

    ModelLimits limits = GetModelLimits();
    int threshold = limits.contextWindow - limits.maxOutput - 8000;
    if (threshold < 8000) threshold = 8000;

    DLOG("[COMPACT] est %d tokens, threshold %d, messages %d, base %d",
         estTokens, threshold, histSize, historyCompactBaseCount_);

    if (estTokens <= threshold) {
        DoSendActualRequest();
        return;
    }

    // Find the most recent non-summary user message — that's where the
    // current turn starts, and the head we can safely summarize ends right
    // before it. Since every user turn begins with role=user, splitting here
    // never leaves an orphan tool_use in the head or an orphan tool_result in
    // the tail.
    int splitIdx = -1;
    for (int i = histSize - 1; i >= 0; i--) {
        if (hist[i].role == "user" && !hist[i].isSummary) {
            splitIdx = i;
            break;
        }
    }
    if (splitIdx <= 0) {
        // Nothing older than the current turn to summarize.
        DoSendActualRequest();
        return;
    }

    RunSummaryThenResend(splitIdx);
}

void App::RunSummaryThenResend(int splitIdx) {
    compacting_ = true;
    auto& hist = session_.History();
    int headCount = splitIdx;

    AppendSystem("📦 Compacting context — summarizing " + std::to_string(headCount) +
                 " older messages before continuing. This usually takes a few seconds…");

    // Render the head as plain text. We feed the summary model a flat
    // transcript rather than replaying the raw tool_use / tool_result
    // structure, because (a) it works identically for any wire protocol and
    // (b) the summary model doesn't need the machine-readable tool shape —
    // it just needs to read what happened.
    std::string headText;
    headText.reserve(16384);
    for (int i = 0; i < splitIdx; i++) {
        auto& m = hist[i];
        headText += "--- ";
        if (m.isSummary) headText += "earlier summary";
        else headText += m.role;
        headText += " ---\n";
        if (!m.content.empty()) headText += m.content + "\n";
        for (auto& tc : m.toolCalls) {
            headText += "[tool call: " + tc.value("name", std::string("?")) + " ";
            headText += tc.value("arguments", std::string("{}"));
            headText += "]\n";
        }
        headText += "\n";
    }

    // Cap the summary-call input so the summary request itself doesn't
    // overflow. Budget = context window − response budget − prompt overhead.
    ModelLimits limits = GetModelLimits();
    size_t maxChars = (size_t)(limits.contextWindow - 6000) * 4;
    if (maxChars < 40000) maxChars = 40000;
    if (headText.size() > maxChars) {
        size_t drop = headText.size() - maxChars;
        headText = "[... much older history truncated to fit this summarization call ...]\n\n"
                   + headText.substr(drop);
    }

    const std::string summarySystem =
        "You are helping to compact a long coding-assistant conversation so it fits "
        "within the model's context window for future turns. Produce a detailed, "
        "faithful summary that preserves:\n"
        "  • The user's overall goal(s) and any sub-tasks.\n"
        "  • Concrete decisions made and their rationale.\n"
        "  • Files touched, functions edited, and the substance of each change.\n"
        "  • Results of commands run (build pass/fail, test outcomes, error messages).\n"
        "  • Open questions, blockers, and what should happen next.\n"
        "  • Any user preferences, constraints, or corrections given.\n"
        "Write a compact past-tense narrative. Do not invent details, do not add a "
        "sign-off, do not ask questions. Output only the summary.";

    std::string summaryUser =
        "Summarize this conversation so a fresh session can continue the work "
        "without re-reading it:\n\n" + headText;

    auto protocol = ProtocolForActiveModel();
    std::string reqJson;
    if (protocol == net::CurlHttpClient::Protocol::Anthropic) {
        json j;
        j["model"] = activeModel_;
        j["stream"] = true;
        j["max_tokens"] = 2500;
        j["system"] = summarySystem;
        j["messages"] = json::array({
            {{"role", "user"}, {"content", summaryUser}}
        });
        reqJson = j.dump();
    } else {
        json j;
        j["model"] = activeModel_;
        j["stream"] = true;
        j["max_tokens"] = 2500;
        j["messages"] = json::array({
            {{"role", "system"}, {"content", summarySystem}},
            {{"role", "user"},   {"content", summaryUser}}
        });
        reqJson = j.dump();
    }

    // Make sure the base URL is set for the current provider — DoSendToProvider
    // would normally do this, but we're intercepting before it runs.
    {
        std::string baseUrl = (activeProvider_ == "opencode-go")
            ? "https://opencode.ai/zen/go/v1"
            : "https://opencode.ai/zen/v1";
        if (!baseUrl.empty()) httpClient_.SetBaseUrl(baseUrl);
    }

    uint64_t gen = requestGen_.load();

    httpClient_.SendStreaming(protocol, reqJson,
        // onChunk — discarded; we don't surface partial summary text.
        [](const std::string&, bool){},
        // onComplete
        [this, gen, splitIdx, headCount](
            bool ok, const std::string& content, const std::string& error,
            const std::vector<json>&, const std::string&,
            int, int, const std::string&)
        {
            std::string summary = content;
            std::string err = error;
            bool success = ok && !summary.empty();
            events_.Push([this, gen, splitIdx, headCount, success, summary, err]() {
                if (gen != requestGen_.load()) {
                    // Cancelled (Esc) or superseded — drop the summary, leave
                    // history untouched. compacting_ must still be cleared so
                    // a future send isn't blocked.
                    compacting_ = false;
                    return;
                }
                ApplyCompaction(splitIdx, success, summary, err, headCount);
            });
        });
}

void App::ApplyCompaction(int splitIdx, bool success, const std::string& summary,
                          const std::string& error, int origHeadCount) {
    auto& hist = session_.History();
    // Guard: history could have mutated (unlikely on main thread, but safe).
    if (splitIdx <= 0 || splitIdx > (int)hist.size()) {
        compacting_ = false;
        DoSendActualRequest();
        return;
    }

    std::string summaryBody;
    if (success) {
        summaryBody =
            "[Prior conversation summary — the earlier turns have been compacted "
            "into this summary to fit the model's context window. Treat it as "
            "authoritative background for continuing the current task.]\n\n" +
            summary;
        AppendSystem("📦 Context compacted: " + std::to_string(origHeadCount) +
                     " older messages replaced by a summary.");
    } else {
        // Fallback — even a failed summary should shrink history, otherwise
        // the very next request hits the same overflow. Keep the hysteresis
        // floor so we don't immediately retry.
        summaryBody =
            "[Prior conversation context was dropped to fit the model's "
            "context window. Summary unavailable" +
            (error.empty() ? std::string("") : (": " + error)) + ".]";
        AppendSystem("⚠️ Compaction summary failed" +
                     (error.empty() ? std::string("") : (" (" + error + ")")) +
                     " — dropping " + std::to_string(origHeadCount) +
                     " older messages without a summary so the next request can fit.");
    }

    ChatMessage summaryMsg;
    summaryMsg.role = "user";
    summaryMsg.content = std::move(summaryBody);
    summaryMsg.isSummary = true;

    std::vector<ChatMessage> newHist;
    newHist.reserve(hist.size() - splitIdx + 1);
    newHist.push_back(std::move(summaryMsg));
    for (int i = splitIdx; i < (int)hist.size(); i++) {
        newHist.push_back(std::move(hist[i]));
    }
    hist = std::move(newHist);
    historyCompactBaseCount_ = (int)hist.size();
    session_.MarkDirty();
    SaveAndIndex();

    compacting_ = false;
    DoSendActualRequest();
}

void App::DoSendToProvider() {
    if (activeProvider_ == "claude") {
        // Fresh ACP turn — drop any per-turn tool streaming state from a
        // previous (possibly cancelled) run.
        acpToolBlockIdx_ = -1;
        acpToolInputBuf_.clear();
        acpToolNames_.clear();
        acpToolUseIdToName_.clear();

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
        std::string mcpConfig = claudeMcpConfigPath_;

        std::thread([this, prompt, model, gen, mcpConfig]() {
            int inPipe[2];   // parent -> child stdin
            int outPipe[2];  // child stdout -> parent
            if (CloexecPipe(inPipe) < 0) {
                events_.Push([this, gen]() {
                    if (gen != requestGen_.load()) return;
                    AppendSystem("Error: pipe failed");
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
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
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
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
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
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
                std::vector<const char*> argv = {
                    "claude",
                    "--print", "--verbose",
                    "--output-format", "stream-json",
                    "--include-partial-messages",
                    "--dangerously-skip-permissions",
                    "--model", model.c_str(),
                };
                // Suppress Claude Code's built-in auto-memory reflex
                // (~/.claude/projects/<cwd>/memory/). That system beats our
                // grit_history_search to the punch whenever the user asks
                // about prior-conversation context, and usually finds nothing
                // while grit_history_search has the real answer.
                static const char* kSuppressAutoMemory =
                    "IMPORTANT: Do NOT check, list, or use any "
                    "~/.claude/projects/.../memory/ directory — that "
                    "auto-memory system is disabled in this environment. "
                    "For any recall of prior conversations or past project "
                    "decisions, use the grit_history_search MCP tool "
                    "(mcp__grit-history__grit_history_search). It is the "
                    "authoritative source of prior-conversation context "
                    "across all the user's gritcode projects.";
                argv.push_back("--append-system-prompt");
                argv.push_back(kSuppressAutoMemory);
                // Register grit-history as a stdio MCP server for this spawn
                // only. We intentionally do NOT pass --strict-mcp-config:
                // users who've added their own MCPs (github, linear, etc.)
                // via `claude mcp add` should keep getting them here too.
                if (!mcpConfig.empty()) {
                    argv.push_back("--mcp-config");
                    argv.push_back(mcpConfig.c_str());
                }
                argv.push_back(nullptr);
                execvp("claude", const_cast<char* const*>(argv.data()));
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

                        if (evtType == "content_block_start" && evt.contains("content_block")) {
                            auto& cb = evt["content_block"];
                            int idx = evt.value("index", -1);
                            if (cb.value("type", "") == "tool_use") {
                                std::string tname = cb.value("name", "");
                                std::string tid = cb.value("id", "");
                                events_.Push([this, idx, tname, tid, gen]() {
                                    if (gen != requestGen_.load()) return;
                                    acpToolNames_[idx] = tname;
                                    acpToolUseIdToName_[tid] = tname;
                                    acpToolInputBuf_[idx] = "";

                                    // Finalize any in-flight assistant text so
                                    // markdown settles before the tool block
                                    // appears below it.
                                    if (!responseBuffer_.empty()) {
                                        RenderMarkdownToBlocks(true);
                                        responseBuffer_.clear();
                                        lastMarkdownLen_ = 0;
                                    }
                                    if (receivingThinking_) {
                                        receivingThinking_ = false;
                                    }

                                    std::string header = "Tool: " + tname + "\n";
                                    if (acpToolBlockIdx_ < 0) {
                                        scrollView_.AppendStream(BlockType::THINKING, header);
                                        acpToolBlockIdx_ = (int)scrollView_.BlockCount() - 1;
                                        scrollView_.StartThinking(acpToolBlockIdx_);
                                    } else {
                                        scrollView_.ContinueStream(header);
                                    }
                                    MarkDirty();
                                });
                            }
                        } else if (evtType == "content_block_stop") {
                            int idx = evt.value("index", -1);
                            events_.Push([this, idx, gen]() {
                                if (gen != requestGen_.load()) return;
                                auto nit = acpToolNames_.find(idx);
                                if (nit != acpToolNames_.end()) {
                                    // Tool_use block finished — parse the
                                    // accumulated input JSON and emit one line
                                    // per argument, matching Zen's layout.
                                    auto bit = acpToolInputBuf_.find(idx);
                                    std::string partial = (bit != acpToolInputBuf_.end()) ? bit->second : "";
                                    std::string argText;
                                    try {
                                        json args = json::parse(partial.empty() ? "{}" : partial);
                                        for (auto& [k, v] : args.items()) {
                                            argText += "  " + k + ": " + v.dump() + "\n";
                                        }
                                    } catch (...) {}
                                    if (!argText.empty() && acpToolBlockIdx_ >= 0) {
                                        scrollView_.ContinueStream(argText);
                                    }
                                    acpToolInputBuf_.erase(idx);
                                    acpToolNames_.erase(idx);
                                }
                                if (receivingThinking_) {
                                    receivingThinking_ = false;
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
                                        // Close any open tool-call block before
                                        // text streams in — tool round is over.
                                        if (acpToolBlockIdx_ >= 0) {
                                            scrollView_.StopThinking(acpToolBlockIdx_);
                                            acpToolBlockIdx_ = -1;
                                        }
                                        bool firstChunk = responseBuffer_.empty();
                                        if (receivingThinking_) {
                                            receivingThinking_ = false;
                                            firstChunk = true;
                                            responseBuffer_.clear();
                                            lastMarkdownLen_ = 0;
                                        }
                                        if (firstChunk) {
                                            scrollView_.StopAllAnimations();
                                            responseStartBlock_ = scrollView_.BlockCount();
                                        }
                                        responseBuffer_ += text;

                                        double now = GetMonotonicTime();
                                        if (now - lastMarkdownTime_ > 0.2 &&
                                            responseBuffer_.size() > lastMarkdownLen_ + 20) {
                                            RenderMarkdownToBlocks();
                                            lastMarkdownTime_ = now;
                                            lastMarkdownLen_ = responseBuffer_.size();
                                        }
                                        MarkDirty();
                                    });
                                }
                            } else if (deltaType == "input_json_delta") {
                                int idx = evt.value("index", -1);
                                std::string pj = delta.value("partial_json", "");
                                if (idx >= 0 && !pj.empty()) {
                                    events_.Push([this, idx, pj, gen]() {
                                        if (gen != requestGen_.load()) return;
                                        acpToolInputBuf_[idx] += pj;
                                    });
                                }
                            } else if (deltaType == "thinking_delta") {
                                std::string thinking = delta.value("thinking", "");
                                if (!thinking.empty()) {
                                    events_.Push([this, thinking, gen]() {
                                        if (gen != requestGen_.load()) return;
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
                    } else if (type == "user" && j.contains("message")) {
                        // tool_result content from the claude subprocess. Append
                        // each result to the current tool block (same visual
                        // shape as Zen's ExecuteToolCalls finalizer) and close
                        // the block's thinking animation.
                        auto& msg = j["message"];
                        if (msg.contains("content") && msg["content"].is_array()) {
                            for (auto& part : msg["content"]) {
                                if (part.value("type", "") != "tool_result") continue;
                                std::string tid = part.value("tool_use_id", "");
                                std::string out;
                                if (part.contains("content")) {
                                    auto& c = part["content"];
                                    if (c.is_string()) out = c.get<std::string>();
                                    else if (c.is_array()) {
                                        for (auto& seg : c) {
                                            if (seg.value("type", "") == "text")
                                                out += seg.value("text", "");
                                        }
                                    }
                                }
                                events_.Push([this, tid, out, gen]() {
                                    if (gen != requestGen_.load()) return;
                                    auto nit = acpToolUseIdToName_.find(tid);
                                    std::string tname = (nit != acpToolUseIdToName_.end()) ? nit->second : "";
                                    if (acpToolBlockIdx_ >= 0) {
                                        std::string resultText = "\n" + tname + ":\n" + StripAnsi(out) + "\n";
                                        scrollView_.ContinueStream(resultText);
                                        scrollView_.StopThinking(acpToolBlockIdx_);
                                        acpToolBlockIdx_ = -1;
                                    }
                                    acpToolUseIdToName_.erase(tid);
                                    MarkDirty();
                                });
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
                acpToolBlockIdx_ = -1;
                acpToolInputBuf_.clear();
                acpToolNames_.clear();
                acpToolUseIdToName_.clear();
                scrollView_.StopAllAnimations();
                if (!responseBuffer_.empty()) {
                    RenderMarkdownToBlocks(true);
                    responseBuffer_.clear();
                }
                if (!fullResponse.empty()) {
                    ChatMessage m;
                    m.role = "assistant";
                    m.content = fullResponse;
                    if (!reasoningBuffer_.empty())
                        m.reasoningContent = reasoningBuffer_;
                    session_.History().push_back(std::move(m));
                } else if (!reasoningBuffer_.empty()) {
                    ChatMessage m;
                    m.role = "assistant";
                    m.content = reasoningBuffer_;
                    m.reasoningContent = reasoningBuffer_;
                    session_.History().push_back(std::move(m));
                } else if (responseBuffer_.empty()) {
                    AppendSystemExpandable("Empty response from model",
                        "The Claude subprocess returned no content.\n\n"
                        "Provider: " + activeProvider_ + "\n"
                        "Model: " + activeModel_);
                }
                requestInProgress_ = false;
                sendButton_.enabled = true;
                session_.SetProvider(activeProvider_);
                session_.SetModel(activeModel_);
                SaveAndIndex();
                // Natural turn end: auto-dispatch the next queued message if
                // any BEFORE LayoutWidgets/refocus — otherwise LayoutWidgets
                // sees (reqInProgress=false, queue non-empty) and flips into
                // idle-queue mode, which hides+unfocuses the input. Dispatching
                // first keeps reqInProgress high across back-to-back turns so
                // the input stays visible and focused the whole time.
                // Error paths (pipe/fork/exec) fall through to idle-queue mode
                // where the user decides whether to Continue.
                if (!pendingQueue_.empty()) {
                    DispatchNextQueued();
                } else {
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
                }
                MarkDirty();
            });
        }).detach();
        return;
    }

    // Non-Claude path: run the compaction preflight. If the history is over
    // budget it fires an async summary LLM call; ApplyCompaction() rewrites
    // session_.History() in place and then calls DoSendActualRequest(). If
    // compaction is not needed, MaybeCompactThenSend() falls straight through
    // to DoSendActualRequest() synchronously.
    MaybeCompactThenSend();
}

void App::DoSendActualRequest() {
    // Zen / OpenCode Go provider: HTTP streaming. The wire protocol depends
    // on the active model, not the provider — opencode-go serves both
    // OpenAI-compatible and Anthropic-style models under the same base URL.

    // Ensure base URL matches the active provider (in case provider was
    // switched without reconnecting, or session restored a different provider)
    {
        std::string baseUrl = (activeProvider_ == "opencode-go")
            ? "https://opencode.ai/zen/go/v1"
            : "https://opencode.ai/zen/v1";
        if (activeProvider_ == "claude") baseUrl = "";  // Claude uses pipe, not HTTP
        if (!baseUrl.empty()) httpClient_.SetBaseUrl(baseUrl);
    }

    auto protocol = ProtocolForActiveModel();
    std::string requestJson = (protocol == net::CurlHttpClient::Protocol::Anthropic)
        ? BuildAnthropicRequestJson()
        : BuildRequestJson();

    responseBuffer_.clear();
    reasoningBuffer_.clear();
    receivingThinking_ = false;
    lastMarkdownLen_ = 0;
    lastMarkdownTime_ = GetMonotonicTime();

    waitingDotFrame_ = -1;
    waitingDotTimer_ = 0;

    httpClient_.SendStreaming(protocol, requestJson,
        // onChunk (bg thread)
        [this](const std::string& chunk, bool isThinking) {
            events_.Push([this, chunk, isThinking]() {
                if (isThinking) {
                    reasoningBuffer_ += chunk;  // Accumulate for history replay
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
                    if (now - lastMarkdownTime_ > 0.2 &&
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
               int, int, const std::string& rawBody) {
            events_.Push([this, ok, content, error, toolCalls, finishReason, rawBody]() {
                DLOG("[DEBUG-COMPLETE] ok=%d contentLen=%zu error='%s' toolCalls=%zu finishReason='%s' rawBodyLen=%zu",
                    ok, content.size(), error.c_str(), toolCalls.size(), finishReason.c_str(), rawBody.size());
                if (!ok) {
                    if (!session_.History().empty() && session_.History().back().role == "user")
                        session_.History().pop_back();
                    if (error.find("401") != std::string::npos) {
                        // Don't auto-clear the saved key: 401 can be transient
                        // (quota/rate-limit/billing blips on Zen) and a single
                        // failed request is not enough reason to nuke a secret
                        // the user may not have backed up anywhere else.
                        AppendSystemExpandable(
                            "Unauthorized (HTTP 401). Key was not cleared — retry, or replace it manually via the key button if it's really invalid.",
                            rawBody);
                        statusLabel_.text = "Unauthorized";
                    } else if (error.find("429") != std::string::npos) {
                        // Rate limit — auto-retry after the provider's suggested
                        // cooldown. The Zen gateway returns retry_after_seconds
                        // in the error JSON, but it's already been consumed by
                        // the SSE parser so we extract it from rawBody.
                        int waitSec = 10;  // default
                        try {
                            auto errJson = json::parse(rawBody);
                            if (errJson.contains("error") && errJson["error"].is_object()) {
                                auto& ej = errJson["error"];
                                if (ej.contains("metadata") && ej["metadata"].is_object())
                                    waitSec = ej["metadata"].value("retry_after_seconds", 10);
                            }
                        } catch (...) {}
                        if (waitSec < 2) waitSec = 2;
                        if (waitSec > 60) waitSec = 60;
                        if (retryCount_ < 3) {
                            retryCount_++;
                            DLOG("[DEBUG-429] rate limited, retry %d in %ds", retryCount_.load(), waitSec);
                            statusLabel_.text = "Rate limited, retrying in " + std::to_string(waitSec) + "s...";
                            MarkDirty();
                            // Schedule retry — can't sleep on the UI thread, so
                            // use the event loop with a timed push.
                            auto gen = requestGen_.load();
                            int finalWait = waitSec;
                            std::thread([this, gen, finalWait]() {
                                std::this_thread::sleep_for(std::chrono::seconds(finalWait));
                                // Only retry if the generation hasn't been bumped
                                // (user didn't cancel or send a new message)
                                if (requestGen_.load() == gen) {
                                    events_.Push([this, gen]() {
                                        if (requestGen_.load() == gen)
                                            DoSendToProvider();
                                    });
                                }
                            }).detach();
                            return;
                        }
                        AppendSystemExpandable("Rate limited (HTTP 429). Provider asks to wait " +
                            std::to_string(waitSec) + "s before retrying.", rawBody);
                        statusLabel_.text = "Rate limited";
                        retryCount_ = 0;
                    } else {
                        AppendSystemExpandable("Error: " + error, rawBody);
                    }
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    // Error/rate-limit path: DO NOT auto-dispatch the queue.
                    // Land in idle-queue mode so the user decides whether to
                    // Continue (retry the turn by resending the next chip)
                    // or Clear.
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
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

                // We used to run a substring-match heuristic here ("let me",
                // "i will", ...) to nudge the model back into tool-calling
                // when it stopped mid-task with planning text. That was
                // fragile in both directions: false positives nudged a model
                // that had legitimately finished and was waiting for user
                // feedback, and false negatives missed mid-task stops with
                // neutral phrasing. Opencode's prompt loop (see
                // ~/Developer/opencode/packages/opencode/src/session/prompt.ts)
                // trusts the model: if the turn ends without tool calls, the
                // turn is over. We follow the same rule — keep the two
                // unambiguous signals handled below (finish_reason=="tool_calls"
                // with no parsed tool_calls, and finish_reason=="length"), and
                // rely on the system prompt + user retrying for everything else.

                // Some providers (notably Z.AI GLM via the Zen gateway) may
                // return finish_reason="tool_calls" but the tool_call data
                // gets lost in transit — the SSE delta.tool_calls chunks
                // arrive but aren't parsed, or the upstream doesn't stream
                // them at all in some edge cases. Instead of showing a
                // confusing "Empty response from model" error, auto-retry
                // with a prompt that asks the model to continue.
                if (finishReason == "tool_calls" && toolCalls.empty() && toolRound_ < 40) {
                    DLOG("[DEBUG-COMPLETE] finish_reason=tool_calls but no tool calls parsed — auto-retrying");
                    if (!content.empty()) {
                        ChatMessage m;
                        m.role = "assistant";
                        m.content = content;
                        if (!reasoningBuffer_.empty())
                            m.reasoningContent = reasoningBuffer_;
                        session_.History().push_back(std::move(m));
                    } else if (!reasoningBuffer_.empty()) {
                        ChatMessage m;
                        m.role = "assistant";
                        m.content = "[thinking only — tool calls lost in transit]";
                        m.reasoningContent = reasoningBuffer_;
                        session_.History().push_back(std::move(m));
                    }
                    session_.History().push_back({"user", "[system: your previous response indicated you wanted to call a tool, but the tool call data was not received. Please try again — call the tool you intended to call.]", {}, {}});
                    toolRound_++;

                    // Render what we have so far
                    if (!responseBuffer_.empty()) {
                        RenderMarkdownToBlocks(true);
                        responseBuffer_.clear();
                    }
                    if (receivingThinking_) {
                        receivingThinking_ = false;
                        scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                    }

                    DoSendToProvider();
                    MarkDirty();
                    return;
                }

                // Model was cut off by token limit — it may have wanted to call tools.
                // Add what we got to history and auto-continue so it can keep going.
                if (finishReason == "length" && toolRound_ < 40) {
                    if (!content.empty()) {
                        ChatMessage m;
                        m.role = "assistant";
                        m.content = content;
                        if (!reasoningBuffer_.empty())
                            m.reasoningContent = reasoningBuffer_;
                        session_.History().push_back(std::move(m));
                    } else if (!reasoningBuffer_.empty()) {
                        ChatMessage m;
                        m.role = "assistant";
                        m.content = reasoningBuffer_;
                        m.reasoningContent = reasoningBuffer_;
                        session_.History().push_back(std::move(m));
                    }
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

                if (!content.empty()) {
                    ChatMessage m;
                    m.role = "assistant";
                    m.content = content;
                    if (!reasoningBuffer_.empty())
                        m.reasoningContent = reasoningBuffer_;
                    session_.History().push_back(std::move(m));
                } else if (!reasoningBuffer_.empty()) {
                    // Model sent only reasoning/thinking, no regular content.
                    // Save the reasoning as the message so the history stays
                    // coherent and we don't show a false "Empty response".
                    ChatMessage m;
                    m.role = "assistant";
                    m.content = reasoningBuffer_;
                    m.reasoningContent = reasoningBuffer_;
                    session_.History().push_back(std::move(m));
                } else if (responseBuffer_.empty()) {
                    std::string detail = rawBody;
                    if (detail.empty())
                        detail = "The server returned 200 OK but sent no content.\n\n"
                                 "Provider: " + activeProvider_ + "\n"
                                 "Model: " + activeModel_ + "\n"
                                 "Finish reason: " + finishReason;
                    AppendSystemExpandable("Empty response from model", detail);
                }
                requestInProgress_ = false;
                sendButton_.enabled = true;
                scrollView_.StopAllAnimations();
                session_.SetProvider(activeProvider_);
                session_.SetModel(activeModel_);
                SaveAndIndex();
                // Natural turn end. If there are queued messages, dispatch the
                // next one BEFORE LayoutWidgets/refocus — LayoutWidgets would
                // otherwise see (reqInProgress=false, queue non-empty) and flip
                // to idle-queue mode, hiding+unfocusing the input between turns.
                // Error paths return early above, so they fall through to real
                // idle-queue mode where the user decides whether to Continue.
                if (!pendingQueue_.empty()) {
                    DispatchNextQueued();
                } else {
                    LayoutWidgets();
                    RefocusMessageInputAfterTurn();
                }
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

    // Add assistant message with tool calls (include reasoning if any)
    ChatMessage assistantMsg;
    assistantMsg.role = "assistant";
    assistantMsg.content = content;
    assistantMsg.toolCalls = toolCalls;
    assistantMsg.reasoningContent = reasoningBuffer_;
    session_.History().push_back(std::move(assistantMsg));
    reasoningBuffer_.clear();  // Don't double-count in next round

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

    // Snapshot the current generation so we can cancel mid-run. Escape bumps
    // requestGen_ and synthesises tool_results inline, so the background
    // thread must not clobber history when it finishes after the fact.
    auto tcCopy = toolCalls;
    uint64_t gen = requestGen_.load();
    MemoryDB* mem = &memory_;
    std::string excludeSid = session_.SessionId();

    std::thread([this, tcCopy, gen, mem, excludeSid]() {
        struct Result { std::string id, output; };
        std::vector<Result> results;
        for (auto& tc : tcCopy) {
            // User cancelled while we were running the previous tool — stop
            // here, don't start another child process.
            if (requestGen_.load() != gen) break;
            results.push_back({
                tc.value("id", ""),
                ExecuteTool(tc.value("name", ""), tc.value("arguments", "{}"), mem, excludeSid)
            });
        }

        events_.Push([this, results, tcCopy, gen]() {
            // If Escape fired while this thread was running, history has
            // already been fixed up with synthetic tool_results and an
            // assistant cancel marker. Drop the results on the floor.
            if (requestGen_.load() != gen) {
                scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                MarkDirty();
                return;
            }

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
            SaveAndIndex();

            scrollView_.StopThinking(scrollView_.BlockCount() - 1);
            DoSendToProvider();  // Continue the loop
            MarkDirty();
        });
    }).detach();
}

void App::CancelInFlight() {
    if (!requestInProgress_) return;

    // Bump generation so any finalizer from the cancelled request is
    // dropped when it eventually arrives.
    ++requestGen_;
    httpClient_.Abort();
    // Claude ACP: signal the child process group so fgets unblocks and
    // the reader thread can finish and reap. Otherwise the detached
    // thread lives on holding a pipe to an orphaned claude child.
    pid_t cpid = claudePid_.exchange(-1);
    if (cpid > 0) kill(-cpid, SIGTERM);
    // Bash tool: if a RunCommand child is running, kill its whole group
    // so the read loop unblocks within ~100ms (short-poll interval).
    // The tool thread will see the bumped generation and drop its
    // result in the main-thread finalizer lambda.
    pid_t tpgid = g_toolPgid.exchange(0);
    if (tpgid > 0) kill(-tpgid, SIGKILL);
    requestInProgress_ = false;
    sendButton_.enabled = true;

    // History hygiene. Two cancel shapes we need to handle:
    //   a) Last entry is a user message — the model hadn't replied yet.
    //      Just mark as cancelled so the turn closes cleanly.
    //   b) Last entry is an assistant message with outstanding tool_calls —
    //      the model asked for tools and we were mid-run. The wire protocol
    //      requires every tool_use to be followed by a matching tool_result
    //      before any other turn, so synthesise a [cancelled by user]
    //      tool_result for each pending id and only then append the
    //      "[cancelled]" assistant marker. Without this, the next user send
    //      builds a request with a dangling tool_use and the API returns 400.
    if (!session_.History().empty()) {
        auto& last = session_.History().back();
        if (last.role == "assistant" && !last.toolCalls.empty()) {
            auto pendingCalls = last.toolCalls;  // copy: we're about to mutate
            for (auto& tc : pendingCalls) {
                std::string id = tc.value("id", "");
                session_.History().push_back({"tool", "[cancelled by user]", {}, id});
            }
            session_.History().push_back({"assistant", "[cancelled]", {}, {}});
        } else if (last.role == "user") {
            session_.History().push_back({"assistant", "[cancelled]", {}, {}});
        }
    }
    SaveAndIndex();
    scrollView_.StopAllAnimations();
    receivingThinking_ = false;
    responseBuffer_.clear();
    AppendSystem("Cancelled");
    // Preserve pendingQueue_ — the user may have queued course-correction
    // messages while the agent was going sideways. We land in idle-queue mode
    // (Continue / Clear buttons visible) so they decide what happens next.
    LayoutWidgets();
    RefocusMessageInputAfterTurn();
    MarkDirty();
}

// ============================================================================
// Input handling
// ============================================================================

void App::OnMouseDown(float x, float y, bool shift) {
    // One-shot: if the window just gained focus, this mousedown is the click
    // that raised it. Keep the message input focused even if it lands in the
    // scroll view — otherwise the first click into the transcript after an
    // alt-tab would unfocus the box the focus-gained handler just focused.
    bool focusGainRedirect = redirectNextClickToInput_;
    redirectNextClickToInput_ = false;
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
            window_.SetTitle(CollapseHome(cwd).c_str());
            RestoreSessionToView();
            StartConnect();
        }
        MarkDirty();
        return;
    }

    // All coords are physical pixels from WaylandWindow

    // Dropdowns: only one can be open at a time. If any popup is open and
    // the click landed inside it, let that dropdown handle it (select item
    // or dismiss). Otherwise close all and let the click fall through —
    // including the case where it falls onto a different dropdown's
    // closed bar, which would otherwise open a second dropdown while the
    // first stayed visible.
    Dropdown* dds[] = {&workspaceDropdown_, &providerDropdown_, &modelDropdown_};
    Dropdown* openDd = nullptr;
    for (auto* d : dds) if (d->open) { openDd = d; break; }
    if (openDd) {
        WidgetRect pr = openDd->PopupRect();
        if (PointInRect(x, y, pr)) {
            openDd->OnMouseDown(x, y);
            MarkDirty();
            return;
        }
        bool clickedOwnBar = PointInRect(x, y, openDd->bounds);
        openDd->Close();
        // Clicking the bar of an already-open dropdown should toggle it
        // closed. Without this early return the for-loop below would
        // immediately re-open it on the same click.
        if (clickedOwnBar) {
            MarkDirty();
            return;
        }
    }
    for (auto* d : dds) {
        if (PointInRect(x, y, d->bounds)) {
            d->OnMouseDown(x, y);
            MarkDirty();
            return;
        }
    }

    // Queue chips: × badge removes; clicking the chip body pulls the text
    // back into the message input for editing (only when the input is
    // actually visible — in idle-queue mode it's hidden, so a body-click
    // implicitly takes us out of that mode via the queue becoming empty or
    // the user hitting Continue).
    for (auto& hit : queueChipHits_) {
        if (PointInRect(x, y, hit.closeBtn)) {
            RemoveQueuedAt(hit.idx);
            MarkDirty();
            return;
        }
        // Body-click pulls the chip text back into the input for editing.
        // Only meaningful while the input is visible (i.e., agent busy); in
        // idle-queue mode the input is hidden and the user's options are
        // Continue / Clear / ×, so body clicks are no-ops there.
        if (messageInput_.visible && PointInRect(x, y, hit.bodyHit)) {
            ClickQueueChip(hit.idx);
            MarkDirty();
            return;
        }
    }

    if (sendButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (continueQueueButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (clearQueueButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
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
            dragTarget_ = (inp == &apiKeyInput_) ? DragTarget::ApiKeyInput
                                                 : DragTarget::MessageInput;
            MarkDirty();
            return;
        }
    }

    // Scroll view
    float s = window_.Scale();
    float viewH = window_.Height() - (barHeight_ + inputHeight_ + chromeTopPad_) * s - chipRowHeight_;
    if (y < viewH) {
        scrollView_.OnMouseDown(x, y, shift);
        dragTarget_ = DragTarget::ScrollView;
        if (!focusGainRedirect) {
            messageInput_.focused = false;
            apiKeyInput_.focused = false;
        }
        MarkDirty();
    }
}

void App::OnMouseUp(float x, float y) {
    sendButton_.OnMouseUp(x, y);
    continueQueueButton_.OnMouseUp(x, y);
    clearQueueButton_.OnMouseUp(x, y);
    if (apiKeyEditing_) { apiKeyAccept_.OnMouseUp(x, y); apiKeyCancel_.OnMouseUp(x, y); }
    else apiKeyButton_.OnMouseUp(x, y);
    scrollView_.OnMouseUp(x, y);
    dragTarget_ = DragTarget::None;
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
    // Route drag to the widget the drag started on — not to whichever input
    // happens to be focused. Otherwise clicking in the transcript would start
    // a message-input selection instead of selecting text in the transcript.
    if (leftDown && dragTarget_ != DragTarget::None) {
        if (dragTarget_ == DragTarget::MessageInput) {
            messageInput_.OnMouseDrag(x, y, scrollView_.Fonts());
        } else if (dragTarget_ == DragTarget::ApiKeyInput) {
            apiKeyInput_.OnMouseDrag(x, y, scrollView_.Fonts());
        } else if (dragTarget_ == DragTarget::ScrollView) {
            scrollView_.OnMouseMove(x, y, leftDown);
        }
        MarkDirty();
        return;
    }

    // Only mark dirty when a widget's hover state actually changes — otherwise
    // every pointer-motion event would force a redraw and peg a CPU core.
    // ScrollView manages its own needsRedraw_ which the main loop checks.
    bool changed = false;
    changed |= sendButton_.OnMouseMove(x, y);
    if (apiKeyEditing_) {
        changed |= apiKeyAccept_.OnMouseMove(x, y);
        changed |= apiKeyCancel_.OnMouseMove(x, y);
    } else {
        changed |= apiKeyButton_.OnMouseMove(x, y);
    }
    changed |= continueQueueButton_.OnMouseMove(x, y);
    changed |= clearQueueButton_.OnMouseMove(x, y);
    changed |= workspaceDropdown_.OnMouseMove(x, y);
    changed |= providerDropdown_.OnMouseMove(x, y);
    changed |= modelDropdown_.OnMouseMove(x, y);

    float s = window_.Scale();
    float viewH = window_.Height() - (barHeight_ + inputHeight_ + chromeTopPad_) * s - chipRowHeight_;
    if (y < viewH) {
        scrollView_.OnMouseMove(x, y, leftDown);
    }
    if (changed) MarkDirty();
}

void App::OnScroll(float delta) {
    if (chooserMode_) {
        chooserScroll_ -= delta * 40;
        if (chooserScroll_ < 0) chooserScroll_ = 0;
        MarkDirty();
        return;
    }
    // Scroll inside open dropdown popups
    if (modelDropdown_.open) { modelDropdown_.OnScroll(delta); MarkDirty(); return; }
    if (providerDropdown_.open) { providerDropdown_.OnScroll(delta); MarkDirty(); return; }
    if (workspaceDropdown_.open) { workspaceDropdown_.OnScroll(delta); MarkDirty(); return; }
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
                window_.SetTitle(CollapseHome(cwd).c_str());
                PopulateWorkspaceDropdown();
                RestoreSessionToView();
                StartConnect();
            }
            MarkDirty();
        } else if (key == XKB_KEY_Escape) {
            window_.SetShouldClose(true);
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
        CancelInFlight();
        return;
    }

    // Ctrl+C: copy from focused input, or scroll view
    if ((mods & Mod::Ctrl) && key == Key::C) {
        auto* inp = FocusedInput();
        if (inp && inp->selStart != inp->selEnd) {
            std::string sel = inp->GetSelectedText();
            if (!sel.empty()) {
                Clipboard::Copy(sel);
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
    window_.SetTitle(CollapseHome(dir).c_str());
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
    window_.SetTitle(CollapseHome(dir).c_str());
    RestoreSessionToView();
    StartConnect();
    MarkDirty();
}

void App::Run() {
    double lastTime = GetMonotonicTime();

    while (!window_.ShouldClose()) {
        bool needsAnim = scrollView_.HasActiveThinking() || waitingDotFrame_ >= 0;
        if (!dirty_ && events_.Empty() && !scrollView_.NeedsRedraw() && !window_.NeedsRedraw()) {
            if (needsAnim)
                window_.WaitEventsTimeout(0.1);
            else
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
            // Auto-scroll open dropdowns when hovering near edges
            if (modelDropdown_.open && modelDropdown_.autoScrollSpeed != 0) { modelDropdown_.Update(dt); MarkDirty(); }
            if (providerDropdown_.open && providerDropdown_.autoScrollSpeed != 0) { providerDropdown_.Update(dt); MarkDirty(); }
            if (workspaceDropdown_.open && workspaceDropdown_.autoScrollSpeed != 0) { workspaceDropdown_.Update(dt); MarkDirty(); }
        }

        bool showDots = requestInProgress_
                        && !scrollView_.HasActiveThinking();
        if (showDots) {
            if (waitingDotFrame_ < 0) {
                waitingDotFrame_ = 0;
                waitingDotTimer_ = 0;
                MarkDirty();
            } else {
                waitingDotTimer_ += dt;
                static constexpr float DOT_INTERVAL = 0.25f;  // 4fps
                if (waitingDotTimer_ >= DOT_INTERVAL) {
                    waitingDotTimer_ = 0;
                    waitingDotFrame_ = (waitingDotFrame_ + 1) % 3;
                    MarkDirty();
                }
            }
        } else if (waitingDotFrame_ >= 0) {
            waitingDotFrame_ = -1;
            MarkDirty();
        }

        if (!dirty_ && !scrollView_.NeedsRedraw() && !window_.NeedsRedraw()) continue;
        dirty_ = false;
        scrollView_.ClearDirty();

        // Drain again in case events arrived during update
        events_.Drain();

        // Render
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        auto frame = window_.BeginContentFrame();
        renderer_.BeginFrame(frame.width, frame.height, scrollView_.Fonts());

        if (chooserMode_) {
            PaintChooser();
        } else {
            // Scroll view (top portion)
            scrollView_.Paint(renderer_);

            // Waiting dots (plain, below last block)
            if (waitingDotFrame_ >= 0) {
                auto& fm = scrollView_.Fonts();
                float dotR = fm.LineHeight(FontStyle::Regular) * 0.15f;
                float spacing = dotR * 3;
                float baseX = 20;
                float baseY = scrollView_.ContentBottom() + fm.LineHeight(FontStyle::Regular);
                for (int d = 0; d < 3; d++) {
                    float alpha = (d == waitingDotFrame_) ? 1.0f : 0.3f;
                    Color c{0.5f, 0.5f, 0.5f, alpha};
                    float cx = baseX + d * spacing;
                    renderer_.DrawRect(cx - dotR, baseY - dotR, dotR * 2, dotR * 2, c);
                }
            }

            // Bottom bar + input
            PaintBottomBar();
        }

        renderer_.EndFrame();
        window_.EndContentFrame();

        // Perf tracking
        clock_gettime(CLOCK_MONOTONIC, &t1);
        static int frameCount = 0;
        static long totalUs = 0;
        static double perfStart = now;
        long frameUs = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        totalUs += frameUs;
        frameCount++;
        if (now - perfStart > 2.0 && frameCount > 0) {
            FILE* pf = fopen("/tmp/grit-perf.log", "a");
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
