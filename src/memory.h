#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <mutex>

struct sqlite3;

// Cross-project conversation memory backed by SQLite FTS5.
//
// One row per turn (chat message), keyed on (session_id, turn_index) for
// idempotent re-indexing. Search returns BM25-ranked turns matching the
// FTS5 MATCH expression. Stays out of the way: no embeddings, no daemon,
// just an inverted index over the same conversation history that
// SessionStore already persists as JSON.
class MemoryDB {
public:
    struct Hit {
        std::string cwd;
        std::string role;
        std::string text;         // snippet (Search) or full turn text (Fetch)
        std::string session_id;
        int turn_index = 0;
        std::string timestamp;
        double score = 0;         // BM25 score (lower = better); 0 for Fetch rows
        int full_chars = 0;       // byte size of the full turn text — lets the
                                  // agent decide whether a Fetch is worth it
    };

    MemoryDB() = default;
    ~MemoryDB();

    // Open (and create if missing) the memory database. Returns false if the
    // file can't be opened or the schema can't be created. Safe to call
    // multiple times — subsequent calls are no-ops if already open.
    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return db_ != nullptr; }

    // Delete every indexed turn for this session.
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
    // ClearSession + IndexTurn calls. `messages` is a JSON array of
    // {role, content, ...} entries (gritcode's wire format). System
    // messages and tool messages with no content are skipped.
    bool RebuildSession(const std::string& session_id,
                        const std::string& cwd,
                        const nlohmann::json& messages,
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

    // Return a window of consecutive turns around a given (session_id,
    // turn_index). Full turn text is returned (no snippetting) — this is the
    // "zoom in on a specific match" surface. `before` and `after` are each
    // clamped to [0, 10].
    std::vector<Hit> Fetch(const std::string& session_id,
                           int turn_index,
                           int before,
                           int after);

    // Default db path: $XDG_DATA_HOME/gritcode/memory.db (or
    // ~/.local/share/gritcode/memory.db).
    static std::string DefaultPath();

    // Default sessions directory we walk during --reindex.
    static std::string SessionsDir();

    // Compact search output: snippet only, with session_id/turn_index/full_chars
    // so the agent can follow up with grit_history_fetch. Total output bounded.
    static std::string FormatSearchHits(const std::vector<Hit>& hits);

    // Full-text window output (from Fetch). Per-hit cap + total cap so even a
    // window of fat tool turns stays under the MCP response limit.
    static std::string FormatFetchHits(const std::vector<Hit>& hits);

private:
    sqlite3* db_ = nullptr;
    std::mutex mu_;  // serializes access; sqlite is THREADSAFE=2 (multi-thread)

    bool EnsureSchema();
    bool Exec(const char* sql);
};
