#include "tools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/string.h>
#include <wx/webrequest.h>

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

std::string ToolBash(const nlohmann::json& args) {
    std::string cmd = GetStringArg(args, "command");
    if (cmd.empty()) return "Error: missing 'command' argument";

    // Write the command to a temp file and run it under `timeout`. This avoids
    // the quoting hell of inlining arbitrary user code into a `bash -c "..."`.
    char tmpl[] = "/tmp/wxgrit_bashXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return "Error: mkstemp failed";
    if (write(fd, cmd.data(), cmd.size()) != (ssize_t)cmd.size()) {
        close(fd);
        unlink(tmpl);
        return "Error: tempfile write failed";
    }
    close(fd);

    std::string full = "timeout 30 bash " + std::string(tmpl) + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        unlink(tmpl);
        return "Error: popen failed";
    }

    std::string out;
    char buf[4096];
    bool truncated = false;
    while (fgets(buf, sizeof(buf), pipe)) {
        if (out.size() < kMaxOutput) {
            out.append(buf);
            if (out.size() > kMaxOutput) {
                out.resize(kMaxOutput);
                truncated = true;
            }
        } else {
            truncated = true;
        }
    }
    int status = pclose(pipe);
    unlink(tmpl);

    if (truncated) out += "\n[output truncated]";
    if (out.empty()) out = "(no output)";

    // `timeout` exits 124 when it kills the child.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 124)
        out += "\n[timed out after 30s]";
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        out += "\n[exit " + std::to_string(WEXITSTATUS(status)) + "]";

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

// Synchronous HTTP GET via wxWebRequest + nested event loop. Tools are
// dispatched on the GUI thread between completion turns; running a nested
// loop here keeps the UI responsive (paint, etc.) while we wait.
struct WebGetResult {
    bool ok = false;
    int httpStatus = 0;
    wxString contentType;
    std::string body;   // raw bytes
    wxString error;
};

// Generic synchronous HTTP via wxWebRequest. method=="POST" means send `body`
// with `contentType`; otherwise GET. Extra headers are applied on top of the
// defaults (User-Agent, Accept).
WebGetResult WebRequestSync(const wxString& url,
                            const wxString& method = "GET",
                            const std::string& body = {},
                            const wxString& contentType = "application/json",
                            const std::vector<std::pair<wxString, wxString>>& extraHeaders = {}) {
    WebGetResult r;

    wxEvtHandler handler;
    auto request = wxWebSession::GetDefault().CreateRequest(&handler, url);
    if (!request.IsOk()) {
        r.error = "wxWebRequest creation failed";
        return r;
    }
    request.SetHeader("User-Agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36 wx_gritcode");
    request.SetHeader("Accept", "*/*");
    for (const auto& h : extraHeaders) request.SetHeader(h.first, h.second);
    if (method.Upper() == "POST") {
        request.SetMethod("POST");
        request.SetData(wxString::FromUTF8(body), contentType);
    }
    request.SetStorage(wxWebRequest::Storage_None);

    wxEventLoop loop;

    handler.Bind(wxEVT_WEBREQUEST_DATA,
        [&r](wxWebRequestEvent& e) {
            const char* p = static_cast<const char*>(e.GetDataBuffer());
            size_t n = e.GetDataSize();
            if (p && n) r.body.append(p, n);
        });

    handler.Bind(wxEVT_WEBREQUEST_STATE,
        [&r, &loop](wxWebRequestEvent& e) {
            switch (e.GetState()) {
            case wxWebRequest::State_Completed:
                r.ok = true;
                r.httpStatus = e.GetResponse().GetStatus();
                r.contentType = e.GetResponse().GetMimeType();
                loop.Exit(0);
                break;
            case wxWebRequest::State_Failed:
            case wxWebRequest::State_Cancelled:
            case wxWebRequest::State_Unauthorized:
                r.ok = false;
                r.error = e.GetErrorDescription();
                if (r.error.IsEmpty()) r.error = "request failed";
                loop.Exit(1);
                break;
            default:
                break;
            }
        });

    request.Start();
    loop.Run();

    if (r.ok && r.httpStatus >= 400) {
        r.ok = false;
        r.error = wxString::Format("HTTP %d", r.httpStatus);
    }
    return r;
}

WebGetResult WebGetSync(const wxString& url) {
    return WebRequestSync(url);
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

    auto r = WebGetSync(wxString::FromUTF8(url));
    if (!r.ok) return "Error: " + r.error.ToStdString();

    std::string ctLower = ToLower(r.contentType.ToStdString());
    std::string text;
    if (ctLower.find("html") != std::string::npos
        || (ctLower.empty() && r.body.find("<html") != std::string::npos))
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

    auto r = WebRequestSync(wxString::FromUTF8(url), "POST", body,
                            "application/json",
                            {{"Accept", "application/json, text/event-stream"}});
    if (!r.ok) return "Error: " + r.error.ToStdString();

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

    return tools;
}

std::string DispatchTool(const std::string& name, const nlohmann::json& args) {
    try {
        if (name == "read_file")     return CapOutput(ToolReadFile(args));
        if (name == "write_file")    return CapOutput(ToolWriteFile(args));
        if (name == "edit_file")     return CapOutput(ToolEditFile(args));
        if (name == "bash")          return CapOutput(ToolBash(args));
        if (name == "list_directory")return CapOutput(ToolListDirectory(args));
        if (name == "web_fetch")     return ToolWebFetch(args);   // already capped
        if (name == "web_search")    return ToolWebSearch(args);  // already capped
        return "Error: unknown tool '" + name + "'";
    } catch (const std::exception& e) {
        return std::string("Error: tool threw exception: ") + e.what();
    } catch (...) {
        return "Error: tool threw unknown exception";
    }
}
