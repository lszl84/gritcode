#!/usr/bin/env python3
"""
Smoke test for worktree threads, in a temp git repo with the fake model:
a worktree thread writes alongside the folder's main thread, review shows its
diff, merge brings it in, a conflicting merge aborts cleanly, discard asks
before losing work, and a worktree removed by hand is reported.

Usage:
    python3 scripts/smoke/worktrees.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir).
"""

import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..")]
from fake_model import serve  # noqa: E402
from grit import Grit  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
MODEL_URL = serve()
DATA = tempfile.mkdtemp(prefix="grit-worktrees-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18600, 18699))
SESSIONS = os.path.join(DATA, "gritcode", "sessions")
REPO = os.path.join(DATA, "repo")
failures = 0


def launch():
    env = dict(os.environ, GRIT_TEST_DATA=DATA)
    out = subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}",
                                   f"GRIT_FREE_MODEL_URL={MODEL_URL}"],
                                  env=env, stderr=subprocess.DEVNULL)
    # Wait for this instance's own port before sending anything.
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            return int(out), Grit(port=PORT)
        except OSError:
            time.sleep(0.2)
    sys.exit("gritcode did not start")


def stop(pid):
    os.kill(pid, 15)
    for _ in range(100):
        try:
            os.kill(pid, 0)
            time.sleep(0.1)
        except ProcessLookupError:
            return


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    failures += not cond


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args], capture_output=True, text=True).stdout.strip()


def write(path, text):
    with open(path, "w") as f:
        f.write(text + "\n")


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def threads(g):
    return {t["id"]: t for t in g.list_threads()["threads"]}


def wait_idle(g, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        if not any(t["running"] or t["waiting"] for t in threads(g).values()):
            return True
        time.sleep(0.5)
    return False


def bash(cmd):
    return f"Call the bash tool now with exactly this command: {cmd}"


def previews(g):
    return " ".join(b["preview"] for b in g.blocks())


os.makedirs(REPO)
git("init", "-q", "-b", "main")
git("config", "user.name", "Smoke Test")
git("config", "user.email", "smoke@example.invalid")
write(os.path.join(REPO, "README"), "readme")
write(os.path.join(REPO, "shared.txt"), "base")
git("add", "-A")
git("commit", "-q", "-m", "initial")

pid, g = launch()
try:
    g.set_model(0)
    main = g.switch_session(REPO)["threadId"]

    print("-- creating a worktree thread")
    w = g.new_thread(REPO, worktree=True)["threadId"]
    t = threads(g)[w]
    wt, branch = t["worktree"], t["branch"]
    check(branch.startswith("grit/") and wt.startswith(os.path.join(DATA, "gritcode", "worktrees")),
          "it has a branch and a worktree outside the repo")
    check(os.path.isdir(wt) and wt in git("worktree", "list"), "git knows the worktree")
    check(t["cwd"] == REPO, "it belongs to the project folder")
    check(f"isolated git worktree of {REPO}" in g.conversation()[0]["content"]
          and branch in g.conversation()[0]["content"], "its system prompt says so")
    check("Review changes" in previews(g), "its canvas offers the actions")
    with open(os.path.join(SESSIONS, w + ".json")) as f:
        saved = json.load(f)
    check(saved.get("worktree") == wt and saved.get("branch") == branch,
          "the session file records worktree and branch")
    g._call("togglePanel", {"panel": "threads"})
    rows = {r["id"]: r for r in g._call("getPanes")["threadRows"]}
    check(branch in rows[w]["detail"], "its pane row shows the branch")

    print("-- writing alongside the folder's main thread")
    g.select_thread(main)
    g.send(bash("sleep 3; echo main > main.txt"))
    g.select_thread(w)
    g.send(bash("sleep 3; pwd > wt_ran.txt; echo feature > feature.txt"))
    now = threads(g)
    check(now[main]["running"] and now[w]["running"] and not now[w]["waiting"],
          "both run at the same time")
    check(wait_idle(g), "both turns finished")
    check(read(os.path.join(REPO, "main.txt")) == "main", "the main thread wrote in the repo")
    check(read(os.path.join(wt, "wt_ran.txt")) == wt and read(os.path.join(wt, "feature.txt")) == "feature",
          "the worktree thread wrote in its worktree")
    check(not os.path.exists(os.path.join(REPO, "feature.txt")), "the repo doesn't have its file yet")

    print("-- review")
    r = g.worktree_action("review")["notice"]
    check("feature.txt" in r and "+feature" in r, "review lists the new file and its diff")
    check(any(b["type"] == "code" and "+feature" in b["preview"] for b in g.blocks()),
          "the diff shows in the canvas")

    print("-- merge")
    head = git("rev-parse", "HEAD")
    r = g.worktree_action("merge")["notice"]
    check(f"Merged {branch} into main" in r and "committed" in r, "merge reports what it did")
    check(read(os.path.join(REPO, "feature.txt")) == "feature", "the change is in the repo")
    check(git("rev-parse", "HEAD^1") == head and git("log", "-1", "--format=%s").startswith("Merge"),
          "as a merge commit on main")

    print("-- merge refused while the repo has uncommitted changes")
    write(os.path.join(REPO, "README"), "edited by the user")
    head = git("rev-parse", "HEAD")
    r = g.worktree_action("merge")["notice"]
    check("uncommitted changes" in r and "README" in r, "it says why")
    check(git("rev-parse", "HEAD") == head and read(os.path.join(REPO, "README")) == "edited by the user",
          "nothing changed")
    git("checkout", "README")

    print("-- a conflicting merge")
    write(os.path.join(REPO, "shared.txt"), "project side")
    git("commit", "-q", "-am", "project side")
    g.send(bash("echo worktree side > shared.txt"))
    check(wait_idle(g), "the turn finished")
    head = git("rev-parse", "HEAD")
    r = g.worktree_action("merge")["notice"]
    check("conflicts in shared.txt" in r and "aborted" in r, "it reports the conflict and the abort")
    check(any(b["preview"].startswith(r[:100]) for b in g.blocks()), "the notice shows in the canvas")
    check(git("rev-parse", "HEAD") == head and git("status", "--porcelain", "--untracked-files=no") == ""
          and read(os.path.join(REPO, "shared.txt")) == "project side",
          "the repo is as before")
    check(subprocess.run(["git", "-C", REPO, "rev-parse", "-q", "--verify", "MERGE_HEAD"],
                         capture_output=True).returncode != 0, "no merge in progress")

    print("-- discard")
    r = g.worktree_action("discard")
    check(not r["ok"] and "not merged" in r["asked"], "it asks first (unmerged commit)")
    check(os.path.isdir(wt) and git("branch", "--list", branch) != "", "nothing removed without a yes")
    r = g.worktree_action("discard", confirm=True)
    check(r["ok"] and not os.path.exists(wt) and git("branch", "--list", branch) == "",
          "a yes removes worktree and branch")
    check(threads(g)[w]["worktree"] == "" and "Review changes" not in previews(g),
          "the thread is a plain thread now")

    print("-- a worktree removed by hand")
    w2 = g.new_thread(REPO, worktree=True)["threadId"]
    wt2, branch2 = threads(g)[w2]["worktree"], threads(g)[w2]["branch"]
    stop(pid)
    shutil.rmtree(wt2)
    pid, g = launch()
    check(threads(g)[w2]["worktree"] == wt2, "worktree threads are restored")
    g.select_thread(w2)
    check("is gone" in previews(g), "its canvas says the worktree is gone")
    g.send("hello")
    time.sleep(0.5)
    check(not threads(g)[w2]["running"] and "can't run" in previews(g), "it doesn't run")
    r = g.worktree_action("discard")
    check(r["ok"] and r["asked"] == "", "discarding needs no question (no work)")
    check(wt2 not in git("worktree", "list") and git("branch", "--list", branch2) == "",
          "the stale worktree is pruned and its branch deleted")
finally:
    stop(pid)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
