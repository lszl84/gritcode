#!/usr/bin/env python3
"""
FastCode Native MCP Client

A simple Python client to connect to the FastCode Native MCP server and 
programmatically investigate issues.

Usage:
  python3 mcp_client.py <host> <port>
  
Example:
  python3 mcp_client.py localhost 8765
"""

import socket
import json
import sys
import time

def send_command(sock, method, params=None, request_id=1):
    """Send a command to the MCP server"""
    cmd = {
        "method": method,
        "id": request_id
    }
    if params:
        cmd["params"] = params
    
    cmd_str = json.dumps(cmd) + "\n"
    sock.send(cmd_str.encode())
    
    # Read response
    response = sock.recv(4096).decode()
    return json.loads(response)

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 mcp_client.py <host> <port>")
        print("Example: python3 mcp_client.py localhost 8765")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2])
    
    print(f"Connecting to FastCode Native MCP server at {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("Connected!")
        
        # Test 1: Get status
        print("\n1. Getting status...")
        resp = send_command(sock, "getStatus")
        print(f"Status: {json.dumps(resp, indent=2)}")
        
        # Test 2: Get models
        print("\n2. Getting available models...")
        resp = send_command(sock, "getModels")
        print(f"Models: {json.dumps(resp, indent=2)}")
        
        # Test 3: Send a test message
        print("\n3. Sending test message...")
        resp = send_command(sock, "sendMessage", {"message": "Hello from MCP client!"})
        print(f"Send result: {json.dumps(resp, indent=2)}")
        
        # Test 4: Wait and check for response
        print("\n4. Waiting for response...")
        time.sleep(5)
        resp = send_command(sock, "getResponse")
        print(f"Response: {json.dumps(resp, indent=2)}")
        
        # Test 5: Get chat history
        print("\n5. Getting chat history...")
        resp = send_command(sock, "getChatHistory")
        print(f"History: {json.dumps(resp, indent=2)}")
        
        # Test 6: Get full UI state
        print("\n6. Getting full UI state...")
        resp = send_command(sock, "getUIState")
        print(f"UI State: {json.dumps(resp, indent=2)}")
        
        print("\n✅ All tests completed!")
        
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        sock.close()

if __name__ == "__main__":
    main()
