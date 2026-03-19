#!/usr/bin/env python3
"""
ZenCode MCP Inspector - Auto-detects port

Connects to a running ZenCode instance via MCP server.
Auto-detects port (tries 8765, 8766, 8767)
"""

import socket
import json
import sys
import time

class ZenCodeInspector:
    def __init__(self, host="localhost", ports=[8765, 8766, 8767]):
        self.host = host
        self.ports = ports
        self.port = None
        self.sock = None
        
    def connect(self):
        """Connect to ZenCode MCP server (auto-detect port)"""
        for port in self.ports:
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(3)
                self.sock.connect((self.host, port))
                self.port = port
                print(f"✅ Connected to ZenCode MCP on port {port}")
                return True
            except (ConnectionRefusedError, socket.timeout):
                self.sock.close()
                self.sock = None
                continue
        
        print(f"❌ Could not connect to ZenCode on any port: {self.ports}")
        print("   Is ZenCode running? Start it with: ./build/zencode")
        return False
        
    def disconnect(self):
        if self.sock:
            self.sock.close()
            self.sock = None
            print("👋 Disconnected")
    
    def send_cmd(self, method, params=None):
        cmd = {"method": method, "id": 1}
        if params:
            cmd["params"] = params
        
        try:
            self.sock.send((json.dumps(cmd) + "\n").encode())
            self.sock.settimeout(10)
            response = self.sock.recv(4096).decode()
            return json.loads(response)
        except socket.timeout:
            return {"success": False, "error": "Timeout"}
        except Exception as e:
            return {"success": False, "error": str(e)}

def main():
    inspector = ZenCodeInspector()
    
    if not inspector.connect():
        sys.exit(1)
    
    try:
        # Quick status check
        print("\n📊 Checking status...")
        resp = inspector.send_cmd("getStatus")
        if resp.get("success"):
            result = resp.get("result", {})
            print(f"   Connected to Zen: {result.get('connected')}")
            print(f"   Models: {result.get('modelCount')}")
            
            if result.get('connected'):
                print("\n✅ ZenCode is working! Connected to OpenCode Zen.")
                print(f"   Status: {result.get('status')}")
            else:
                print("\n⚠️  Not connected to Zen")
        else:
            print(f"   Error: {resp.get('error')}")
            
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        inspector.disconnect()

if __name__ == "__main__":
    main()
