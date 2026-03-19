#!/usr/bin/env python3
"""Simple MCP test - just connects and sends one command"""

import socket
import json

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5)

try:
    print("Connecting...")
    sock.connect(("localhost", 8765))
    print("Connected!")
    
    # Send a simple command
    cmd = '{"method":"getStatus","id":1}\n'
    print(f"Sending: {cmd.strip()}")
    sock.send(cmd.encode())
    
    # Receive response
    print("Waiting for response...")
    data = sock.recv(4096)
    print(f"Received: {data.decode()}")
    
    if data:
        resp = json.loads(data.decode())
        print(f"Parsed: {json.dumps(resp, indent=2)}")
    
except Exception as e:
    print(f"Error: {e}")
    import traceback
    traceback.print_exc()
finally:
    sock.close()
    print("Disconnected")
