#!/usr/bin/env python3
"""
Smoke test for project-scoped history search (grit_history_search).

Usage:
    python3 scripts/smoke/history.py build/gritcode

Seeds a temp data dir with an old-format memory.db (the 0.5.x schema, one
turn from a session that has no file any more) and four threads' session
files, in two folders. Then checks, all against that temp dir:
- `gritcode --mcp-stdio` opens the old DB and finds the old turn;
- `gritcode --reindex` indexes every thread under its own id, with title
  and parent, and keeps the old turn;
- the old version's own SQL still works on the result;
- in the app (hidden, fake model), scope "folder" keeps to the folder,
  the calling thread is left out and hits carry titles.
The CLI modes run with HOME and XDG_DATA_HOME in the temp dir and no display;
the app runs only through ~/.local/share/grit-dev/run-hidden.sh.
"""

import json
import os
import random
import socket
import sqlite3
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
DATA = tempfile.mkdtemp(prefix="grit-history-")
PORT = int(os.environ.get("GRIT_MCP_PORT") or random.randint(18700, 18799))
ROOT = os.path.join(DATA, "gritcode")
DB = os.path.join(ROOT, "memory.db")
failures = 0

# The schema and queries 0.5.x uses (git show main:src/memory.cpp).
OLD_SCHEMA = ("CREATE VIRTUAL TABLE IF NOT EXISTS turns USING fts5(cwd, text, "
              "role UNINDEXED, session_id UNINDEXED, turn_index UNINDEXED, "
              "timestamp UNINDEXED, tokenize = 'porter unicode61')")
OLD_INSERT = ("INSERT INTO turns(cwd, text, role, session_id, turn_index, timestamp) "
              "VALUES (?, ?, ?, ?, ?, ?)")
OLD_SEARCH = ("SELECT cwd, role, snippet(turns, 1, '[', ']', '…', 20), session_id, "
              "turn_index, timestamp, bm25(turns), length(text) FROM turns "
              "WHERE turns MATCH ? ORDER BY bm25(turns) LIMIT ?")


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    failures += not cond


P, Q = os.path.join(DATA, "P"), os.path.join(DATA, "Q")
A, B, C, D = "00000000000000a1", "00000000000000b1", "00000000000000c1", "00000000000000d1"
for d in (P, Q, os.path.join(ROOT, "sessions"), os.path.join(DATA, "home")):
    os.makedirs(d)


def session(sid, cwd, title, text, parent=""):
    j = {"id": sid, "cwd": cwd, "lastUsed": "2026-10-19T10:00:00", "title": title,
         "messages": [{"role": "system", "content": "sys"},
                      {"role": "user", "content": text},
                      {"role": "assistant", "content": "noted"}]}
    if parent:
        j["parent"] = parent
    with open(os.path.join(ROOT, "sessions", sid + ".json"), "w") as f:
        json.dump(j, f)


session(A, P, "Fix the SSE parser", "zebracorn mentioned in thread A")
session(B, P, "Add dark mode", "zebracorn colours in thread B")
session(C, P, "Look around", "zebracorn found by the subagent", parent=B)
session(D, Q, "Other project", "zebracorn in another folder")

old = sqlite3.connect(DB)
old.execute(OLD_SCHEMA)
old.execute(OLD_INSERT, (P, "zebracorn from an old version", "user", "0ld0000000000000", 1,
                         "2025-01-01T00:00:00"))
old.commit()
old.close()

CLI_ENV = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
CLI_ENV.update(HOME=os.path.join(DATA, "home"), XDG_DATA_HOME=DATA)


def stdio_search(args):
    """One grit_history_search call through `gritcode --mcp-stdio`, run in P."""
    reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
             "params": {"name": "grit_history_search", "arguments": args}}]
    out = subprocess.run([BIN, "--mcp-stdio"], cwd=P, env=CLI_ENV, timeout=30,
                         input="".join(json.dumps(r) + "\n" for r in reqs),
                         capture_output=True, text=True).stdout
    return json.loads(out.splitlines()[-1])["result"]["content"][0]["text"]


print("-- old-format memory.db")
r = stdio_search({"query": "zebracorn", "scope": "folder"})
check("0ld0000000000000" in r and "thread:" not in r, "opens and finds the old turn, no title")
tables = {n for (n,) in sqlite3.connect(DB).execute("SELECT name FROM sqlite_master")}
check("threads" in tables, "the threads table was added")

print("-- reindex")
out = subprocess.run([BIN, "--reindex"], env=CLI_ENV, timeout=60,
                     capture_output=True, text=True).stdout
check("4 sessions indexed" in out, "every thread's session file is indexed")
db = sqlite3.connect(DB)
sids = {s for (s,) in db.execute("SELECT session_id FROM turns")}
check(sids == {A, B, C, D, "0ld0000000000000"}, "each under its own id, the old turn kept")
rows = dict(((s, (t, p)) for s, t, p in db.execute("SELECT * FROM threads")))
check(rows.get(C) == ("Look around", B) and rows.get(A) == ("Fix the SSE parser", ""),
      "titles and parents recorded")
db.execute(OLD_SCHEMA)
db.execute(OLD_INSERT, (Q, "yakwhistle", "user", "0ld0000000000001", 0, ""))
check(len(db.execute(OLD_SEARCH, ("zebracorn", 20)).fetchall()) == 5
      and len(db.execute(OLD_SEARCH, ("yakwhistle", 20)).fetchall()) == 1,
      "0.5.x's own schema statement, insert and search still work")
db.rollback()
db.close()
r = stdio_search({"query": "zebracorn", "scope": "folder", "limit": 20})
check(D not in r and "thread: Look around (subagent)" in r, "stdio: folder scope and titles")

print("-- in the app")
env = dict(os.environ, GRIT_TEST_DATA=DATA)
pid = int(subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}",
                                   f"GRIT_FREE_MODEL_URL={serve(0.2)}"],
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
    g.select_thread(A)
    g.send("Call these tools now: " + json.dumps([
        ["grit_history_search", {"query": "zebracorn", "scope": "folder", "limit": 20}],
        ["grit_history_search", {"query": "zebracorn", "limit": 20}]]))
    end = time.time() + 60
    while time.time() < end and (time.sleep(0.5) or any(
            t["running"] for t in g.list_threads()["threads"])):
        pass
    results = [m["content"] for m in g.conversation() if m["role"] == "tool"]
    check(len(results) == 2, "both searches ran")
    folder, everywhere = (results + ["", ""])[:2]
    check(B in folder and C in folder and "0ld0000000000000" in folder,
          "folder scope finds the folder's other threads and old turns")
    check(D not in folder, "folder scope leaves out the other folder")
    check(A not in folder and A not in everywhere, "the calling thread is left out")
    check("thread: Add dark mode\n" in folder and "thread: Look around (subagent)" in folder,
          "hits carry the thread title and mark subagents")
    check(D in everywhere and "thread: Other project" in everywhere,
          "default scope searches every folder")
    time.sleep(1)  # the save runs on a worker after the turn
    db = sqlite3.connect(DB)
    saved = db.execute("SELECT count(*) FROM turns WHERE session_id = ? AND text LIKE "
                       "'Call these tools now%'", (A,)).fetchone()[0]
    check(saved == 1 and db.execute("SELECT title FROM threads WHERE session_id = ?",
                                    (A,)).fetchone() == ("Fix the SSE parser",),
          "the app indexes the thread's turn under its own id, with its title")
    db.close()
finally:
    os.kill(pid, 15)

print("all ok" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
