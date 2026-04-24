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

#pragma once
#include "session.h"
#include <string>
#include <vector>
#include <mutex>

struct sqlite3;

// Cross-project conversation memory backed by SQLite FTS5.
//
// One row per turn (ChatMessage), keyed on (session_id, turn_index) for
// idempotent re-indexing. Search returns BM25-ranked turns matching the
// FTS5 MATCH expression. Stays out of the way: no embeddings, no daemon,
// just an inverted index over the same conversation history that Session
// already persists as JSON.
class MemoryDB {
public:
    struct Hit {
        std::string cwd;
        std::string role;
        std::string text;
        std::string session_id;
        int turn_index = 0;
        std::string timestamp;
        double score = 0;  // BM25 score (lower = better)
    };

    MemoryDB() = default;
    ~MemoryDB();

    // Open (and create if missing) the memory database. Returns false if the
    // file can't be opened or the schema can't be created. Safe to call
    // multiple times — subsequent calls are no-ops if already open.
    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return db_ != nullptr; }

    // Delete every indexed turn for this session. Used before a full
    // re-insert when we don't know which turns changed.
    bool ClearSession(const std::string& session_id);

    // Insert one turn. The (session_id, turn_index) key is enforced via a
    // pre-delete: any existing row with that key is removed before insert,
    // so calling this on the same turn twice produces one row.
    bool IndexTurn(const std::string& session_id,
                   const std::string& cwd,
                   int turn_index,
                   const std::string& role,
                   const std::string& text,
                   const std::string& timestamp);

    // Bulk re-index every turn of a session. Wraps a transaction around
    // ClearSession + IndexTurn calls. Used by --reindex backfill and by
    // Session-loading paths.
    bool RebuildSession(const std::string& session_id,
                        const std::string& cwd,
                        const std::vector<ChatMessage>& history,
                        const std::string& timestamp);

    // FTS5 MATCH search. `query` is an FTS5 query expression — for the
    // common case of "csd shadow" (whitespace-separated terms) it works
    // unmodified. Bad queries (FTS5 syntax errors) return an empty vector.
    // If exclude_session_id is non-empty, rows with that session_id are
    // excluded from results — used to keep the agent from re-surfacing
    // turns from the very session it's currently in.
    std::vector<Hit> Search(const std::string& query,
                            int limit,
                            const std::string& exclude_session_id);

    // Default db path: $XDG_DATA_HOME/gritcode/memory.db (or
    // ~/.local/share/gritcode/memory.db).
    static std::string DefaultPath();

    // Default sessions directory we walk during --reindex.
    static std::string SessionsDir();

private:
    sqlite3* db_ = nullptr;
    std::mutex mu_;  // serializes access; sqlite is THREADSAFE=2 (multi-thread)

    bool EnsureSchema();
    bool Exec(const char* sql);
};
