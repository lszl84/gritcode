#!/usr/bin/env python3
"""
Smoke test for the project brief: written when a new thread starts in a
folder without one, reused by the folder's later new threads, and what goes
into a thread's system prompt.

Usage:
    python3 scripts/smoke/brief.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir), with the free model pointed at a local fake
(fake_model.py), which answers a brief request with the material it got,
and a stand-in `claude` on PATH for the Claude case.
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from grit import Grit  # noqa: E402
from fake_model import Handler, serve  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
DATA = tempfile.mkdtemp(prefix="grit-brief-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18800, 18899))
failures = 0


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    failures += not cond


def system_prompt(g, thread_id):
    g.select_thread(thread_id)
    return g.conversation()[0]["content"]


def previews(g):
    return " ".join(b["preview"] for b in g.blocks())


def wait_for(cond, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.2)
    return False


P, Q = os.path.join(DATA, "P"), os.path.join(DATA, "Q")
os.makedirs(P)
os.makedirs(Q)
with open(os.path.join(P, "AGENTS.md"), "w") as f:
    f.write("agents-marker: run make test\n")
with open(os.path.join(P, "README.md"), "w") as f:
    f.write("readme-marker: a C++ chat app.\n")
# The fake model answers this folder's brief request with HTTP 500.
with open(os.path.join(Q, "README.md"), "w") as f:
    f.write("FAIL-500\n")
# Folder R is worked on with Claude: its brief must go through the Claude Code
# CLI (here a stand-in that records what it got), never to the HTTP model.
R = os.path.join(DATA, "R")
os.makedirs(R)
with open(os.path.join(R, "README.md"), "w") as f:
    f.write("claude-readme-marker\n")
FAKEBIN = os.path.join(DATA, "fakebin")
os.makedirs(FAKEBIN)
with open(os.path.join(FAKEBIN, "claude"), "w") as f:
    f.write("""#!/bin/bash
d=$(dirname "$0")
printf '%s\\n' "$@" > "$d/args.txt"
cat > "$d/stdin.txt"
python3 -c 'import json,sys; print(json.dumps({"type": "result", "is_error": False,
    "result": "Claude brief. " + open(sys.argv[1]).read().splitlines()[0]}))' "$d/stdin.txt"
""")
os.chmod(os.path.join(FAKEBIN, "claude"), 0o755)
STUBS = os.path.expanduser("~/.local/share/grit-dev/stubs")
# Folder P has two threads; the first has no id, so gritcode derives it from
# the folder, which also names the folder's brief file.
os.makedirs(os.path.join(DATA, "gritcode"))
with open(os.path.join(DATA, "gritcode", "sessions.json"), "w") as f:
    json.dump({"sessions": [{"cwd": P, "lastUsed": "2026-10-18T10:00:00",
                             "title": "Fix the SSE parser"}],
               "threads": [{"id": "00000000000000aa", "cwd": P,
                            "lastUsed": "2026-10-19T09:00:00",
                            "title": "Add dark mode"}],
               "lastActiveCwd": P}, f)
BRIEFS = os.path.join(DATA, "gritcode", "briefs")

env = dict(os.environ, GRIT_TEST_DATA=DATA)
pid = int(subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}",
                                   f"GRIT_FREE_MODEL_URL={serve()}",
                                   f"PATH={FAKEBIN}:{STUBS}:{os.environ['PATH']}"],
                                  env=env, stderr=subprocess.DEVNULL))
try:
    # Wait for this instance's own port (Grit would also try the next ones).
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.2)
    g = Grit(port=PORT)
    threads = g.list_threads()["threads"]
    first = next(t["id"] for t in threads if t["cwd"] == P and t["id"] != "00000000000000aa")
    brief_file = os.path.join(BRIEFS, first + ".md")

    print("-- no brief")
    sp = system_prompt(g, "00000000000000aa")
    check("agents-marker" in sp, "AGENTS.md is still injected")
    check("Fix the SSE parser (last used 2026-10-18)" in sp, "the other thread is listed")
    check("Add dark mode" not in sp, "the thread itself is not listed")
    check("Project brief" not in sp, "no brief section")
    check("project brief" not in previews(g), "opening a thread writes no brief")

    print("-- a new thread writes it")
    fresh = g.new_thread(P)["threadId"]
    check("Writing a project brief for this folder with" in previews(g),
          "the new thread says a brief is being written, and by which model")
    check(wait_for(lambda: os.path.exists(brief_file)), "the brief is saved")
    brief = open(brief_file).read() if os.path.exists(brief_file) else ""
    check(brief.startswith("Fake brief.") and "readme-marker" in brief
          and "agents-marker" in brief and "Fix the SSE parser" in brief,
          "it was written from the README, AGENTS.md and the thread titles")
    check(wait_for(lambda: "starts with the folder's project brief" in previews(g)
                   and "Writing a project brief" not in previews(g)),
          "the thread then shows the brief it starts with")
    sp = system_prompt(g, fresh)
    check("Fake brief." in sp and "agents-marker" in sp, "the brief is in its system prompt")
    check("Fix the SSE parser" in sp and "Add dark mode" in sp, "it lists both threads")

    print("-- the next new thread reuses it")
    mtime = os.path.getmtime(brief_file)
    g.new_thread(P)
    p = previews(g)
    check("starts with the folder's project brief" in p and "Writing" not in p,
          "it starts with the saved brief right away")
    time.sleep(2)
    check(os.path.getmtime(brief_file) == mtime, "the brief is not written again")

    print("-- a failed brief")
    q = g.switch_session(Q)["threadId"]
    sp = system_prompt(g, q)
    check("Fake brief" not in sp and "Other threads" not in sp, "nothing from P")
    g.new_thread(Q)
    check(wait_for(lambda: "couldn't write the project brief" in previews(g)),
          "the failure is shown")
    check("scripted failure" in previews(g), "with the model's error")
    check(os.listdir(BRIEFS) == [first + ".md"], "no brief is saved for the folder")

    print("-- Claude writes a Claude user's brief")
    g.switch_session(R)
    # Dropdown row 3: after Kilo Free and the two fallback DeepSeek models.
    check(g.set_model(3).get("modelIndex") == 100, "Claude (Opus) is selected")
    g.new_thread(R)
    check(wait_for(lambda: "Writing a project brief for this folder with Claude (Sonnet"
                   in previews(g) or len(os.listdir(BRIEFS)) == 2),
          "it is written by Claude's Sonnet")
    check(wait_for(lambda: len(os.listdir(BRIEFS)) == 2), "the brief is saved")
    saved = [n for n in os.listdir(BRIEFS) if n != first + ".md"]
    brief = open(os.path.join(BRIEFS, saved[0])).read() if saved else ""
    check(brief == "Claude brief. Folder: " + R, f"from the CLI's result ({brief[:60]})")
    args = open(os.path.join(FAKEBIN, "args.txt")).read().split("\n")
    check(args[:1] == ["-p"] and "claude-sonnet-5" in args, "a print-mode run on Sonnet")
    check("--tools" in args and args[args.index("--tools") + 1] == ""
          and "--no-session-persistence" in args, "with no tools, not saved as a session")
    check("claude-readme-marker" in open(os.path.join(FAKEBIN, "stdin.txt")).read(),
          "the material goes in on stdin")
    check(not any(R in json.dumps(m) for m in Handler.requests),
          "nothing about the folder reached the other model")
finally:
    os.kill(pid, 15)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
