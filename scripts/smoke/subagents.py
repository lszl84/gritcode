#!/usr/bin/env python3
"""
Smoke test for subagents: drives a hidden gritcode over the control API.

Usage:
    python3 scripts/smoke/subagents.py build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). Claude Code subagents are checked against a
fake `claude` that replays a few events in the shape the real CLI sends. The
`task` tool needs a live model: the free one (Kilo Free) is flaky, so those
results are informational, and DeepSeek is skipped without an API key.
"""

import json
import os
import random
import shutil
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from grit import Grit  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
STUBS = os.path.expanduser("~/.local/share/grit-dev/stubs")
BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/gritcode")
DATA = tempfile.mkdtemp(prefix="grit-subagents-")
FAKE = os.path.join(DATA, "fakebin")
PORT = random.randint(18900, 18999)
CLAUDE_ROW = 3  # Kilo Free, two DeepSeek fallback models, then Claude
failures = 0

FAKE_CLAUDE = """#!/usr/bin/env python3
import json, os, sys, time
sys.stdin.readline()
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "events.jsonl")) as f:
    for line in f:
        ev = json.loads(line)
        if "sleep" in ev:
            time.sleep(ev["sleep"])
            continue
        print(json.dumps(ev), flush=True)
        time.sleep(0.05)
"""


def launch():
    env = dict(os.environ, GRIT_TEST_DATA=DATA)
    path = f"PATH={FAKE}:{STUBS}:{os.environ['PATH']}"
    out = subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}", path],
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


def info(cond, what):
    print(("ok    " if cond else "info  not: ") + what)


def threads(g):
    return {t["id"]: t for t in g.list_threads()["threads"]}


def children(g, parent):
    return [t for t in threads(g).values() if t["parent"] == parent]


def wait_idle(g, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if not any(t["running"] or t["waiting"] for t in threads(g).values()):
            return True
        time.sleep(0.5)
    return False


def wait_for(cond, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.2)
    return False


def text(g):
    return json.dumps(g.conversation())


def blocks(g):
    return " | ".join(b["preview"] for b in g.blocks())


# ---- fake Claude Code events ----

def assistant(msg_id, block, parent=None):
    return {"type": "assistant", "parent_tool_use_id": parent,
            "message": {"id": msg_id, "role": "assistant", "content": [block]}}


def tool_use(tool_id, name, inp):
    return {"type": "tool_use", "id": tool_id, "name": name, "input": inp}


def tool_result(tool_id, content, parent=None, background=False):
    ev = {"type": "user", "parent_tool_use_id": parent,
          "message": {"role": "user", "content": [
              {"type": "tool_result", "tool_use_id": tool_id, "content": content}]}}
    if background:
        ev["tool_use_result"] = {"isAsync": True, "status": "async_launched"}
    return ev


INIT = {"type": "system", "subtype": "init", "session_id": "fake-1", "model": "claude-opus-5-5"}
RESULT = {"type": "result", "subtype": "success", "is_error": False,
          "result": "done", "session_id": "fake-1"}


def scenario(events):
    with open(os.path.join(FAKE, "events.jsonl"), "w") as f:
        f.writelines(json.dumps(e) + "\n" for e in events)


os.makedirs(FAKE)
with open(os.path.join(FAKE, "claude"), "w") as f:
    f.write(FAKE_CLAUDE)
os.chmod(os.path.join(FAKE, "claude"), stat.S_IRWXU)
A = os.path.join(DATA, "A")
os.makedirs(A)
pid, g = launch()
top = g.switch_session(A)["threadId"]
check(g.set_model(CLAUDE_ROW).get("modelIndex") == 100, "Claude model selected")

print("-- Claude Code subagents")
scenario([
    INIT,
    assistant("m1", {"type": "text", "text": "Delegating."}),
    assistant("m1", tool_use("A1", "Agent", {"description": "Count files",
                                             "prompt": "List the files and count them.",
                                             "subagent_type": "general-purpose"})),
    tool_result("A1", "Async agent launched successfully.", background=True),
    assistant("m2", tool_use("B1", "Bash", {"command": "ls"}), parent="A1"),
    tool_result("B1", "a.txt\nb.txt", parent="A1"),
    assistant("m3", {"type": "text", "text": "SUB-REPORT: 2 files."}, parent="A1"),
    {"type": "system", "subtype": "task_notification", "tool_use_id": "A1",
     "status": "completed"},
    # An older CLI: the Task call's result arrives when the subagent is done.
    assistant("m4", tool_use("A2", "Task", {"description": "Old style task",
                                            "prompt": "Say hi."})),
    assistant("m5", {"type": "text", "text": "OLD-REPORT hi"}, parent="A2"),
    tool_result("A2", "OLD-REPORT hi"),
    assistant("m6", {"type": "text", "text": "PARENT-DONE"}),
    RESULT,
])
g.send("Count the files with a subagent.")
check(wait_idle(g), "the turn finished")
kids = {t["title"]: t for t in children(g, top)}
check(set(kids) == {"Count files", "Old style task"}, "two child threads under the parent")
check(not any(t["running"] or t["failed"] for t in kids.values()), "both done, none failed")
parent = text(g)
check("PARENT-DONE" in parent and "SUB-REPORT" not in parent,
      "the parent has its own messages only")
check(kids.get("Count files", {}).get("id", "?") in parent, "the Agent call names its thread")
check("Subagent thread: Count files" in blocks(g), "the parent links to the child")
g.select_thread(kids["Count files"]["id"])
child = text(g)
check("List the files" in child and "a.txt" in child and "SUB-REPORT" in child,
      "the child has the prompt, its tool call and its report")
check("Subagent thread (view only)" in blocks(g), "the child says it is view only")
try:
    g.send("hello")
    check(False, "sending to a child is refused")
except RuntimeError:
    check(True, "sending to a child is refused")
g.select_thread(kids["Old style task"]["id"])
check("OLD-REPORT" in text(g), "an older CLI's Task subagent is shown too")

print("-- cancelling")
slow = [
    INIT,
    assistant("m1", tool_use("A1", "Agent", {"description": "Slow task", "prompt": "Wait."})),
    tool_result("A1", "Async agent launched successfully.", background=True),
    assistant("m2", {"type": "text", "text": "working"}, parent="A1"),
    {"sleep": 60},
    RESULT,
]
for view in ("parent", "child"):
    scenario(slow)
    p = g.new_thread(A)["threadId"]
    g.send("Start a slow subagent.")
    check(wait_for(lambda: any(t["running"] for t in children(g, p))),
          f"[{view}] the child runs")
    c = children(g, p)[0]["id"]
    if view == "child":
        g.select_thread(c)
    g.cancel()
    check(wait_idle(g), f"[{view}] cancel stops the parent and the child")
    if view == "parent":
        check(threads(g)[c]["failed"], "[parent] the child is marked as failed")
        check("Cancelled." in blocks(g), "[parent] the parent says it was cancelled")
    g.select_thread(c)
    check("was stopped" in blocks(g), f"[{view}] the child says it was stopped")
    g.select_thread(p)
    check("was stopped" in blocks(g), f"[{view}] so does the parent's Agent call")

print("-- restart")
stop(pid)
pid, g = launch()
kids = {t["title"]: t for t in children(g, top)}
check(set(kids) == {"Count files", "Old style task"}, "child threads restored with their parent")
g.select_thread(top)
check("Subagent thread: Count files" in blocks(g), "the restored parent links to the child")

print("-- task tool (live, informational)")
models = [(0, "Kilo Free")]
if g.preferences().get("hasDeepseekKey"):
    models.append((2, "DeepSeek"))
else:
    print("skip  DeepSeek subagents (no API key)")
for row, name in models:
    g.set_model(row)
    p = g.new_thread(A)["threadId"]
    g.send("Call the task tool twice in one response, so both run in parallel: "
           "first with description 'Echo one' and prompt 'Run the bash command "
           "echo SUB-ONE and report its output.', then with description 'Echo two' "
           "and prompt 'Run the bash command echo SUB-TWO and report its output.' "
           "Then tell me what they reported.")
    info(wait_idle(g, 300), f"[{name}] the turn finished")
    kids = children(g, p)
    info(len(kids) == 2, f"[{name}] two child threads")
    tools = [m for m in g.conversation() if m["role"] == "tool" and m.get("name") == "task"]
    info([("SUB-ONE" in t["content"], "SUB-TWO" in t["content"]) for t in tools]
         == [(True, False), (False, True)], f"[{name}] reports in call order")
stop(pid)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
