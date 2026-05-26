#include "tools.h"
#include "format_u8.h"
#include "memory.h"
#include "streaming_web_request.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxOutput = 30000;

std::string CapOutput(std::string s) {
    if (s.size() > kMaxOutput) {
        s.resize(kMaxOutput);
        s += "\n[output truncated]";
    }
    return s;
}

std::string GetStringArg(const nlohmann::json& args, const char* key) {
    if (!args.is_object() || !args.contains(key)) return {};
    const auto& v = args[key];
    if (!v.is_string()) return {};
    return v.get<std::string>();
}

// ---------------- file/shell tools ----------------

std::string ToolReadFile(const nlohmann::json& args) {
    std::string path = GetStringArg(args, "path");
    if (path.empty()) return "Error: missing 'path' argument";

    std::ifstream f(path);
    if (!f) return "Error: cannot open " + path;

    std::ostringstream out;
    std::string line;
    int n = 1;
    while (std::getline(f, line)) {
        out << n++ << '\t' << line << '\n';
        if (out.tellp() > (std::streamoff)kMaxOutput) {
            out << "[truncated at line " << (n - 1) << "]\n";
            break;
        }
    }
    std::string s = out.str();
    if (s.empty()) s = "(empty file)";
    return s;
}

std::string ToolWriteFile(const nlohmann::json& args) {
    std::string path = GetStringArg(args, "path");
    if (path.empty()) return "Error: missing 'path' argument";
    if (!args.contains("content") || !args["content"].is_string())
        return "Error: missing 'content' argument";
    std::string content = args["content"].get<std::string>();

    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) return "Error: mkdir failed: " + ec.message();
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return "Error: cannot open " + path + " for writing";
    f.write(content.data(), (std::streamsize)content.size());
    if (!f) return "Error: write failed for " + path;
    return "Wrote " + std::to_string(content.size()) + " bytes to " + path;
}

std::string ToolEditFile(const nlohmann::json& args) {
    std::string path = GetStringArg(args, "path");
    std::string oldStr = GetStringArg(args, "old_str");
    std::string newStr = GetStringArg(args, "new_str");
    if (path.empty()) return "Error: missing 'path' argument";
    if (oldStr.empty()) return "Error: 'old_str' must not be empty";

    std::ifstream in(path, std::ios::binary);
    if (!in) return "Error: cannot open " + path;
    std::stringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();
    in.close();

    size_t pos = content.find(oldStr);
    if (pos == std::string::npos)
        return "Error: 'old_str' not found in " + path;
    if (content.find(oldStr, pos + oldStr.size()) != std::string::npos)
        return "Error: 'old_str' is not unique in " + path
             + " (matches multiple times — make it more specific)";

    content.replace(pos, oldStr.size(), newStr);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "Error: cannot write " + path;
    out.write(content.data(), (std::streamsize)content.size());
    if (!out) return "Error: write failed for " + path;
    return "Edited " + path;
}

// fork+exec instead of popen so we can put the child in its own process group
// and SIGTERM the whole group on Escape. popen() doesn't expose the child PID
// and can't be interrupted from another thread. The pipe is O_CLOEXEC so the
// child's exec'd program doesn't inherit our fd.
std::string ToolBash(const nlohmann::json& args, ToolCancelToken* token) {
    std::string cmd = GetStringArg(args, "command");
    if (cmd.empty()) return "Error: missing 'command' argument";
    if (token && token->cancelled.load()) return "[cancelled]";

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) return "Error: pipe failed";

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return "Error: fork failed";
    }
    if (pid == 0) {
        // child: become its own process group leader so the parent can signal
        // the whole tree (timeout → bash → user command) with one kill(-pgid).
        setpgid(0, 0);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        // pfd[0]/pfd[1] are O_CLOEXEC so they auto-close on exec.
        execlp("timeout", "timeout", "30", "bash", "-c", cmd.c_str(),
               (char*)nullptr);
        _exit(127);
    }

    // Parent: race-resolve setpgid (either side wins, the other gets EACCES
    // or succeeds idempotently). Then publish the pgid so Escape can find it.
    setpgid(pid, pid);
    if (token) token->activePgid.store(pid);
    close(pfd[1]);

    // Loop terminates on direct-child exit, NOT on pipe EOF. Bash commands
    // that disown setsid'd grandchildren (e.g. `setsid foo &>/dev/null &
    // disown`) leave the pipe write end open in those grandchildren forever;
    // gating on pipe EOF would hang us in poll() indefinitely. Once the
    // direct child exits we drain whatever's still queued and stop.
    auto append = [&](const char* data, size_t n, std::string& out, bool& truncated) {
        if (out.size() >= kMaxOutput) { truncated = true; return; }
        out.append(data, n);
        if (out.size() > kMaxOutput) { out.resize(kMaxOutput); truncated = true; }
    };

    std::string out;
    bool truncated = false;
    bool sentTerm = false;
    bool sentKill = false;
    bool pipeEof = false;
    int status = 0;
    char buf[4096];
    auto killDeadline = std::chrono::steady_clock::time_point::max();

    for (;;) {
        if (token && token->cancelled.load() && !sentTerm) {
            kill(-pid, SIGTERM);
            sentTerm = true;
            killDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        if (sentTerm && !sentKill && std::chrono::steady_clock::now() >= killDeadline) {
            kill(-pid, SIGKILL);
            sentKill = true;
        }

        if (!pipeEof) {
            struct pollfd p{pfd[0], POLLIN, 0};
            int pr = poll(&p, 1, 100);
            if (pr < 0 && errno != EINTR) {
                pipeEof = true;
            } else if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(pfd[0], buf, sizeof(buf));
                if (n > 0) append(buf, (size_t)n, out, truncated);
                else if (n == 0) pipeEof = true;
                else if (errno != EINTR && errno != EAGAIN) pipeEof = true;
            }
        } else {
            // Pipe is dead, just wait for the child without busy-spinning.
            struct timespec ts{0, 50'000'000};  // 50 ms
            nanosleep(&ts, nullptr);
        }

        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            // Direct child gone. Final non-blocking drain of anything still
            // sitting in the kernel buffer for us, then exit.
            if (!pipeEof) {
                int flags = fcntl(pfd[0], F_GETFL, 0);
                fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
                for (;;) {
                    ssize_t n = read(pfd[0], buf, sizeof(buf));
                    if (n <= 0) break;
                    append(buf, (size_t)n, out, truncated);
                }
            }
            break;
        }
    }
    close(pfd[0]);
    if (token) token->activePgid.store(0);

    if (token && token->cancelled.load()) return "[cancelled]";

    if (truncated) out += "\n[output truncated]";
    if (out.empty()) out = "(no output)";

    // `timeout` exits 124 when it kills the child.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 124)
        out += "\n[timed out after 30s]";
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        out += "\n[exit " + std::to_string(WEXITSTATUS(status)) + "]";
    else if (WIFSIGNALED(status))
        out += "\n[killed by signal " + std::to_string(WTERMSIG(status)) + "]";

    return out;
}

std::string ToolListDirectory(const nlohmann::json& args) {
    std::string path = GetStringArg(args, "path");
    if (path.empty()) path = ".";

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) return "Error: " + path + " does not exist";
    if (!fs::is_directory(path, ec)) return "Error: " + path + " is not a directory";

    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        if (ec) break;
        std::string name = e.path().filename().string();
        bool isDir = e.is_directory(ec);
        std::string line;
        line += isDir ? 'd' : '-';
        line += ' ';
        line += name;
        if (isDir) line += '/';
        else {
            std::error_code se;
            auto sz = e.file_size(se);
            if (!se) line += "  " + std::to_string(sz) + " B";
        }
        entries.push_back(std::move(line));
    }
    if (ec) return "Error: " + ec.message();

    std::sort(entries.begin(), entries.end());
    std::string out;
    for (auto& s : entries) { out += s; out += '\n'; }
    if (out.empty()) out = "(empty directory)";
    return CapOutput(std::move(out));
}

// ---------------- web tools ----------------

// HTTP for the web tools is a blocking call on the tool worker thread (tools
// already run off the GUI thread, so blocking is fine and there's no nested
// event loop to juggle). The defaults below match what a browser would send
// so search engines and CDNs don't 403 on a missing UA.
WebRequestSpec MakeWebSpec(const std::string& url) {
    WebRequestSpec spec;
    spec.url = url;
    spec.headers.push_back({"User-Agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36 wx_gritcode"});
    spec.headers.push_back({"Accept", "*/*"});
    return spec;
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string DecodeEntities(std::string s) {
    auto repl = [](std::string& s, const std::string& f, const std::string& r) {
        size_t p = 0;
        while ((p = s.find(f, p)) != std::string::npos) {
            s.replace(p, f.size(), r);
            p += r.size();
        }
    };
    repl(s, "&amp;", "&");
    repl(s, "&lt;", "<");
    repl(s, "&gt;", ">");
    repl(s, "&quot;", "\"");
    repl(s, "&#39;", "'");
    repl(s, "&apos;", "'");
    repl(s, "&nbsp;", " ");
    repl(s, "&mdash;", "—");
    repl(s, "&ndash;", "–");
    repl(s, "&hellip;", "…");
    return s;
}

std::string StripHtml(const std::string& html) {
    std::string s = html;
    std::string lower = ToLower(s);
    auto removeBlock = [&](const std::string& tag) {
        const std::string open = "<" + tag;
        const std::string close = "</" + tag + ">";
        size_t p = 0;
        while ((p = lower.find(open, p)) != std::string::npos) {
            size_t after = p + open.size();
            if (after < lower.size() && lower[after] != ' '
                && lower[after] != '>' && lower[after] != '\t'
                && lower[after] != '\n') {
                p = after;
                continue;
            }
            size_t end = lower.find(close, after);
            if (end == std::string::npos) {
                s.erase(p);
                lower.erase(p);
                break;
            }
            size_t cnt = end + close.size() - p;
            s.erase(p, cnt);
            lower.erase(p, cnt);
        }
    };
    removeBlock("script");
    removeBlock("style");
    removeBlock("noscript");

    auto replaceBlocks = [](std::string& s) {
        const char* blocks[] = {
            "</p>", "</div>", "</li>", "</h1>", "</h2>", "</h3>",
            "</h4>", "</h5>", "</h6>", "</tr>", "<br>", "<br/>", "<br />"
        };
        for (auto* tag : blocks) {
            std::string lower(tag);
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
            size_t p = 0;
            std::string slow = s;
            for (auto& c : slow) c = (char)std::tolower((unsigned char)c);
            while ((p = slow.find(lower, p)) != std::string::npos) {
                s.replace(p, lower.size(), "\n");
                slow.replace(p, lower.size(), "\n");
                p += 1;
            }
        }
    };
    replaceBlocks(s);

    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) out += c;
    }

    out = DecodeEntities(std::move(out));

    std::string compact;
    compact.reserve(out.size());
    int spaces = 0;
    int newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            spaces = 0;
            ++newlines;
            if (newlines <= 2) compact += '\n';
        } else if (c == ' ' || c == '\t' || c == '\r') {
            ++spaces;
            newlines = 0;
            if (spaces == 1 && !compact.empty() && compact.back() != '\n')
                compact += ' ';
        } else {
            spaces = 0;
            newlines = 0;
            compact += c;
        }
    }
    return compact;
}

std::string ToolWebFetch(const nlohmann::json& args) {
    std::string url = GetStringArg(args, "url");
    if (url.empty()) return "Error: missing 'url' argument";
    if (url.find("://") == std::string::npos) url = "https://" + url;

    auto r = RequestSync(MakeWebSpec(url));
    if (!r.ok) return "Error: " + r.error;

    std::string text;
    if (r.contentType.find("html") != std::string::npos
        || (r.contentType.empty() && r.body.find("<html") != std::string::npos))
        text = StripHtml(r.body);
    else
        text = r.body;

    return CapOutput(std::move(text));
}

// Web search via Exa MCP — same backend opencode uses. POST a JSON-RPC
// `tools/call` to https://mcp.exa.ai/mcp; response is SSE-formatted with a
// single `data: {...}` line containing the result text. Works
// unauthenticated against the public endpoint; an EXA_API_KEY env var is
// honored if set.
std::string ToolWebSearch(const nlohmann::json& args) {
    std::string q = GetStringArg(args, "query");
    if (q.empty()) return "Error: missing 'query' argument";

    std::string url = "https://mcp.exa.ai/mcp";
    if (const char* key = std::getenv("EXA_API_KEY"); key && *key) {
        url += "?exaApiKey=" + UrlEncode(key);
    }

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {
            {"name", "web_search_exa"},
            {"arguments", {
                {"query", q},
                {"type", "auto"},
                {"numResults", 8},
                {"livecrawl", "fallback"},
            }},
        }},
    };
    std::string body = req.dump(-1, ' ', false,
                                 nlohmann::json::error_handler_t::replace);

    WebRequestSpec spec = MakeWebSpec(url);
    spec.method = "POST";
    spec.body = std::move(body);
    spec.bodyContentType = "application/json";
    spec.headers.push_back({"Accept", "application/json, text/event-stream"});
    auto r = RequestSync(std::move(spec));
    if (!r.ok) return "Error: " + r.error;

    // Walk SSE events, find the first `data: {...}` line and extract
    // result.content[0].text.
    std::istringstream stream(r.body);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data: ", 0) != 0) continue;
        std::string payload = line.substr(6);
        try {
            auto j = nlohmann::json::parse(payload);
            if (j.contains("result") && j["result"].contains("content")
                && j["result"]["content"].is_array()
                && !j["result"]["content"].empty()) {
                const auto& first = j["result"]["content"][0];
                if (first.contains("text") && first["text"].is_string()) {
                    return CapOutput(first["text"].get<std::string>());
                }
            }
            // JSON-RPC error path.
            if (j.contains("error") && j["error"].is_object()) {
                std::string msg = j["error"].value("message", std::string{"unknown error"});
                return "Error: Exa MCP returned: " + msg;
            }
        } catch (...) {
            continue;
        }
    }
    return "No results (Exa MCP returned no parseable content).";
}

// ---------------- definitions ----------------

nlohmann::json ToolDef(const char* name, const char* desc, nlohmann::json params) {
    return {
        {"type", "function"},
        {"function", {
            {"name", name},
            {"description", desc},
            {"parameters", std::move(params)},
        }},
    };
}

nlohmann::json StrParam(const char* desc) {
    return {{"type", "string"}, {"description", desc}};
}

}  // namespace

nlohmann::json GetToolDefinitions() {
    using J = nlohmann::json;
    J tools = J::array();

    tools.push_back(ToolDef(
        "read_file",
        "Read a text file and return its contents prefixed with line numbers.",
        {{"type", "object"},
         {"properties", {{"path", StrParam("Path to the file (absolute or relative to cwd).")}}},
         {"required", {"path"}}}));

    tools.push_back(ToolDef(
        "write_file",
        "Create or overwrite a file with the given content. Parent directories are created automatically.",
        {{"type", "object"},
         {"properties", {
             {"path", StrParam("Path to the file.")},
             {"content", StrParam("Full file contents to write.")},
         }},
         {"required", {"path", "content"}}}));

    tools.push_back(ToolDef(
        "edit_file",
        "Replace a unique substring in an existing file. Fails if old_str is missing or matches more than once.",
        {{"type", "object"},
         {"properties", {
             {"path", StrParam("Path to the file.")},
             {"old_str", StrParam("Existing substring to replace. Must be unique within the file.")},
             {"new_str", StrParam("Replacement string.")},
         }},
         {"required", {"path", "old_str", "new_str"}}}));

    tools.push_back(ToolDef(
        "bash",
        "Run a bash command. Captures stdout+stderr, 30 second timeout, runs in the application's cwd.",
        {{"type", "object"},
         {"properties", {{"command", StrParam("Shell command to execute.")}}},
         {"required", {"command"}}}));

    tools.push_back(ToolDef(
        "list_directory",
        "List the entries of a directory.",
        {{"type", "object"},
         {"properties", {{"path", StrParam("Directory path. Defaults to '.'.")}}},
         {"required", nlohmann::json::array()}}));

    tools.push_back(ToolDef(
        "web_fetch",
        "Fetch a URL and return its content. HTML responses are reduced to readable text.",
        {{"type", "object"},
         {"properties", {{"url", StrParam("Absolute URL (https assumed if scheme is omitted).")}}},
         {"required", {"url"}}}));

    tools.push_back(ToolDef(
        "web_search",
        "Search the web via Exa AI and return the top 8 results with titles, URLs, "
        "and content highlights. Good for fresh facts, docs, errors, and recent news.",
        {{"type", "object"},
         {"properties", {{"query", StrParam("Search query (natural language).")}}},
         {"required", {"query"}}}));

    tools.push_back(ToolDef(
        "grit_history_search",
        "Search the transcripts of past wx_gritcode conversations across "
        "every project the user has worked on (excludes the current session). "
        "This is the AUTHORITATIVE source of prior-conversation recall — use "
        "it first whenever the user references any past work (\"last time\", "
        "\"once again\", \"we had\", \"how did we\", \"in <project>\"). "
        "Query budget: typically 1-3 searches per user question. Start with "
        "ONE broad keyword query; only run more if the first returned strong "
        "hits. Returns short snippets (~20-token window) with session_id, "
        "turn_index, and full_chars — follow up with grit_history_fetch.",
        {{"type", "object"},
         {"properties", {
             {"query", StrParam("2-5 keywords. FTS5 MATCH expression (phrases in quotes, AND/OR/NOT, prefix*).")},
             {"limit", {{"type", "integer"},
                        {"description", "Max results (default 5, max 20)."}}},
         }},
         {"required", {"query"}}}));

    tools.push_back(ToolDef(
        "grit_history_fetch",
        "Read the full text of a specific past turn plus a window of "
        "surrounding turns. Use after grit_history_search when a snippet "
        "looks relevant. session_id + turn_index come directly from a "
        "grit_history_search hit. Default window is 2 turns before + 2 after.",
        {{"type", "object"},
         {"properties", {
             {"session_id", StrParam("Opaque session id from grit_history_search.")},
             {"turn_index", {{"type", "integer"},
                             {"description", "Turn index from grit_history_search."}}},
             {"before", {{"type", "integer"},
                         {"description", "How many prior turns to include (default 2, max 10)."}}},
             {"after",  {{"type", "integer"},
                         {"description", "How many following turns to include (default 2, max 10)."}}},
         }},
         {"required", {"session_id", "turn_index"}}}));

    return tools;
}

namespace {

std::string ToolGritHistorySearch(const nlohmann::json& args, MemoryDB* memory,
                                  const std::string& currentSessionId) {
    if (!memory || !memory->IsOpen()) return "Memory index is not available.";
    std::string query = GetStringArg(args, "query");
    if (query.empty()) return "Error: 'query' is required.";
    int limit = 5;
    if (args.is_object() && args.contains("limit") && args["limit"].is_number_integer())
        limit = args["limit"].get<int>();
    if (limit < 1) limit = 1;
    if (limit > 20) limit = 20;
    auto hits = memory->Search(query, limit, currentSessionId);
    return MemoryDB::FormatSearchHits(hits);
}

std::string ToolGritHistoryFetch(const nlohmann::json& args, MemoryDB* memory) {
    if (!memory || !memory->IsOpen()) return "Memory index is not available.";
    std::string sid = GetStringArg(args, "session_id");
    int ti = -1, before = 2, after = 2;
    if (args.is_object()) {
        if (args.contains("turn_index") && args["turn_index"].is_number_integer())
            ti = args["turn_index"].get<int>();
        if (args.contains("before") && args["before"].is_number_integer())
            before = args["before"].get<int>();
        if (args.contains("after") && args["after"].is_number_integer())
            after = args["after"].get<int>();
    }
    if (sid.empty() || ti < 0)
        return "Error: 'session_id' and 'turn_index' are required.";
    auto hits = memory->Fetch(sid, ti, before, after);
    return MemoryDB::FormatFetchHits(hits);
}

}  // namespace

std::string DispatchTool(const std::string& name, const nlohmann::json& args,
                         ToolCancelToken* token,
                         MemoryDB* memory,
                         const std::string& currentSessionId) {
    if (token && token->cancelled.load()) return "[cancelled]";
    try {
        if (name == "read_file")     return CapOutput(ToolReadFile(args));
        if (name == "write_file")    return CapOutput(ToolWriteFile(args));
        if (name == "edit_file")     return CapOutput(ToolEditFile(args));
        if (name == "bash")          return CapOutput(ToolBash(args, token));
        if (name == "list_directory")return CapOutput(ToolListDirectory(args));
        if (name == "web_fetch")     return ToolWebFetch(args);   // already capped
        if (name == "web_search")    return ToolWebSearch(args);  // already capped
        if (name == "grit_history_search")
            return ToolGritHistorySearch(args, memory, currentSessionId);
        if (name == "grit_history_fetch")
            return ToolGritHistoryFetch(args, memory);
        return "Error: unknown tool '" + name + "'";
    } catch (const std::exception& e) {
        return std::string("Error: tool threw exception: ") + e.what();
    } catch (...) {
        return "Error: tool threw unknown exception";
    }
}
