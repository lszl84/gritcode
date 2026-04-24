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

#include "mcp_stdio.h"
#include "memory.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>

using json = nlohmann::json;

// Protocol version we advertise. Claude CLI's current `initialize` call uses
// "2024-11-05"; the MCP spec allows the server to pick a version it supports
// and the client adapts. We just echo back whatever they sent, which is the
// recommended fallback behavior.
static constexpr const char* kMcpProtocolVersion = "2025-03-26";

static json ToolDef() {
    return {
        {"name", "memory_search"},
        {"description",
         "Search past gritcode conversations across all your projects. "
         "Use when the user references prior work in this or another project "
         "(\"last time\", \"in <project>\", \"how did we fix\"). "
         "Query: 2-5 keywords. Returns up to 5 verbatim turns with "
         "{project, timestamp, role, text}."},
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

static std::string FormatHits(const std::vector<MemoryDB::Hit>& hits) {
    if (hits.empty()) return "No matches.";
    std::string out;
    out += "Found " + std::to_string(hits.size()) + " match(es):\n\n";
    for (size_t i = 0; i < hits.size(); i++) {
        const auto& h = hits[i];
        out += "### [" + std::to_string(i + 1) + "] " + h.cwd +
               " (" + h.role + ", " + h.timestamp + ")\n";
        out += h.text;
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "\n";
    }
    return out;
}

static json MakeResult(const json& id, const json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json MakeError(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

int RunMcpStdioServer() {
    // Ensure stdout is line-buffered so each response flushes as soon as it's
    // written. Claude's stdio MCP client reads one line at a time and hangs
    // otherwise.
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
            // Can't respond without an id; just log to stderr and continue.
            std::fprintf(stderr, "grit-mcp-stdio: parse error on input\n");
            continue;
        }

        // Notifications have no id — we just swallow them (initialized,
        // cancelled, etc.).
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
                    {"name", "grit-memory"},
                    {"version", "0.1.0"}
                }}
            };
            std::cout << MakeResult(id, result).dump() << "\n";

        } else if (method == "tools/list") {
            json result = {{"tools", json::array({ToolDef()})}};
            std::cout << MakeResult(id, result).dump() << "\n";

        } else if (method == "tools/call") {
            json params = req.value("params", json::object());
            std::string name = params.value("name", "");
            json args = params.value("arguments", json::object());

            if (name != "memory_search") {
                std::cout << MakeError(id, -32601, "unknown tool: " + name).dump() << "\n";
                continue;
            }

            std::string query = args.value("query", "");
            int limit = args.value("limit", 5);
            if (limit < 1) limit = 1;
            if (limit > 20) limit = 20;

            std::string text;
            if (!memory.IsOpen()) {
                text = "Memory index is not available.";
            } else if (query.empty()) {
                text = "Error: 'query' is required.";
            } else {
                auto hits = memory.Search(query, limit, "");
                text = FormatHits(hits);
            }

            json result = {
                {"content", json::array({
                    {{"type", "text"}, {"text", text}}
                })},
                {"isError", false}
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
