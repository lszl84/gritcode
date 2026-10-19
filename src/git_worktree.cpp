#include "git_worktree.h"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct GitRun {
    int code = -1;  // exit status; -1 when git didn't exit normally
    std::string out;
    std::string err;
};

// Runs `git -C dir args...` (fork/exec, no shell) and collects its output.
GitRun Git(const std::string& dir, std::vector<std::string> args) {
    args.insert(args.begin(), {"git", "-C", dir});
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    GitRun r;
    int out[2], err[2];
    if (pipe(out) < 0) return r;
    if (pipe(err) < 0) {
        close(out[0]); close(out[1]);
        return r;
    }
    for (int fd : {out[0], out[1], err[0], err[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);

    const pid_t pid = fork();
    if (pid == 0) {
        if (devNull >= 0) dup2(devNull, STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(err[1], STDERR_FILENO);
        execvp("git", argv.data());
        const char msg[] = "could not run git\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
        _exit(127);
    }
    close(out[1]);
    close(err[1]);
    if (devNull >= 0) close(devNull);
    if (pid < 0) {
        close(out[0]); close(err[0]);
        r.err = "could not start git";
        return r;
    }

    // Drain both pipes together so neither fills up and blocks git.
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    std::string* sinks[2] = {&r.out, &r.err};
    int openFds = 2;
    char buf[65536];
    while (openFds > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !fds[i].revents) continue;
            const ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, (size_t)n);
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --openFds;
            }
        }
    }
    for (const pollfd& p : fds)
        if (p.fd >= 0) close(p.fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
    return r;
}

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

// What went wrong, in git's words.
std::string Failure(const GitRun& r) {
    std::string msg = Trim(r.err.empty() ? r.out : r.err);
    return msg.empty() ? "git failed" : msg;
}

std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) lines.push_back(line);
    return lines;
}

// "a, b, c and 4 more"
std::string FileList(const std::vector<std::string>& files) {
    constexpr size_t kShown = 5;
    std::string out;
    for (size_t i = 0; i < files.size() && i < kShown; ++i)
        out += (i ? ", " : "") + files[i];
    if (files.size() > kShown) out += " and " + std::to_string(files.size() - kShown) + " more";
    return out;
}

}  // namespace

std::string GitTopLevel(const std::string& dir) {
    GitRun r = Git(dir, {"rev-parse", "--show-toplevel"});
    return r.code == 0 ? Trim(r.out) : std::string();
}

std::string GitBranch(const std::string& dir) {
    GitRun r = Git(dir, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    return r.code == 0 ? Trim(r.out) : std::string();
}

std::string AddWorktree(const std::string& project, const std::string& worktree,
                        const std::string& branch) {
    std::error_code ec;
    fs::create_directories(fs::path(worktree).parent_path(), ec);
    GitRun r = Git(project, {"worktree", "add", "-b", branch, worktree, "HEAD"});
    return r.code == 0 ? std::string() : Failure(r);
}

std::string WorktreeDir(const std::string& project, const std::string& worktree) {
    const std::string top = GitTopLevel(project);
    std::error_code ec;
    const fs::path rel = top.empty() ? fs::path() : fs::relative(project, top, ec);
    if (ec || rel.empty() || rel == "." || *rel.begin() == "..") return worktree;
    return (fs::path(worktree) / rel).string();
}

WorktreeWork UnmergedWork(const std::string& project, const std::string& worktree,
                          const std::string& branch) {
    WorktreeWork w;
    if (fs::exists(worktree)) {
        GitRun s = Git(worktree, {"status", "--porcelain"});
        if (s.code != 0) {
            w.error = Failure(s);
            return w;
        }
        w.changedFiles = (int)Lines(s.out).size();
    }
    GitRun c = Git(project, {"rev-list", "--count", "HEAD.." + branch});
    if (c.code != 0) w.error = Failure(c);
    else w.unmergedCommits = std::atoi(c.out.c_str());
    return w;
}

WorktreeDiff ReviewWorktree(const std::string& project, const std::string& worktree) {
    WorktreeDiff d;
    if (!fs::exists(worktree)) {
        d.error = "The worktree " + worktree + " no longer exists.";
        return d;
    }
    GitRun head = Git(project, {"rev-parse", "HEAD"});
    if (head.code != 0) {
        d.error = Failure(head);
        return d;
    }
    // Where the worktree branched off the project's branch, even when that
    // has moved on since (e.g. another worktree was merged).
    GitRun base = Git(worktree, {"merge-base", "HEAD", Trim(head.out)});
    if (base.code != 0) {
        d.error = Failure(base);
        return d;
    }
    if (GitRun add = Git(worktree, {"add", "-A"}); add.code != 0) {
        d.error = Failure(add);
        return d;
    }
    GitRun stat = Git(worktree, {"diff", "--cached", "--no-color", "--stat", Trim(base.out)});
    GitRun diff = Git(worktree, {"diff", "--cached", "--no-color", "--no-ext-diff",
                                 Trim(base.out)});
    if (diff.code != 0) {
        d.error = Failure(diff);
        return d;
    }
    d.stat = Trim(stat.out);
    d.diff = std::move(diff.out);
    return d;
}

MergeOutcome MergeWorktree(const std::string& project, const std::string& worktree,
                           const std::string& branch, const std::string& commitMessage) {
    const std::string target = GitBranch(project);
    if (target.empty())
        return {false, "The project checkout isn't on a branch (detached HEAD). Check out "
                       "the branch to merge into, then merge again. Nothing was changed."};
    GitRun dirty = Git(project, {"status", "--porcelain", "--untracked-files=no"});
    if (dirty.code != 0) return {false, "Can't merge: " + Failure(dirty)};
    if (std::vector<std::string> files = Lines(dirty.out); !files.empty()) {
        for (std::string& f : files) f = f.substr(3);  // drop the "XY " status
        return {false, "The project checkout has uncommitted changes (" + FileList(files)
                           + "). Commit or stash them, then merge again. Nothing was changed."};
    }
    if (!fs::exists(worktree))
        return {false, "The worktree " + worktree + " no longer exists. Nothing was changed."};

    // The worktree's uncommitted work becomes a commit on its branch.
    bool committed = false;
    if (GitRun add = Git(worktree, {"add", "-A"}); add.code != 0)
        return {false, "Can't commit the worktree's changes: " + Failure(add)};
    if (Git(worktree, {"diff", "--cached", "--quiet"}).code == 1) {
        GitRun commit = Git(worktree, {"commit", "-q", "-m", commitMessage});
        if (commit.code != 0)
            return {false, "Committing the worktree's changes on " + branch + " failed: "
                               + Failure(commit) + "\nNothing was merged."};
        committed = true;
    }
    const std::string committedNote =
        committed ? "The worktree's uncommitted changes were committed on " + branch
                        + " as \"" + commitMessage + "\". "
                  : std::string();

    GitRun ahead = Git(project, {"rev-list", "--count", "HEAD.." + branch});
    if (ahead.code != 0) return {false, committedNote + "Can't merge: " + Failure(ahead)};
    if (std::atoi(ahead.out.c_str()) == 0)
        return {false, committedNote + "Nothing to merge: " + target + " already has "
                           "everything on " + branch + "."};

    GitRun merge = Git(project, {"merge", "--no-ff", "--no-edit", "-m",
                                 "Merge " + branch + " (Gritcode worktree thread)", branch});
    if (merge.code == 0)
        return {true, committedNote + "Merged " + branch + " into " + target + "."};

    // Refused before it started (e.g. it would overwrite untracked files):
    // nothing changed.
    if (Git(project, {"rev-parse", "-q", "--verify", "MERGE_HEAD"}).code != 0)
        return {false, committedNote + "Merging " + branch + " into " + target + " failed: "
                           + Failure(merge) + "\n" + target + " is unchanged."};

    // Stopped on conflicts: put the user's checkout back as it was.
    const std::string conflicts =
        FileList(Lines(Git(project, {"diff", "--name-only", "--diff-filter=U"}).out));
    const std::string what = "Merging " + branch + " into " + target + " hit conflicts"
                             + (conflicts.empty() ? std::string() : " in " + conflicts);
    if (GitRun abort = Git(project, {"merge", "--abort"}); abort.code != 0)
        return {false, committedNote + what + ", and git merge --abort failed: "
                           + Failure(abort) + "\nThe project checkout " + project
                           + " is still in the middle of the merge: resolve the conflicts "
                             "and commit, or run git merge --abort there."};
    return {false, committedNote + what + ". The merge was aborted; " + target
                       + " and the project checkout are as before. To bring the work in, "
                         "ask this thread to merge " + target + " into its branch and "
                         "resolve the conflicts in its worktree, then merge again."};
}

std::string RemoveWorktree(const std::string& project, const std::string& worktree,
                           const std::string& branch) {
    GitRun r = fs::exists(worktree)
                   ? Git(project, {"worktree", "remove", "--force", worktree})
                   : Git(project, {"worktree", "prune"});
    if (r.code != 0) return Failure(r);
    if (Git(project, {"rev-parse", "-q", "--verify", "refs/heads/" + branch}).code != 0)
        return {};  // already gone
    r = Git(project, {"branch", "-D", branch});
    return r.code == 0 ? std::string() : Failure(r);
}
