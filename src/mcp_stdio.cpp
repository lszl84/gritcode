#include "mcp_stdio.h"
#include "memory.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <iostream>
#include <string>

using json = nlohmann::json;

// Echoed back in initialize replies. Claude's stdio client picks whatever the
// server sends and adapts. "2025-03-26" is the value other gritcode binaries
// already use.
static constexpr const char* kMcpProtocolVersion = "2025-03-26";

static json SearchToolDef() {
    return {
        {"name", "grit_history_search"},
        {"description",
         "Search the transcripts of past wx_gritcode conversations across "
         "every project the user has worked on. This is the AUTHORITATIVE "
         "source of prior-conversation recall — use it first whenever the "
         "user references any past work (\"last time\", \"once again\", "
         "\"we had\", \"how did we\", \"in <project>\", \"that thing from "
         "<name>\"). Do NOT try to list any memory directory under "
         "~/.claude — that is unrelated and unused here. "
         "Query budget: typically 1-3 searches per user question. Start "
         "with ONE broad keyword query; only run more if the first returned "
         "strong hits that suggest specific follow-ups. If a search returns "
         "no clearly relevant hits, STOP and answer from what you have (or "
         "ask the user) — do not fire speculative variant queries. "
         "Query: 2-5 keywords. Returns short snippets (~20-token window) "
         "with session_id, turn_index, and full_chars. Follow up with "
         "grit_history_fetch to read the full turn + surrounding context."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "2-5 keywords. Supports FTS5 MATCH syntax (phrases in quotes, AND/OR/NOT, prefix*)."}
                }},
                {"limit", {
                    {"type", "integer"},
                    {"description", "Max results (default 5, max 20)."},
                    {"default", 5}
                }}
            }},
            {"required", json::array({"query"})}
        }}
    };
}

static json FetchToolDef() {
    return {
        {"name", "grit_history_fetch"},
        {"description",
         "Read the full text of a specific past turn plus a window of "
         "surrounding turns. Use after grit_history_search when a snippet "
         "looks relevant and you need the surrounding conversation. "
         "session_id + turn_index come directly from a grit_history_search "
         "hit. Default window is 2 turns before + 2 after (5 total)."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"session_id", {
                    {"type", "string"},
                    {"description", "Opaque session id from grit_history_search result."}
                }},
                {"turn_index", {
                    {"type", "integer"},
                    {"description", "Turn index from grit_history_search result."}
                }},
                {"before", {
                    {"type", "integer"},
                    {"description", "How many prior turns to include (default 2, max 10)."},
                    {"default", 2}
                }},
                {"after", {
                    {"type", "integer"},
                    {"description", "How many following turns to include (default 2, max 10)."},
                    {"default", 2}
                }}
            }},
            {"required", json::array({"session_id", "turn_index"})}
        }}
    };
}

static json MakeResult(const json& id, const json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json MakeError(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", msg}}}};
}

int RunMcpStdioServer() {
    // Line-buffered stdout: Claude's stdio MCP client reads one line at a
    // time and hangs if the response isn't flushed immediately.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    MemoryDB memory;
    memory.Open(MemoryDB::DefaultPath());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            std::fprintf(stderr, "wx_gritcode --mcp-stdio: parse error\n");
            continue;
        }

        // Notifications have no id — swallow them (initialized, cancelled, …).
        if (!req.contains("id")) continue;
        json id = req["id"];
        std::string method = req.value("method", "");

        if (method == "initialize") {
            std::string clientVer = req.value("params", json::object())
                                        .value("protocolVersion", kMcpProtocolVersion);
            json result = {
                {"protocolVersion", clientVer},
                {"capabilities", {
                    {"tools", {{"listChanged", false}}}
                }},
                {"serverInfo", {
                    {"name", "grit-history"},
                    {"version", "0.1.0"}
                }}
            };
            std::cout << MakeResult(id, result).dump() << "\n";

        } else if (method == "tools/list") {
            json result = {{"tools", json::array({SearchToolDef(), FetchToolDef()})}};
            std::cout << MakeResult(id, result).dump() << "\n";

        } else if (method == "tools/call") {
            json params = req.value("params", json::object());
            std::string name = params.value("name", "");
            json args = params.value("arguments", json::object());

            std::string text;
            bool ok = true;

            if (!memory.IsOpen()) {
                text = "Memory index is not available.";
            } else if (name == "grit_history_search") {
                std::string query = args.value("query", "");
                int limit = args.value("limit", 5);
                if (limit < 1) limit = 1;
                if (limit > 20) limit = 20;
                if (query.empty()) {
                    text = "Error: 'query' is required.";
                } else {
                    auto hits = memory.Search(query, limit, "");
                    text = MemoryDB::FormatSearchHits(hits);
                }
            } else if (name == "grit_history_fetch") {
                std::string sid = args.value("session_id", "");
                int ti = args.value("turn_index", -1);
                int before = args.value("before", 2);
                int after  = args.value("after", 2);
                if (sid.empty() || ti < 0) {
                    text = "Error: 'session_id' and 'turn_index' are required.";
                } else {
                    auto hits = memory.Fetch(sid, ti, before, after);
                    text = MemoryDB::FormatFetchHits(hits);
                }
            } else {
                std::cout << MakeError(id, -32601, "unknown tool: " + name).dump() << "\n";
                continue;
            }

            json result = {
                {"content", json::array({
                    {{"type", "text"}, {"text", text}}
                })},
                {"isError", !ok}
            };
            std::cout << MakeResult(id, result).dump() << "\n";

        } else if (method == "ping") {
            std::cout << MakeResult(id, json::object()).dump() << "\n";

        } else {
            std::cout << MakeError(id, -32601, "unknown method: " + method).dump() << "\n";
        }
        std::cout.flush();
    }
    return 0;
}
