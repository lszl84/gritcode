#!/usr/bin/env python3
"""
wx_gritcode MCP client — programmatic control of a running app.

Usage:
    python3 scripts/grit.py status
    python3 scripts/grit.py send "test message"
    python3 scripts/grit.py wait                # block until streaming==false
    python3 scripts/grit.py last                # last assistant text
    python3 scripts/grit.py conversation        # full history JSON
    python3 scripts/grit.py cancel              # cancel in-flight request
"""

import json
import socket
import sys
import time


class Grit:
    def __init__(self, host="127.0.0.1", base_port=8765):
        self.host = host
        self.base_port = base_port
        self._id = 0

    def _call(self, method, params=None, timeout=10):
        for p in range(self.base_port, self.base_port + 6):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(timeout)
                s.connect((self.host, p))
                break
            except (ConnectionRefusedError, OSError):
                s.close()
        else:
            raise ConnectionError(f"no MCP server on {self.host}:{self.base_port}-{self.base_port+5}")

        self._id += 1
        req = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            req["params"] = params
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            buf += chunk
        s.close()
        resp = json.loads(buf[: buf.index(b"\n")])
        if "error" in resp and resp["error"]:
            raise RuntimeError(f"MCP error: {resp['error']}")
        return resp.get("result")

    def status(self):
        return self._call("getStatus")

    def send(self, msg):
        r = self._call("sendMessage", {"message": msg})
        if isinstance(r, dict) and not r.get("sent"):
            raise RuntimeError(f"sendMessage rejected: {r.get('reason', 'unknown')}")
        return r

    def conversation(self):
        return self._call("getConversation")

    def last_assistant(self):
        return self._call("getLastAssistant")

    def cancel(self):
        return self._call("cancelRequest")

    def blocks(self):
        return self._call("getBlocks")

    def toggle(self, index):
        return self._call("toggleTool", {"index": index})

    def list_sessions(self):
        return self._call("listSessions")

    def switch_session(self, cwd):
        return self._call("switchSession", {"cwd": cwd})

    def new_session(self):
        return self._call("newSession")

    def set_model(self, idx):
        return self._call("setModel", {"index": idx})

    def preferences(self):
        return self._call("getPreferences")

    def wait_idle(self, timeout=300, poll=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.status().get("streaming"):
                return True
            time.sleep(poll)
        raise TimeoutError(f"still streaming after {timeout}s")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    g = Grit()
    if cmd == "status":
        print(json.dumps(g.status(), indent=2))
    elif cmd == "send":
        msg = " ".join(sys.argv[2:])
        if not msg:
            print("usage: send <text>")
            sys.exit(1)
        g.send(msg)
        print("sent")
    elif cmd == "wait":
        timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 300
        g.wait_idle(timeout=timeout)
        print("idle")
    elif cmd == "last":
        print(g.last_assistant().get("text", ""))
    elif cmd == "conversation":
        print(json.dumps(g.conversation(), indent=2))
    elif cmd == "cancel":
        print(json.dumps(g.cancel()))
    elif cmd == "blocks":
        print(json.dumps(g.blocks(), indent=2))
    elif cmd == "toggle":
        idx = int(sys.argv[2])
        print(json.dumps(g.toggle(idx)))
    elif cmd == "sessions":
        print(json.dumps(g.list_sessions(), indent=2))
    elif cmd == "switch":
        if len(sys.argv) < 3:
            print("usage: switch <cwd>")
            sys.exit(1)
        print(json.dumps(g.switch_session(sys.argv[2]), indent=2))
    elif cmd == "new":
        print(json.dumps(g.new_session(), indent=2))
    elif cmd == "model":
        if len(sys.argv) < 3:
            print("usage: model <index>  (0=OpenCode Free, 1=DeepSeek Flash, 2=DeepSeek Pro)")
            sys.exit(1)
        print(json.dumps(g.set_model(int(sys.argv[2])), indent=2))
    elif cmd == "prefs":
        print(json.dumps(g.preferences(), indent=2))
    else:
        print(f"unknown command: {cmd}")
        sys.exit(1)


if __name__ == "__main__":
    main()
