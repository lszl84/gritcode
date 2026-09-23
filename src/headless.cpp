// Headless agent loop for benchmarking inside a Docker container (no display,
// no wx event loop). It reuses the same tool layer as the GUI
// (GetToolDefinitions / DispatchTool) and the blocking HTTP client
// (RequestSync), so tool behaviour matches the desktop app. Grit history
// tools are intentionally disabled.
//
// Model: DeepSeek "flash" (fast chat model), key from DEEPSEEK_API_KEY.

#include "headless.h"
#include "streaming_web_request.h"
#include "tools.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kModel = "deepseek-flash";
constexpr const char* kUrl = "https://api.deepseek.com/chat/completions";
constexpr int kMaxToolRounds = 40;

// Collect AGENTS.md from cwd upward to the git root (same idea as the GUI's
// LoadAgentsInstructions, minus the global config dir which is irrelevant in
// a fresh container).
std::string LoadAgents(const std::string& cwd) {
    // Locate the git root (a directory containing .git), if any.
    std::string root;
    {
        std::string d = cwd;
        while (true) {
            if (fs::is_directory(d + "/.git")) { root = d; break; }
            const size_t pos = d.find_last_of('/');
            if (pos == std::string::npos || pos == 0) break;
            d = d.substr(0, pos);
        }
    }

    std::vector<std::string> paths;
    std::string cur = cwd;
    while (true) {
        paths.push_back(cur + "/AGENTS.md");
        if (!root.empty() && cur == root) break;
        if (root.empty()) break;  // not in a repo: only the session dir
        const size_t pos = cur.find_last_of('/');
        if (pos == std::string::npos) break;
        const std::string parent = cur.substr(0, pos);
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }

    std::string out;
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();
        if (content.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += "Instructions from: " + p + "\n" + content;
    }
    return out;
}

std::string SystemPrompt(const std::string& cwd) {
    std::string s =
        "You are a helpful AI coding assistant with access to local file, "
        "shell, and web tools. Use them when the user's request requires "
        "reading files, running shell commands, or fetching web pages. "
        "Prefer concrete actions over speculation. Use markdown for "
        "formatting and fenced code blocks for code.\n\n"
        "You are running on Linux. The shell tool uses bash. "
        "Use Unix commands. Path separators are forward slashes.\n\n"
        "Working directory: " + cwd + "\n";

    s += "\nGrit History tools (grit_history_search, grit_history_fetch) "
         "are DISABLED for this session. Do not call them - they will return "
         "an error. Do not reference or rely on prior conversation history "
         "from other projects or sessions.";

    const std::string agents = LoadAgents(cwd);
    if (!agents.empty()) s += "\n\n" + agents;
    return s;
}

std::string CurrentCwd() {
    std::error_code ec;
    std::string p = fs::current_path(ec).string();
    return ec ? std::string(".") : p;
}

}  // namespace

int RunHeadless(const std::string& prompt) {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) {
        std::fprintf(stderr, "headless: DEEPSEEK_API_KEY is not set\n");
        return 1;
    }
    const std::string cwd = CurrentCwd();

    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", SystemPrompt(cwd)}});
    messages.push_back({{"role", "user"}, {"content", prompt}});

    ToolCancelToken token;

    std::string finalAnswer;
    bool answered = false;
    for (int round = 0; round < kMaxToolRounds && !answered; ++round) {
        json req;
        req["model"] = kModel;
        req["stream"] = false;
        req["max_tokens"] = 32000;
        req["messages"] = messages;
        req["tools"] = GetToolDefinitions(/*includeGritHistory=*/false);

        WebRequestSpec spec;
        spec.url = kUrl;
        spec.method = "POST";
        spec.body = req.dump();
        spec.bodyContentType = "application/json";
        spec.headers.push_back({"Authorization",
                                std::string("Bearer ") + key});
        spec.connectTimeoutSeconds = 30;
        spec.idleTimeoutSeconds = 120;

        WebResponse resp = RequestSync(std::move(spec), nullptr);
        if (!resp.ok) {
            std::fprintf(stderr,
                         "headless: request failed (HTTP %ld): %s\n",
                         resp.status, resp.error.c_str());
            return 1;
        }

        json j;
        try {
            j = json::parse(resp.body);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "headless: unparseable response: %s\n", e.what());
            std::fprintf(stderr, "headless: %.400s\n", resp.body.c_str());
            return 1;
        }

        if (!j.contains("choices") || !j["choices"].is_array()
            || j["choices"].empty()) {
            std::fprintf(stderr, "headless: no choices in response\n");
            std::fprintf(stderr, "headless: %.400s\n", resp.body.c_str());
            return 1;
        }

        const json& msg = j["choices"][0]["message"];
        const std::string content = msg.value("content", std::string());
        const json& toolCalls = msg.value("tool_calls", json::array());

        json am;
        am["role"] = "assistant";
        am["content"] = content;
        if (msg.contains("reasoning_content")
            && msg["reasoning_content"].is_string()) {
            am["reasoning_content"] = msg["reasoning_content"];
        }

        if (toolCalls.is_array() && !toolCalls.empty()) {
            am["tool_calls"] = toolCalls;
            messages.push_back(am);

            for (const auto& tc : toolCalls) {
                const std::string callId = tc.value("id", std::string());
                const std::string name =
                    tc.value("function", json::object()).value("name", std::string());
                const std::string argsStr =
                    tc.value("function", json::object())
                        .value("arguments", std::string());

                json args = json::object();
                if (!argsStr.empty()) {
                    try {
                        args = json::parse(argsStr);
                    } catch (const std::exception&) {
                        args = json::object();
                    }
                }

                std::fprintf(stderr, "[headless tool] %s\n", name.c_str());
                const std::string result =
                    DispatchTool(name, args, &token, /*memory=*/nullptr, cwd,
                                 /*enableGritHistory=*/false);

                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", callId},
                    {"content", result},
                });
            }
            continue;  // loop again to feed tool results back to the model
        }

        finalAnswer = content;
        answered = true;
    }

    if (!answered) {
        std::fprintf(stderr, "headless: exceeded %d tool rounds\n", kMaxToolRounds);
        return 1;
    }

    std::cout << finalAnswer << std::endl;
    return 0;
}
