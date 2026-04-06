#!/usr/bin/env python3
"""
FCN MCP Client — programmatic control of a running FastCode Native instance.

Usage as library:
    from mcp_client import FCN
    fcn = FCN()
    fcn.send("create a PHP project with unit tests")
    fcn.wait_idle(timeout=120)
    print(fcn.last_assistant())

Usage as CLI:
    python3 scripts/mcp_client.py status
    python3 scripts/mcp_client.py send "hello world"
    python3 scripts/mcp_client.py wait
    python3 scripts/mcp_client.py conversation
    python3 scripts/mcp_client.py last
"""

import socket
import json
import sys
import time


class FCN:
    def __init__(self, host="localhost", port=8765):
        self.host = host
        self.port = port
        self.sock = None
        self._id = 0

    def connect(self):
        """Connect to FCN MCP server. Tries ports 8765-8770."""
        for p in range(self.port, self.port + 6):
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(5)
                self.sock.connect((self.host, p))
                self.port = p
                return self
            except (ConnectionRefusedError, socket.timeout, OSError):
                self.sock.close()
                self.sock = None
        raise ConnectionError(f"Cannot connect to FCN on ports {self.port}-{self.port+5}")

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *args):
        self.close()

    def _call(self, method, params=None, timeout=10):
        if not self.sock:
            self.connect()
        self._id += 1
        req = {"method": method, "id": self._id}
        if params:
            req["params"] = params
        self.sock.settimeout(timeout)
        self.sock.send((json.dumps(req) + "\n").encode())
        # Read response line
        buf = b""
        while b"\n" not in buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("Connection closed")
            buf += chunk
        line = buf[:buf.index(b"\n")]
        resp = json.loads(line)
        if "error" in resp and resp["error"]:
            raise RuntimeError(f"MCP error: {resp['error']}")
        return resp.get("result")

    def status(self):
        return self._call("getStatus")

    def send(self, message):
        """Send a message to FCN (like typing and pressing Enter)."""
        return self._call("sendMessage", {"message": message})

    def conversation(self):
        """Get full conversation history."""
        return self._call("getConversation")

    def last_assistant(self):
        """Get the last assistant response text."""
        return self._call("getLastAssistant")

    def is_busy(self):
        """Check if a request is in progress."""
        s = self.status()
        return s.get("requestInProgress", False)

    def wait_idle(self, timeout=300, poll_interval=2):
        """Block until requestInProgress is False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.is_busy():
                return True
            time.sleep(poll_interval)
        raise TimeoutError(f"FCN still busy after {timeout}s")


def main():
    if len(sys.argv) < 2:
        print("Usage: mcp_client.py <command> [args]")
        print("Commands: status, send <msg>, wait, conversation, last")
        sys.exit(1)

    cmd = sys.argv[1]

    with FCN() as fcn:
        if cmd == "status":
            print(json.dumps(fcn.status(), indent=2))

        elif cmd == "send":
            if len(sys.argv) < 3:
                print("Usage: mcp_client.py send <message>")
                sys.exit(1)
            msg = " ".join(sys.argv[2:])
            fcn.send(msg)
            print(f"Sent: {msg}")

        elif cmd == "wait":
            timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 300
            print(f"Waiting up to {timeout}s for FCN to finish...")
            fcn.wait_idle(timeout=timeout)
            print("Done.")

        elif cmd == "conversation":
            conv = fcn.conversation()
            for m in conv:
                role = m.get("role", "?")
                content = m.get("content", "")
                if role == "tool":
                    print(f"[tool:{m.get('tool_call_id','')}] {content[:200]}...")
                elif role == "assistant" and m.get("tool_calls"):
                    tcs = m["tool_calls"]
                    names = ", ".join(tc.get("name", "?") for tc in tcs)
                    print(f"[assistant] {content[:100]}... -> tools: {names}")
                else:
                    print(f"[{role}] {content[:200]}")

        elif cmd == "last":
            resp = fcn.last_assistant()
            print(resp.get("text", "(no response)"))

        else:
            print(f"Unknown command: {cmd}")
            sys.exit(1)


if __name__ == "__main__":
    main()
