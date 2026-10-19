#pragma once
#include <string>

// Git worktrees for worktree threads: a thread writes in its own checkout on
// branch grit/<id>, so it never writes under the project folder's other
// threads, and the user reviews, merges or discards that branch. Plain
// functions that run git synchronously (argv, no shell). `project` is the
// thread's folder, anywhere inside the user's checkout; `worktree` is the
// root of the thread's checkout.

// The root of the checkout holding `dir`; empty when it isn't in a git repo
// (or git can't run).
std::string GitTopLevel(const std::string& dir);

// The branch checked out at `dir`; empty when HEAD is detached.
std::string GitBranch(const std::string& dir);

// Creates `worktree` on a new branch `branch` from the project's HEAD (its
// committed state). Returns git's error, empty on success.
std::string AddWorktree(const std::string& project, const std::string& worktree,
                        const std::string& branch);

// Where a thread of folder `project` works in `worktree`: the same subfolder
// of it as `project` is of the user's checkout.
std::string WorktreeDir(const std::string& project, const std::string& worktree);

// What discarding the worktree would lose.
struct WorktreeWork {
    int unmergedCommits = 0;  // on the branch but not in the project's HEAD
    int changedFiles = 0;     // uncommitted in the worktree, new files included
    std::string error;        // git failed: the counts are unknown
};
WorktreeWork UnmergedWork(const std::string& project, const std::string& worktree,
                          const std::string& branch);

// Everything the worktree changed (committed and uncommitted, new files
// included) since it branched off the project's HEAD. Stages the worktree's
// files to see new ones; its index is the thread's, not the user's.
struct WorktreeDiff {
    std::string stat;   // git diff --stat
    std::string diff;
    std::string error;
};
WorktreeDiff ReviewWorktree(const std::string& project, const std::string& worktree);

// Commits the worktree's uncommitted changes on its branch as
// `commitMessage`, then merges the branch into the project's branch with a
// merge commit. Refuses when the project checkout has uncommitted changes or
// isn't on a branch; a conflicting merge is aborted. `message` says what
// happened and, when it stopped, what to do.
struct MergeOutcome {
    bool merged = false;
    std::string message;
};
MergeOutcome MergeWorktree(const std::string& project, const std::string& worktree,
                           const std::string& branch, const std::string& commitMessage);

// Removes the worktree (with whatever it holds; prunes it when it's gone
// already) and deletes its branch. Returns git's error, empty on success.
std::string RemoveWorktree(const std::string& project, const std::string& worktree,
                           const std::string& branch);
