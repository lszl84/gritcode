#!/usr/bin/env python3
"""
Smoke test for threads: drives a hidden gritcode over the control API.

Usage:
    python3 scripts/smoke/threads.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). Turns use the free model (Kilo Free), so a run
can fail on model errors (HTTP 503) rather than on gritcode.
"""

import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from grit import Grit  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
DATA = tempfile.mkdtemp(prefix="grit-threads-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18800, 18899))
INDEX = os.path.join(DATA, "gritcode", "sessions.json")
SESSIONS = os.path.join(DATA, "gritcode", "sessions")
failures = 0


def launch():
    env = dict(os.environ, GRIT_TEST_DATA=DATA)
    out = subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}"],
                                  env=env, stderr=subprocess.DEVNULL)
    pid, g = int(out), Grit(base_port=PORT)
    for _ in range(100):
        try:
            g.status()
            return pid, g
        except (ConnectionError, OSError):
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


def threads(g):
    return {t["id"]: t for t in g.list_threads()["threads"]}


def wait_idle(g, timeout=240):
    end = time.time() + timeout
    while time.time() < end:
        if not any(t["running"] or t["waiting"] for t in threads(g).values()):
            return True
        time.sleep(1)
    return False


def history(g, thread_id):
    g.select_thread(thread_id)
    return json.dumps(g.conversation())


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def bash(cmd):
    return f"Call the bash tool now with exactly this command: {cmd}"


A, B = os.path.join(DATA, "A"), os.path.join(DATA, "B")
os.makedirs(A)
os.makedirs(B)
pid, g = launch()
g.set_model(0)

print("-- two threads in different folders at once")
a = g.switch_session(A)["threadId"]
g.send(bash("sleep 3; pwd > ran_here.txt; echo marker-A"))
b = g.switch_session(B)["threadId"]
g.send(bash("sleep 3; pwd > ran_here.txt; echo marker-B"))
now = threads(g)
check(now[a]["running"] and now[b]["running"], "both threads run at the same time")
check(wait_idle(g), "both turns finished")
check(read(os.path.join(A, "ran_here.txt")) == A, "A's bash ran in A")
check(read(os.path.join(B, "ran_here.txt")) == B, "B's bash ran in B")
ha, hb = history(g, a), history(g, b)
check("marker-A" in ha and "marker-B" not in ha, "A's history has only A's messages")
check("marker-B" in hb and "marker-A" not in hb, "B's history has only B's messages")

print("-- two threads in the same folder")
g.select_thread(b)
g.send(bash("sleep 6; date +%s.%N > first_done"))
time.sleep(1)
b2 = g.new_thread(B)["threadId"]
g.send(bash("date +%s.%N > second_start"))
now = threads(g)
check(now[b]["running"] and now[b2]["waiting"], "the second thread waits")
check(any("Waiting for" in blk["preview"] for blk in g.blocks()), "waiting notice shown")
check(wait_idle(g), "both turns finished")
first, second = read(os.path.join(B, "first_done")), read(os.path.join(B, "second_start"))
check(bool(first and second) and float(second) > float(first),
      "the second ran after the first finished")

print("-- opening a folder")
g.switch_session(A)
check(g.switch_session(B).get("threadId") == b2, "it shows the folder's most recent thread")

print("-- a turn that fails in the background")
g.select_thread(b)
g.send(bash("sleep 8"))  # keeps the folder busy
x = g.new_thread(B)["threadId"]
g.set_model(1)  # a DeepSeek model: needs a key, and this HOME has none
g.send("remember this prompt")
check(threads(g)[x]["waiting"], "it waits for the folder")
g.select_thread(b)
g.cancel()  # b ends; x starts in the background and fails without a key
end = time.time() + 60
while time.time() < end and threads(g)[x]["waiting"]:
    time.sleep(0.5)
g.set_model(0)
now = threads(g)
check(now[x]["failed"] and not now[x]["running"], "it failed in the background")
g.select_thread(x)
check(any("No API key" in blk["preview"] for blk in g.blocks()), "its notice shows when viewed")
check(g.status()["input"] == "remember this prompt", "its prompt is back in the input")
check(not threads(g)[x]["failed"], "it is no longer marked as failed")

print("-- restart")
stop(pid)
pid, g = launch()
now = threads(g)
check({a, b, b2, x} <= now.keys(), "threads restored")
check(g.status()["threadId"] == x, "last active thread restored")

check(now[b2]["title"].startswith("Call the bash tool"), "titles restored")

print("-- index rewritten by 0.5.x (no threads array)")
stop(pid)
with open(INDEX) as f:
    index = json.load(f)
old = {"sessions": [{k: e[k] for k in ("id", "cwd", "lastUsed")} for e in index["sessions"]],
       "lastActiveCwd": index["lastActiveCwd"]}
with open(INDEX, "w") as f:
    json.dump(old, f, indent=2)
with open(INDEX, "rb") as f:
    before = f.read()
pid, g = launch()
now = threads(g)
check(b2 in now and now[b2]["cwd"] == B, "threads missing from the index are listed again")
check(g.status()["threadId"] == b, "opens the last folder's first thread")
with open(INDEX, "rb") as f:
    check(f.read() == before, "index not rewritten at startup")

print("-- old-format data (one session per folder)")
stop(pid)
first_ids = {e["id"] for e in old["sessions"]}
for name in os.listdir(SESSIONS):
    if name.removesuffix(".json") not in first_ids:
        os.remove(os.path.join(SESSIONS, name))
pid, g = launch()
check(threads(g).keys() == first_ids, "each folder's session is its first thread")
check(g.status()["historyLen"] > 1, "its history loaded")
with open(INDEX, "rb") as f:
    check(f.read() == before, "nothing rewritten")
stop(pid)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
