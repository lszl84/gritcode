#!/usr/bin/env python3
"""
Smoke test for subagents: drives a hidden gritcode over the control API.

Usage:
    python3 scripts/smoke/subagents.py [--live] build/gritcode

Runs the app only through ~/.local/share/grit-dev/run-hidden.sh (invisible
Xvfb, temp HOME and data dir). Claude Code subagents are checked against a
fake `claude` that replays a few events in the shape the real CLI sends. The
`task` tool is checked on the free model (Kilo Free) pointed at a local fake
(fake_model.py). With --live it uses the real Kilo Free, which is flaky, so
those results are informational, plus DeepSeek when an API key is set.
"""

import json
import os
import random
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..")]
from fake_model import serve  # noqa: E402
from grit import Grit  # noqa: E402

LAUNCHER = os.path.expanduser("~/.local/share/grit-dev/run-hidden.sh")
STUBS = os.path.expanduser("~/.local/share/grit-dev/stubs")
LIVE = "--live" in sys.argv
ARGS = [a for a in sys.argv[1:] if a != "--live"]
BIN = os.path.abspath(ARGS[0] if ARGS else "build/gritcode")
MODEL_ENV = [] if LIVE else [f"GRIT_FREE_MODEL_URL={serve()}"]
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
    out = subprocess.check_output([LAUNCHER, BIN, f"GRIT_MCP_PORT={PORT}", path, *MODEL_ENV],
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


def info(cond, what):
    print(("ok    " if cond else "info  not: ") + what)


def threads(g):
    return {t["id"]: t for t in g.list_threads()["threads"]}


def children(g, parent):
    return [t for t in threads(g).values() if t["parent"] == parent]


def wait_idle(g, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if not any(t["running"] for t in threads(g).values()):
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


def tool_results(g, name):
    return [m["content"] for m in g.conversation() if m["role"] == "tool" and m.get("name") == name]


# ---- prompts the fake model (fake_model.py) acts on ----

def bash(cmd):
    return f"Call the bash tool now with exactly this command: {cmd}"


def call_tools(*calls):
    return "Call these tools now: " + json.dumps(calls)


def task(description, prompt):
    return ("task", {"description": description, "prompt": prompt})


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

if LIVE:
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
else:
    print("-- task tool")
    g.set_model(0)
    p = g.new_thread(A)["threadId"]
    g.send(call_tools(task("Echo one", bash("sleep 3; echo SUB-ONE")),
                      task("Echo two", bash("echo SUB-TWO"))))
    check(wait_for(lambda: sum(t["running"] for t in children(g, p)) == 2),
          "two task calls in one batch run at once")
    check(wait_idle(g), "the turn finished")
    check(sorted(t["title"] for t in children(g, p)) == ["Echo one", "Echo two"],
          "two child threads")
    reports = tool_results(g, "task")
    check([("SUB-ONE" in r, "SUB-TWO" in r) for r in reports] == [(True, False), (False, True)],
          "reports in call order, though the second finished first")
    last = g.conversation()[-1]
    check(last["role"] == "assistant" and "SUB-ONE" in last["content"],
          "the parent continues with the reports")

    p = g.new_thread(A)["threadId"]
    g.send(call_tools(task("Fails", "FAIL-500")))
    check(wait_idle(g), "[failing child] the turn finished")
    check([t["failed"] for t in children(g, p)] == [True], "[failing child] the child failed")
    reports = tool_results(g, "task")
    check(len(reports) == 1 and reports[0].startswith("Error: the subagent failed")
          and "HTTP 500" in reports[0], "[failing child] the parent gets an error result")

    p = g.new_thread(A)["threadId"]
    g.send(call_tools(task("Writes", call_tools(("write_file", {"path": "w.txt", "content": "x"})))))
    check(wait_idle(g), "[write_file] the turn finished")
    check(not os.path.exists(os.path.join(A, "w.txt")), "[write_file] the child wrote nothing")
    for t in children(g, p):
        g.select_thread(t["id"])
    refused = tool_results(g, "write_file")
    check(len(refused) == 1 and "read-only" in refused[0], "[write_file] the child's call is refused")

    p = g.new_thread(A)["threadId"]
    g.send(call_tools(task("Sleeps", bash("sleep 5; touch late.txt"))))
    check(wait_for(lambda: any(t["running"] for t in children(g, p))), "[cancel] the child runs")
    time.sleep(2)  # into the child's bash call
    g.cancel()
    check(wait_idle(g, 5), "[cancel] cancelling the parent stops the child")
    check([t["failed"] for t in children(g, p)] == [True], "[cancel] the child is marked as failed")
    time.sleep(5)
    check(not os.path.exists(os.path.join(A, "late.txt")), "[cancel] the child's bash was killed")
stop(pid)

print(f"{failures} failure(s); data in {DATA}" if failures else "all ok")
if not failures:
    shutil.rmtree(DATA, ignore_errors=True)
sys.exit(1 if failures else 0)
