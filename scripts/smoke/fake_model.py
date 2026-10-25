#!/usr/bin/env python3
"""
A fake OpenAI-compatible model for the smoke tests: POST /chat/completions
answers with a scripted SSE stream, picked by the last message:

- a user message "Call the bash tool now with exactly this command: CMD"
  calls bash with CMD (the rest of that line);
- a user message "Call these tools now: [[name, {args}], ...]" calls those
  tools, all in one response;
- tool results get the text "Results: <result> | <result> ...";
- a project brief request (its system prompt starts "You write the project
  brief") gets "Fake brief. " followed by the material it was sent;
- anything else gets the text "ok".

Markers in the last user message, before any of those "Call ..." lines (so a
subagent's prompt inside a task call doesn't count for the parent), change the
replies: FAIL-500 answers that message with HTTP 500, SSE-CRLF ends lines with
CRLF and writes `data:` without a space, SSE-UNTERMINATED leaves out [DONE] and
the blank line after the last event.

Every reply waits `delay` seconds first, so turns overlap and the app's
1-second lastUsed timestamps differ between turns.

Usage: fake_model.py [delay]  (prints the URL; the smoke tests import serve())
"""

import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The first of these in a prompt counts (a task's prompt can hold another).
DIRECTIVE = re.compile("(Call the bash tool now with exactly this command|Call these tools now): (.*)")


def text_of(message):
    content = message.get("content") or ""
    if isinstance(content, list):  # [{"type": "text", "text": ...}, ...]
        return "".join(part.get("text", "") for part in content)
    return content


def reply_for(messages):
    """The delta of the reply: {"content": ...} or {"tool_calls": [...]}."""
    last = messages[-1]
    if text_of(messages[0]).startswith("You write the project brief"):
        return {"content": "Fake brief. " + text_of(last)}
    if last["role"] == "tool":
        results = []
        for m in reversed(messages):
            if m["role"] != "tool":
                break
            results.insert(0, text_of(m).strip())
        return {"content": "Results: " + " | ".join(results)}
    m = DIRECTIVE.search(text_of(last))
    if not m:
        return {"content": "ok"}
    calls = [["bash", {"command": m[2]}]] if "bash" in m[1] else json.loads(m[2])
    return {"tool_calls": [
        {"index": i, "id": f"call_{time.monotonic_ns()}_{i}", "type": "function",
         "function": {"name": name, "arguments": json.dumps(args)}}
        for i, (name, args) in enumerate(calls)]}


def sse(delta, crlf, unterminated):
    finish = "tool_calls" if "tool_calls" in delta else "stop"
    events = [
        {"choices": [{"index": 0, "delta": {"role": "assistant"}}]},
        # The last event carries everything, so dropping it loses the reply.
        {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]},
    ]
    payloads = [json.dumps(e) for e in events] + ([] if unterminated else ["[DONE]"])
    eol, field = ("\r\n", "data:") if crlf else ("\n", "data: ")
    body = (eol + eol).join(field + p for p in payloads)
    return body if unterminated else body + eol + eol


class Handler(BaseHTTPRequestHandler):
    delay = 1.1

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = request["messages"]
        user = next((text_of(m) for m in reversed(messages) if m["role"] == "user"), "")
        directive = DIRECTIVE.search(user)
        markers = user[:directive.start()] if directive else user
        time.sleep(self.delay)
        try:
            if "FAIL-500" in markers and messages[-1]["role"] == "user":
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"error": {"message": "fake model: scripted failure"}}')
                return
            body = sse(reply_for(messages), "SSE-CRLF" in markers,
                       "SSE-UNTERMINATED" in markers)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body.encode())
        except (BrokenPipeError, ConnectionResetError):
            pass  # the app cancelled the request

    def log_message(self, *args):
        pass


def serve(delay=1.1):
    """Starts the server on a free 127.0.0.1 port in a daemon thread; returns
    its /chat/completions URL."""
    Handler.delay = delay
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{server.server_port}/chat/completions"


if __name__ == "__main__":
    print(serve(float(sys.argv[1]) if len(sys.argv) > 1 else 1.1), flush=True)
    threading.Event().wait()
