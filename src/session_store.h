#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// On-disk persistence for chat threads. Every thread has its own session
// file; a folder's first thread uses the id derived from its cwd, so the
// one-session-per-folder layout of older versions stays readable as is:
//   ~/.local/share/gritcode/sessions/<id>.json   (per-thread)
//   ~/.local/share/gritcode/sessions.json        (index)
// The index keeps `sessions` (each folder's first thread, all that 0.5.x
// reads) and adds `threads` for the others, which 0.5.x ignores.
// Each folder can also have a project brief, keyed like its first thread:
//   ~/.local/share/gritcode/briefs/<id>.md
// Worktree threads record their checkout and branch in optional `worktree`
// and `branch` fields (older versions ignore them); the checkouts live in
//   ~/.local/share/gritcode/worktrees/<id>/
// Writes are atomic (temp file then rename) so a crash mid-write can't
// corrupt the index or any session file.
class SessionStore {
public:
    struct Entry {
        std::string id;        // 16 hex; IdForCwd(cwd) for a folder's first thread
        std::string cwd;       // absolute path
        std::string lastUsed;  // ISO 8601, used for sorting (most recent first)
        std::string title;     // empty until the first user message
        std::string parent;    // parent thread id, empty for top-level threads
        // A worktree thread's own checkout (root) and its branch; both empty
        // for threads working in the folder itself.
        std::string worktree;
        std::string branch;
    };

    // Loads the index from disk, creating storage directories if needed.
    void Init();

    // Index entries sorted by lastUsed descending. Empty after Init() if no
    // threads have been created yet.
    const std::vector<Entry>& List() const { return entries_; }

    // The cwd / thread that was active when the app was last closed, or
    // std::nullopt if none recorded.
    std::optional<std::string> LastActiveCwd() const { return lastActiveCwd_; }
    std::optional<std::string> LastActiveThread() const { return lastActiveThread_; }

    // Stable id for a cwd (16-hex hash): the id of the folder's first thread.
    static std::string IdForCwd(const std::string& cwd);
    // Random 16-hex id for an additional thread.
    static std::string NewThreadId();

    // Reads the messages of thread `id`. Returns false if the file is
    // missing or malformed; outHistory is unchanged in that case.
    bool Load(const std::string& id,
              std::vector<nlohmann::json>& outHistory) const;

    // Writes the full session (lastUsed = now) and updates the index. This is
    // the synchronous variant used where the UI thread must block (startup
    // seed, new folder); the hot persist path splits it into the two methods
    // below so the heavy part can run on a worker thread.
    void Save(Entry e, const std::vector<nlohmann::json>& history);

    // Thread-safe: writes only the session file (no index mutation). Safe to
    // call from a worker.
    void WriteSessionFile(const Entry& e,
                          const std::vector<nlohmann::json>& history) const;

    // UI-thread only: insert or update the entry + write sessions.json.
    // Cheap relative to WriteSessionFile.
    void UpdateIndex(const Entry& e);

    // ISO-8601 "now" string used as lastUsed.
    static std::string NowIso();

    // The folder's project brief, empty when it has none. Saving an empty
    // brief removes it; false when the file can't be written.
    std::string LoadBrief(const std::string& cwd) const;
    bool SaveBrief(const std::string& cwd, const std::string& text) const;

    // Where worktree thread `id` gets its checkout: worktrees/<id> in the
    // data dir, outside the user's repo.
    std::string WorktreePath(const std::string& id) const;

    // Persists the last active folder and thread in the index without
    // touching session files. Called when the user switches threads.
    void SetLastActive(const std::string& cwd, const std::string& threadId);

private:
    std::string root_;         // ~/.local/share/gritcode
    std::string sessionsDir_;  // root_ + "/sessions"
    std::string indexPath_;    // root_ + "/sessions.json"

    std::vector<Entry> entries_;
    std::optional<std::string> lastActiveCwd_;
    std::optional<std::string> lastActiveThread_;

    std::string BriefPath(const std::string& cwd) const;
    // Re-sort entries_ by lastUsed descending after a mutation.
    void SortEntries();
    // Persist entries_ + the last-active pointers to indexPath_ atomically.
    void WriteIndex() const;
    // Load index file; populates entries_ and the last-active pointers.
    // Silent on missing file (treated as empty index). Session files the
    // index doesn't list (0.5.x rewrites it without `threads`) are added
    // from their headers.
    void ReadIndex();
};
