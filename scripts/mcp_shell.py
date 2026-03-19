#!/usr/bin/env python3
"""
ZenCode MCP Client - Simple interactive client

Connects to ZenCode MCP server and allows sending commands.
"""

import socket
import json
import sys
import time

def send_command(sock, method, params=None, request_id=1, timeout=30):
    """Send a command to the MCP server and wait for response"""
    cmd = {
        "method": method,
        "id": request_id
    }
    if params:
        cmd["params"] = params
    
    cmd_str = json.dumps(cmd) + "\n"
    sock.send(cmd_str.encode())
    
    # Read response with timeout
    sock.settimeout(timeout)
    try:
        response = sock.recv(4096).decode()
        return json.loads(response)
    except socket.timeout:
        return {"error": "Timeout waiting for response"}
    except json.JSONDecodeError as e:
        return {"error": f"Invalid JSON response: {e}"}

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    
    print(f"Connecting to ZenCode MCP server at {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("✅ Connected!")
        
        # Quick status check
        print("\n📊 Getting status...")
        resp = send_command(sock, "getStatus", timeout=5)
        if "result" in resp:
            status = resp["result"]
            print(f"   Connected: {status.get('connected', False)}")
            print(f"   Status: {status.get('status', 'unknown')}")
            print(f"   Models: {status.get('modelCount', 0)}")
            print(f"   Chat messages: {status.get('chatMessageCount', 0)}")
        else:
            print(f"   Error: {resp.get('error', 'unknown')}")
        
        print("\n💡 Available commands:")
        print("   status, models, send <message>, response, history, ui, exit")
        print("\nEnter command:")
        
        while True:
            try:
                cmd_line = input("> ").strip()
                if not cmd_line:
                    continue
                
                parts = cmd_line.split(maxsplit=1)
                cmd = parts[0].lower()
                
                if cmd == "exit" or cmd == "quit":
                    break
                elif cmd == "status":
                    resp = send_command(sock, "getStatus")
                    print(json.dumps(resp, indent=2))
                elif cmd == "models":
                    resp = send_command(sock, "getModels")
                    print(json.dumps(resp, indent=2))
                elif cmd == "send":
                    if len(parts) < 2:
                        print("Usage: send <message>")
                        continue
                    message = parts[1]
                    resp = send_command(sock, "sendMessage", {"message": message})
                    print(json.dumps(resp, indent=2))
                    print("⏳ Waiting 5 seconds for response...")
                    time.sleep(5)
                    resp = send_command(sock, "getResponse")
                    print("Response:")
                    print(json.dumps(resp, indent=2))
                elif cmd == "response":
                    resp = send_command(sock, "getResponse")
                    print(json.dumps(resp, indent=2))
                elif cmd == "history":
                    resp = send_command(sock, "getChatHistory")
                    print(json.dumps(resp, indent=2))
                elif cmd == "ui":
                    resp = send_command(sock, "getUIState")
                    print(json.dumps(resp, indent=2))
                elif cmd == "connect":
                    resp = send_command(sock, "connect")
                    print(json.dumps(resp, indent=2))
                elif cmd == "disconnect":
                    resp = send_command(sock, "disconnect")
                    print(json.dumps(resp, indent=2))
                else:
                    print(f"Unknown command: {cmd}")
                    
            except EOFError:
                break
            except KeyboardInterrupt:
                break
        
        print("\n👋 Disconnecting...")
        sock.close()
        
    except ConnectionRefusedError:
        print(f"❌ Connection refused. Is ZenCode running with MCP server?")
        sys.exit(1)
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()
