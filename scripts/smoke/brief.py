#!/usr/bin/env python3
"""
Smoke test for the project brief: what goes into a thread's system prompt.

Usage:
    python3 scripts/smoke/brief.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). No model turns: the folder's threads are seeded
into the index before launch.
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


def hint_shown(g):
    return any("A project brief tells" in b["preview"] for b in g.blocks())


P, Q = os.path.join(DATA, "P"), os.path.join(DATA, "Q")
os.makedirs(P)
os.makedirs(Q)
with open(os.path.join(P, "AGENTS.md"), "w") as f:
    f.write("agents-marker: run make test\n")
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

env = dict(os.environ, GRIT_TEST_DATA=DATA)
pid = int(subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}"],
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

    print("-- no brief")
    sp = system_prompt(g, "00000000000000aa")
    check("agents-marker" in sp, "AGENTS.md is still injected")
    check("Fix the SSE parser (last used 2026-10-18)" in sp, "the other thread is listed")
    check("Add dark mode" not in sp, "the thread itself is not listed")
    check("Project brief" not in sp, "no brief section")
    fresh = g.new_thread(P)["threadId"]
    check(hint_shown(g), "a new thread offers a brief")
    sp = system_prompt(g, fresh)
    check("Fix the SSE parser" in sp and "Add dark mode" in sp, "a new thread lists both threads")

    print("-- with a brief")
    os.makedirs(os.path.join(DATA, "gritcode", "briefs"), exist_ok=True)
    with open(os.path.join(DATA, "gritcode", "briefs", first + ".md"), "w") as f:
        f.write("brief-marker: a C++ chat app.")
    fresh = g.new_thread(P)["threadId"]
    check(not hint_shown(g), "no hint once the folder has a brief")
    sp = system_prompt(g, fresh)
    check("brief-marker: a C++ chat app." in sp, "the brief is injected")
    check("agents-marker" in sp, "together with AGENTS.md")

    print("-- another folder")
    q = g.switch_session(Q)["threadId"]
    sp = system_prompt(g, q)
    check("brief-marker" not in sp and "Other threads" not in sp, "nothing from P")
    g.new_thread(Q)
    check(not hint_shown(g), "no hint without other threads")
finally:
    os.kill(pid, 15)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
