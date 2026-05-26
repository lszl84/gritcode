#include "memory.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

MemoryDB::~MemoryDB() { Close(); }

std::string MemoryDB::DefaultPath() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = std::string(xdg) + "/wx_gritcode";
    } else {
        const char* home = std::getenv("HOME");
        if (!home) home = "/tmp";
        base = std::string(home) + "/.local/share/wx_gritcode";
    }
    return base + "/memory.db";
}

std::string MemoryDB::SessionsDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/wx_gritcode/sessions";
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.local/share/wx_gritcode/sessions";
}

bool MemoryDB::Open(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) return true;

    try {
        fs::create_directories(fs::path(path).parent_path());
    } catch (...) {}

    int rc = sqlite3_open_v2(path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "MemoryDB open failed: %s\n",
                     db_ ? sqlite3_errmsg(db_) : "?");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }

    // WAL for concurrent readers/writers; NORMAL sync is fine because the
    // DB is a derived index — a crash that loses the last few writes is
    // recovered by the next save's RebuildSession call.
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA synchronous=NORMAL");
    Exec("PRAGMA temp_store=MEMORY");

    return EnsureSchema();
}

void MemoryDB::Close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool MemoryDB::Exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "MemoryDB exec failed: %s [sql: %s]\n",
                     err ? err : "?", sql);
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool MemoryDB::EnsureSchema() {
    // FTS5 virtual table: cwd + text are searchable; role/ids/ts are
    // metadata-only (UNINDEXED keeps them out of the inverted index so
    // searching for "user" doesn't return the world).
    //
    // tokenize=porter+unicode61: porter stemming so "compacting"/"compact"
    // collide; unicode61 normalizes case + diacritics.
    return Exec(
        "CREATE VIRTUAL TABLE IF NOT EXISTS turns USING fts5("
        "    cwd, text,"
        "    role UNINDEXED,"
        "    session_id UNINDEXED,"
        "    turn_index UNINDEXED,"
        "    timestamp UNINDEXED,"
        "    tokenize = 'porter unicode61'"
        ")");
}

bool MemoryDB::ClearSession(const std::string& session_id) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "DELETE FROM turns WHERE session_id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MemoryDB::IndexTurn(const std::string& session_id,
                         const std::string& cwd,
                         int turn_index,
                         const std::string& role,
                         const std::string& text,
                         const std::string& timestamp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    if (text.empty()) return true;

    {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db_,
            "DELETE FROM turns WHERE session_id = ? AND turn_index = ?",
            -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(del, 2, turn_index);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    sqlite3_stmt* ins = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT INTO turns(cwd, text, role, session_id, turn_index, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?)", -1, &ins, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(ins, 1, cwd.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (ins, 5, turn_index);
    sqlite3_bind_text(ins, 6, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    return rc == SQLITE_DONE;
}

// Extract the searchable text for one message. We index the user-visible
// content plus a stringified form of any tool_calls so a search for the
// command/path the model ran lands on the calling turn.
static std::string TextForMessage(const nlohmann::json& m) {
    if (!m.is_object()) return {};
    std::string s;
    if (m.contains("content") && m["content"].is_string()) {
        s = m["content"].get<std::string>();
    }
    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) {
            if (!tc.is_object() || !tc.contains("function")) continue;
            const auto& fn = tc["function"];
            if (!s.empty()) s += '\n';
            s += "[tool ";
            if (fn.contains("name") && fn["name"].is_string())
                s += fn["name"].get<std::string>();
            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                s += ' ';
                s += fn["arguments"].get<std::string>();
            }
            s += ']';
        }
    }
    return s;
}

bool MemoryDB::RebuildSession(const std::string& session_id,
                              const std::string& cwd,
                              const nlohmann::json& messages,
                              const std::string& timestamp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;
    if (!messages.is_array()) return false;

    Exec("BEGIN");
    {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db_,
            "DELETE FROM turns WHERE session_id = ?", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO turns(cwd, text, role, session_id, turn_index, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?)", -1, &ins, nullptr) != SQLITE_OK) {
        Exec("ROLLBACK");
        return false;
    }

    int idx = 0;
    for (const auto& m : messages) {
        std::string role = m.is_object() ? m.value("role", std::string{}) : "";
        std::string text = TextForMessage(m);
        if (text.empty() || role == "system") { ++idx; continue; }
        sqlite3_reset(ins);
        sqlite3_bind_text(ins, 1, cwd.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (ins, 5, idx);
        sqlite3_bind_text(ins, 6, timestamp.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            sqlite3_finalize(ins);
            Exec("ROLLBACK");
            return false;
        }
        ++idx;
    }
    sqlite3_finalize(ins);
    Exec("COMMIT");
    return true;
}

// Per-hit and total caps for Fetch output. Claude's MCP client spills anything
// over ~25k tokens (~87k chars) to a file, so stay well under that.
static constexpr size_t kMaxFetchHitChars = 4000;
static constexpr size_t kMaxFetchTotalChars = 20000;
static constexpr size_t kMaxSearchTotalChars = 8000;

static std::string TruncateUtf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    // Back up to a UTF-8 code-point boundary so we don't emit a half-glyph.
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "\n... [truncated " +
           std::to_string(s.size() - cut) + " more chars]";
}

std::string MemoryDB::FormatSearchHits(const std::vector<Hit>& hits) {
    if (hits.empty()) return "No matches found.";
    std::string out;
    out.reserve(2048);
    out += "Found " + std::to_string(hits.size()) +
           " match(es). To see full turn + surrounding context, call "
           "grit_history_fetch with the session_id and turn_index from a hit.\n\n";

    size_t shown = 0;
    for (size_t i = 0; i < hits.size(); i++) {
        const auto& h = hits[i];
        std::string entry;
        entry += "### [" + std::to_string(i + 1) + "] " + h.cwd +
                 " (" + h.role + ", " + h.timestamp + ")\n";
        entry += "session_id: " + h.session_id +
                 "  turn_index: " + std::to_string(h.turn_index) +
                 "  full_chars: " + std::to_string(h.full_chars) + "\n";
        entry += h.text;
        if (!entry.empty() && entry.back() != '\n') entry += '\n';
        entry += '\n';

        if (out.size() + entry.size() > kMaxSearchTotalChars) {
            out += "... [" + std::to_string(hits.size() - shown) +
                   " more result(s) omitted; refine query or lower `limit`]\n";
            break;
        }
        out += entry;
        ++shown;
    }
    return out;
}

std::string MemoryDB::FormatFetchHits(const std::vector<Hit>& hits) {
    if (hits.empty()) return "No turns found for that (session_id, turn_index).";
    std::string out;
    out.reserve(4096);
    out += "Returned " + std::to_string(hits.size()) + " turn(s):\n\n";

    size_t shown = 0;
    for (size_t i = 0; i < hits.size(); i++) {
        const auto& h = hits[i];
        std::string entry;
        entry += "### turn " + std::to_string(h.turn_index) +
                 " (" + h.role + ", " + h.timestamp + ")\n";
        entry += TruncateUtf8(h.text, kMaxFetchHitChars);
        if (!entry.empty() && entry.back() != '\n') entry += '\n';
        entry += '\n';

        if (out.size() + entry.size() > kMaxFetchTotalChars) {
            out += "... [" + std::to_string(hits.size() - shown) +
                   " more turn(s) omitted to fit size limit; narrow before/after]\n";
            break;
        }
        out += entry;
        ++shown;
    }
    return out;
}

std::vector<MemoryDB::Hit> MemoryDB::Search(const std::string& query,
                                            int limit,
                                            const std::string& exclude_session_id) {
    std::vector<Hit> out;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_ || query.empty()) return out;

    // snippet(turns, 1, '[', ']', '…', 20): column 1 = text; 20-token window
    // centered on the best match. Matches wrapped in [brackets] so the agent
    // can see what actually matched. length(text) exposes the full turn size
    // so the agent can decide whether grit_history_fetch is worth it.
    std::string sql =
        "SELECT cwd, role, "
        "       snippet(turns, 1, '[', ']', '…', 20) AS snip, "
        "       session_id, turn_index, timestamp, "
        "       bm25(turns) AS score, "
        "       length(text) AS full_chars "
        "FROM turns "
        "WHERE turns MATCH ? ";
    if (!exclude_session_id.empty()) sql += "AND session_id != ? ";
    sql += "ORDER BY bm25(turns) LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return out;
    int idx = 1;
    sqlite3_bind_text(stmt, idx++, query.c_str(), -1, SQLITE_TRANSIENT);
    if (!exclude_session_id.empty())
        sqlite3_bind_text(stmt, idx++, exclude_session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx, limit > 0 ? limit : 5);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Hit h;
        auto sv = [&](int col) {
            const unsigned char* p = sqlite3_column_text(stmt, col);
            return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
        };
        h.cwd        = sv(0);
        h.role       = sv(1);
        h.text       = sv(2);
        h.session_id = sv(3);
        h.turn_index = sqlite3_column_int(stmt, 4);
        h.timestamp  = sv(5);
        h.score      = sqlite3_column_double(stmt, 6);
        h.full_chars = sqlite3_column_int(stmt, 7);
        out.push_back(std::move(h));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<MemoryDB::Hit> MemoryDB::Fetch(const std::string& session_id,
                                           int turn_index,
                                           int before,
                                           int after) {
    std::vector<Hit> out;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_ || session_id.empty()) return out;

    if (before < 0) before = 0;
    if (after  < 0) after  = 0;
    if (before > 10) before = 10;
    if (after  > 10) after  = 10;

    int lo = turn_index - before;
    int hi = turn_index + after;
    if (lo < 0) lo = 0;

    const char* sql =
        "SELECT cwd, role, text, session_id, turn_index, timestamp, length(text) "
        "FROM turns "
        "WHERE session_id = ? AND turn_index >= ? AND turn_index <= ? "
        "ORDER BY turn_index ASC";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, lo);
    sqlite3_bind_int (stmt, 3, hi);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Hit h;
        auto sv = [&](int col) {
            const unsigned char* p = sqlite3_column_text(stmt, col);
            return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
        };
        h.cwd        = sv(0);
        h.role       = sv(1);
        h.text       = sv(2);
        h.session_id = sv(3);
        h.turn_index = sqlite3_column_int(stmt, 4);
        h.timestamp  = sv(5);
        h.full_chars = sqlite3_column_int(stmt, 6);
        out.push_back(std::move(h));
    }
    sqlite3_finalize(stmt);
    return out;
}
