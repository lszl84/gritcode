#!/usr/bin/env python3
"""
Smoke test for the thread pane: its rows, toggle and remembered state.

Usage:
    python3 scripts/smoke/thread_pane.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). The folder's threads are seeded into the index
before launch; the live states come from turns that are cancelled right away.
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
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from grit import Grit  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
DATA = tempfile.mkdtemp(prefix="grit-pane-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18800, 18899))
CONF = os.path.join(DATA, "home", ".gritcode", "gritcode.conf")
failures = 0


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    failures += not cond


def panes(g):
    return g._call("getPanes")


def rows(g):
    return {r["id"]: r for r in panes(g)["threadRows"]}


def day(iso):
    d = datetime.fromisoformat(iso)
    return d.strftime("%b %d") if d.year == datetime.now().year else d.date().isoformat()


P, Q = os.path.join(DATA, "P"), os.path.join(DATA, "Q")
os.makedirs(P)
os.makedirs(Q)
os.makedirs(os.path.join(DATA, "gritcode"))
with open(os.path.join(DATA, "gritcode", "sessions.json"), "w") as f:
    json.dump({"sessions": [{"cwd": P, "lastUsed": "2026-10-18T10:00:00",
                             "title": "Fix the SSE parser"},
                            {"cwd": Q, "lastUsed": "2026-10-17T10:00:00",
                             "title": "Other folder"}],
               "threads": [{"id": "00000000000000aa", "cwd": P,
                            "lastUsed": "2025-10-19T09:00:00",
                            "title": "Add dark mode"},
                           {"id": "00000000000000ab", "cwd": P,
                            "lastUsed": "2026-10-19T09:30:00",
                            "title": "Read the theme code",
                            "parent": "00000000000000aa"}],
               "lastActiveCwd": P}, f)
# The pane was left open last time.
os.makedirs(os.path.dirname(CONF))
with open(CONF, "w") as f:
    f.write("[UI]\nShowThreadPane=1\n")

env = dict(os.environ, GRIT_TEST_DATA=DATA)
pid = int(subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}"],
                                  env=env, stderr=subprocess.DEVNULL))
try:
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.2)
    g = Grit(port=PORT)

    print("-- rows")
    p = panes(g)
    check(p["threads"] > 0, "the remembered pane is open at startup")
    order = [r["id"] for r in p["threadRows"]]
    first = order[0] if order else ""
    check(order[1:] == ["00000000000000aa", "00000000000000ab"],
          "most recent first, the subagent under its parent")
    r = rows(g)
    check(r["00000000000000ab"]["child"] and not r["00000000000000aa"]["child"],
          "only the subagent is nested")
    check(r[first]["detail"] == day("2026-10-18T10:00:00")
          and r["00000000000000aa"]["detail"] == day("2025-10-19T09:00:00"),
          "idle rows say when they were last used")
    check(len(order) == 3, "other folders' threads are not listed")

    print("-- live state")
    a = g.new_thread(P)["threadId"]
    check(next(iter(rows(g))) == a, "a new thread is listed first")
    g.send("Say hello.")
    time.sleep(0.3)
    step = rows(g)[a]["detail"]
    check(step in ("thinking", "writing"), f"a running row shows its step ({step})")
    b = g.new_thread(P)["threadId"]
    g.send("Say bye.")
    time.sleep(0.3)
    if any(t["id"] == a and t["running"] for t in g.list_threads()["threads"]):
        detail = rows(g)[b]["detail"]
        check(detail.startswith("waiting for "), f"a queued row says what it waits for ({detail})")
    else:
        print("skip  the first turn already ended; no queued row to check")
    g.cancel()
    g.select_thread(a)
    g.cancel()
    g.wait_idle(timeout=30, poll=0.2)
    detail = rows(g)[a]["detail"]
    check(":" in detail, f"a stopped row shows its time again ({detail})")

    print("-- toggle")
    before = panes(g)
    g._call("togglePanel", {"panel": "threads"})
    time.sleep(0.5)
    after = panes(g)
    check(after["threads"] == 0, "toggling hides the pane")
    check(after["chat"] == before["chat"], "the chat pane keeps its width")
    check(after["window"] < before["window"], "the window shrinks")
    check("ShowThreadPane=0" in open(CONF).read(), "the closed pane is remembered")
    g._call("togglePanel", {"panel": "threads"})
    time.sleep(0.5)
    check(panes(g)["threads"] > 0 and len(rows(g)) >= 5, "toggling shows it again, filled")

    print("-- another folder")
    q = g.switch_session(Q)["threadId"]
    check(list(rows(g)) == [q], "the pane follows the active folder")
finally:
    os.kill(pid, 15)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
