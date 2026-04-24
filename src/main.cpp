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
#include "memory.h"
#include "mcp_stdio.h"
#include "session.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

static int RunReindex() {
    MemoryDB memory;
    if (!memory.Open(MemoryDB::DefaultPath())) {
        std::fprintf(stderr, "grit --reindex: failed to open memory DB at %s\n",
                     MemoryDB::DefaultPath().c_str());
        return 1;
    }

    std::string dir = MemoryDB::SessionsDir();
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "grit --reindex: no sessions dir at %s (nothing to do)\n",
                     dir.c_str());
        return 0;
    }

    int indexed = 0, skipped = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::ifstream f(entry.path());
        json j;
        try {
            f >> j;
        } catch (...) {
            std::fprintf(stderr, "  skip (parse error): %s\n", entry.path().c_str());
            skipped++;
            continue;
        }

        std::string sessionId = entry.path().stem().string();
        std::string cwd = j.value("cwd", "");
        std::string timestamp = j.value("lastUsed", "");

        std::vector<ChatMessage> history;
        if (j.contains("messages") && j["messages"].is_array()) {
            for (auto& m : j["messages"]) {
                ChatMessage cm;
                cm.role = m.value("role", "");
                cm.content = m.value("content", "");
                cm.toolCallId = m.value("tool_call_id", "");
                cm.reasoningContent = m.value("reasoningContent", "");
                cm.isSummary = m.value("isSummary", false);
                history.push_back(std::move(cm));
            }
        }

        if (!memory.RebuildSession(sessionId, cwd, history, timestamp)) {
            std::fprintf(stderr, "  skip (index error): %s\n", entry.path().c_str());
            skipped++;
            continue;
        }
        std::printf("  indexed %s (%zu turns) — %s\n",
                    sessionId.c_str(), history.size(), cwd.c_str());
        indexed++;
    }

    std::printf("\nReindex complete: %d sessions indexed, %d skipped.\nDB: %s\n",
                indexed, skipped, MemoryDB::DefaultPath().c_str());
    return 0;
}

int main(int argc, char* argv[]) {
    bool sessionChooser = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session-chooser") == 0) {
            sessionChooser = true;
        } else if (strcmp(argv[i], "--mcp-stdio") == 0) {
            // Stdio MCP server mode: serve memory_search over JSON-RPC on
            // stdin/stdout. No GUI, no window, no side effects. Spawned by
            // Claude CLI via --mcp-config for cross-project memory recall
            // during ACP sessions.
            return RunMcpStdioServer();
        } else if (strcmp(argv[i], "--reindex") == 0) {
            // One-shot backfill: walk every session JSON file and re-index
            // its turns into the FTS5 database. Used once after first install
            // to seed the index from pre-existing sessions; safe to re-run
            // (idempotent — RebuildSession deletes and re-inserts).
            return RunReindex();
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: grit [OPTIONS]\n\n"
                        "Options:\n"
                        "  --session-chooser    Show the session chooser UI at startup\n"
                        "  --reindex            Rebuild the memory index from session history on disk\n"
                        "  --mcp-stdio          Run as a stdio MCP server exposing memory_search\n"
                        "  --help, -h           Show this help\n");
            return 0;
        }
    }

    App app;
    if (!app.Init(sessionChooser)) {
        std::fprintf(stderr, "Failed to initialize app\n");
        return 1;
    }
    app.Run();
    return 0;
}
