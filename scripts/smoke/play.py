#!/usr/bin/env python3
"""
Smoke test for Play runs and drafts across threads: one Play run per folder,
its output kept with the thread that pressed Play, and the input draft kept
per thread.

Usage:
    python3 scripts/smoke/play.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). The Play commands are silent shell commands
(sleep, echo) whose output differs from the command text.
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
from fake_model import serve  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
DATA = tempfile.mkdtemp(prefix="grit-play-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18900, 18949))
failures = 0


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    failures += not cond


def previews(g):
    return " ".join(b["preview"] for b in g.blocks())


P, Q = os.path.join(DATA, "P"), os.path.join(DATA, "Q")
os.makedirs(P)
os.makedirs(Q)
os.makedirs(os.path.join(DATA, "gritcode"))
with open(os.path.join(DATA, "gritcode", "run_configs.json"), "w") as f:
    json.dump({P: {"command": "sleep 4; echo P-$((40+2))", "discoveredBy": "user", "lastUsed": ""},
               Q: {"command": "sleep 1; echo Q-$((40+3))", "discoveredBy": "user", "lastUsed": ""}}, f)

env = dict(os.environ, GRIT_TEST_DATA=DATA)
pid = int(subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}",
                                   f"GRIT_FREE_MODEL_URL={serve()}"],
                                  env=env, stderr=subprocess.DEVNULL))
try:
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            break
        except OSError:
            time.sleep(0.2)
    g = Grit(port=PORT)

    print("-- Play in two folders")
    a = g.switch_session(P)["threadId"]
    g._call("setInput", {"text": "draft for P"})
    check(g._call("play").get("started"), "Play starts in P")
    q = g.switch_session(Q)["threadId"]
    check(g.status()["input"] == "", "P's draft doesn't follow to Q")
    check(g._call("play").get("started"), "Play starts in Q while P's run goes on")
    time.sleep(6)
    p = previews(g)
    check("Q-43" in p and "P-42" not in p, "Q's output is in Q's thread only")

    print("-- back in P")
    g.switch_session(P)
    check(g.status()["input"] == "draft for P", "P's draft is back")
    p = previews(g)
    check("Build and run" in p and "P-42" in p, "P's run finished, not stopped by Q's, and shows in P")
    check("Q-43" not in p, "Q's output isn't in P")

    print("-- threads of one folder")
    b = g.new_thread(P)["threadId"]
    check(g.status()["input"] == "" and "P-42" not in previews(g),
          "a new thread starts with an empty input and no Play output")
    g._call("setInput", {"text": "draft for b"})
    g.select_thread(a)
    check(g.status()["input"] == "draft for P", "each thread keeps its own draft")
    g.select_thread(b)
    check(g.status()["input"] == "draft for b", "and gets it back")
    g.switch_session(Q)
    check("Q-43" in previews(g), "Q's output is still there after switching away and back")
finally:
    os.kill(pid, 15)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
