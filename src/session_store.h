#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// On-disk persistence for chat sessions, keyed by working directory (one
// session per folder). Storage layout mirrors gritcode:
//   ~/.local/share/wx_gritcode/sessions/<hash>.json   (per-session)
//   ~/.local/share/wx_gritcode/sessions.json          (index of cwds)
// Writes are atomic (temp file then rename) so a crash mid-write can't
// corrupt the index or any session file.
class SessionStore {
public:
    struct Entry {
        std::string id;        // 16-hex hash of cwd
        std::string cwd;       // absolute path
        std::string lastUsed;  // ISO 8601, used for sorting (most recent first)
    };

    // Loads the index from disk, creating storage directories if needed.
    void Init();

    // Index entries sorted by lastUsed descending. Empty after Init() if no
    // sessions have been created yet.
    const std::vector<Entry>& List() const { return entries_; }

    // The cwd that was active when the app was last closed, or std::nullopt
    // if none recorded.
    std::optional<std::string> LastActiveCwd() const { return lastActiveCwd_; }

    // Stable id for a cwd (16-hex hash). Same cwd always maps to same id.
    static std::string IdForCwd(const std::string& cwd);

    // Reads the full session file for the given cwd. Returns false if the
    // file is missing or malformed; outHistory is unchanged in that case.
    bool Load(const std::string& cwd,
              std::vector<nlohmann::json>& outHistory) const;

    // Writes the full session and updates the index (lastUsed timestamp).
    // Creates a new entry if this cwd hasn't been seen before.
    void Save(const std::string& cwd,
              const std::vector<nlohmann::json>& history);

    // Persists the "lastActiveCwd" pointer in the index without touching
    // session files. Called when the user switches sessions.
    void SetLastActiveCwd(const std::string& cwd);

    // Register a fresh session for cwd in the index even if no history yet.
    // Lets a brand-new session show up in the dropdown immediately.
    void RegisterCwd(const std::string& cwd);

private:
    std::string root_;         // ~/.local/share/wx_gritcode
    std::string sessionsDir_;  // root_ + "/sessions"
    std::string indexPath_;    // root_ + "/sessions.json"

    std::vector<Entry> entries_;
    std::optional<std::string> lastActiveCwd_;

    // Re-sort entries_ by lastUsed descending after a mutation.
    void SortEntries();
    // Persist entries_ + lastActiveCwd_ to indexPath_ atomically.
    void WriteIndex() const;
    // Load index file; populates entries_ and lastActiveCwd_. Silent on
    // missing file (treated as empty index).
    void ReadIndex();
};
