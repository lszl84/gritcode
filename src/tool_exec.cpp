// Gritcode — tool execution. Lifted from the static helpers in app.cpp.
// Kept self-contained: no App state, no event queue, no UI. Callers are
// expected to invoke ExecuteTool from a worker thread and marshal the
// returned string back to their UI thread themselves.

#include "tool_exec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

namespace toolexec {

std::atomic<pid_t> g_toolPgid{0};

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

static std::string RunCommand(const std::string& cmd) {
    int pfd[2];
    if (CloexecPipe(pfd) < 0) return "Error: pipe failed";

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return "Error: fork failed"; }

    if (pid == 0) {
        setsid();
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/bash", "bash", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    close(pfd[1]);
    g_toolPgid.store(pid);

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
            remainingMs = 200;
        }

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
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (reaped) break;
    }

    close(pfd[0]);

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

static std::string ExpandTilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return home + path.substr(1);
    return path;
}

static std::string WriteFileT(const std::string& path, const std::string& content) {
    std::string expanded = ExpandTilde(path);
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

static std::string EditFileT(const std::string& path, const std::string& oldStr, const std::string& newStr) {
    std::string expanded = ExpandTilde(path);
    std::ifstream in(expanded);
    if (!in) return "Error: cannot read " + expanded;
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = contents.find(oldStr);
    if (pos == std::string::npos) return "Error: old_string not found in " + expanded;
    if (contents.find(oldStr, pos + 1) != std::string::npos)
        return "Error: old_string is not unique in " + expanded + " — provide more context";
    contents.replace(pos, oldStr.size(), newStr);

    std::ofstream out(expanded);
    if (!out) return "Error: cannot write " + expanded;
    out << contents;
    out.close();
    return "Applied edit to " + expanded;
}

static std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

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

static void RemoveHtmlBlock(std::string& s, const std::string& tag) {
    std::string openPrefix = "<" + tag;
    std::string closeTag = "</" + tag + ">";
    size_t pos = 0;
    while (true) {
        size_t p = FindICase(s, openPrefix, pos);
        if (p == std::string::npos) break;
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

static std::string StripHtml(const std::string& html) {
    std::string s = html;
    RemoveHtmlBlock(s, "script");
    RemoveHtmlBlock(s, "style");
    RemoveHtmlBlock(s, "head");
    RemoveHtmlBlock(s, "noscript");
    RemoveHtmlBlock(s, "svg");

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
    size_t a = out.find_first_not_of(" \n\t");
    size_t b = out.find_last_not_of(" \n\t");
    if (a == std::string::npos) return "";
    return out.substr(a, b - a + 1);
}

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
        }
    }
    return combined;
}

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

std::string ExecuteTool(const std::string& name, const std::string& argsJson) {
    try {
        auto args = json::parse(argsJson);
        if (name == "bash")           return RunCommand(args.value("command", ""));
        if (name == "read_file")      return RunCommand("cat -n -- '" + ExpandTilde(args.value("path", "")) + "'");
        if (name == "list_directory") return RunCommand("ls -la -- '" + ExpandTilde(args.value("path", ".")) + "'");
        if (name == "write_file")     return WriteFileT(args.value("path", ""), args.value("content", ""));
        if (name == "edit_file")      return EditFileT(args.value("path", ""), args.value("old_string", ""), args.value("new_string", ""));
        if (name == "web_fetch")      return WebFetch(args.value("url", ""));
        if (name == "web_search")     return WebSearch(args.value("query", ""));
    } catch (...) {}
    return "Error: unknown tool " + name;
}

std::string StripAnsi(const std::string& s) {
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

std::string ToolDefsJson() {
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
        {"name","web_search"},{"description","Search the web and return result titles, URLs, and snippets as plain text. Use to find current information, documentation, or pages to follow up on with web_fetch."},
        {"parameters",{{"type","object"},{"properties",{
            {"query",{{"type","string"},{"description","Search query"}}}
        }},{"required",json::array({"query"})}}}
    }}});
    return tools.dump();
}

void KillRunningTool() {
    pid_t pgid = g_toolPgid.load();
    if (pgid > 0) kill(-pgid, SIGKILL);
}

} // namespace toolexec
