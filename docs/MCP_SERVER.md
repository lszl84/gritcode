# ZenCode MCP Server

ZenCode includes an MCP (Model Context Protocol) server that allows programmatic control and testing of the application without manual UI interaction.

## Overview

The MCP server is built into ZenCode and starts automatically when the app launches. It exposes APIs for:
- Sending chat messages programmatically
- Reading responses and chat history
- Monitoring connection state
- Managing models
- Waiting for async operations

## Starting the MCP Server

The MCP server starts automatically on port 8765 when ZenCode launches:

```bash
./build/zencode
```

Check the logs to confirm it's running:
```bash
tail -f /tmp/zencode_mcp.log | grep MCPServer
```

## MCP API Reference

### Connection Control

```cpp
// Connect to OpenCode Zen (anonymous by default)
mcp::MCPServer::Instance().Connect();

// Connect with API key
mcp::MCPServer::Instance().Connect("your-api-key");

// Disconnect
mcp::MCPServer::Instance().Disconnect();

// Check connection status
bool connected = mcp::MCPServer::Instance().IsConnected();
wxString status = mcp::MCPServer::Instance().GetConnectionStatus();
```

### Chat Operations

```cpp
// Send a message
mcp::MCPServer::Instance().SendChatMessage("Hello, AI!");

// Wait for response (blocking with timeout)
bool success = mcp::MCPServer::Instance().WaitForResponse(30000); // 30s timeout

// Get the last response
wxString response = mcp::MCPServer::Instance().GetLastResponse();

// Get full chat history
std::vector<wxString> history = mcp::MCPServer::Instance().GetChatHistory();

// Clear chat history
mcp::MCPServer::Instance().ClearChat();
```

### Model Operations

```cpp
// Get available models
std::vector<wxString> models = mcp::MCPServer::Instance().GetAvailableModels();

// Get currently selected model
wxString activeModel = mcp::MCPServer::Instance().GetActiveModel();

// Set active model
mcp::MCPServer::Instance().SetActiveModel("big-pickle");

// Get model count
int count = mcp::MCPServer::Instance().GetModelCount();
```

### UI State Queries

```cpp
// Check if send button would be enabled
bool canSend = mcp::MCPServer::Instance().IsSendButtonEnabled();
```

### Waiting for Conditions

```cpp
// Wait for connection (blocking)
bool connected = mcp::MCPServer::Instance().WaitForConnection(10000); // 10s timeout

// Wait for response (blocking)
bool gotResponse = mcp::MCPServer::Instance().WaitForResponse(30000); // 30s timeout
```

## Example: Automated Testing

```cpp
#include "mcp/mcp_server.h"

void TestChatFlow() {
  using namespace zencode::mcp;
  
  // 1. Connect
  MCPServer::Instance().Connect();
  if (!MCPServer::Instance().WaitForConnection(10000)) {
    std::cerr << "Failed to connect\n";
    return;
  }
  
  // 2. Wait for models to load
  // (This happens automatically in the background)
  
  // 3. Send a test message
  MCPServer::Instance().SendChatMessage("Test message");
  
  // 4. Wait for and verify response
  if (MCPServer::Instance().WaitForResponse(30000)) {
    wxString response = MCPServer::Instance().GetLastResponse();
    std::cout << "Got response: " << response.ToStdString() << "\n";
    
    // Verify response is not empty
    assert(!response.IsEmpty());
  } else {
    std::cerr << "Timeout waiting for response\n";
  }
  
  // 5. Check chat history
  auto history = MCPServer::Instance().GetChatHistory();
  assert(history.size() == 2); // User message + AI response
  
  std::cout << "Test passed!\n";
}
```

## Integration with Test Frameworks

The MCP server can be integrated with any C++ or Python test framework:

### Google Test Example

```cpp
TEST(ZenCodeMCP, CanSendAndReceiveMessage) {
  auto& mcp = zencode::mcp::MCPServer::Instance();
  
  mcp.Connect();
  ASSERT_TRUE(mcp.WaitForConnection(10000));
  
  mcp.SendChatMessage("Hello");
  ASSERT_TRUE(mcp.WaitForResponse(30000));
  
  EXPECT_FALSE(mcp.GetLastResponse().IsEmpty());
}
```

### Python Integration

You can create a Python wrapper that uses subprocess to control ZenCode:

```python
import subprocess
import time

def test_zencode():
    # Start ZenCode
    proc = subprocess.Popen(['./build/zencode'], 
                           stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    
    # Wait for MCP server to start
    time.sleep(3)
    
    # Check logs for connection
    # ... parse logs or use a proper MCP client protocol ...
    
    # Send test message via MCP (when protocol is implemented)
    
    proc.terminate()
```

## Architecture

The MCP server works by:

1. **Event Binding**: Binds to ZenClient events to track state changes
2. **State Tracking**: Maintains internal state of connection, models, and chat history
3. **Blocking Waits**: Uses wxYield() and polling for WaitFor* methods
4. **Thread Safety**: All operations are safe to call from the main thread

## Future Enhancements

Planned features for the MCP server:

- [ ] Socket-based protocol for external language bindings (Python, Node.js, etc.)
- [ ] JSON-RPC API for remote control
- [ ] Screenshot/capture API for visual testing
- [ ] UI element enumeration and manipulation
- [ ] Record/replay functionality for regression testing

## Debugging

Enable verbose MCP logging:

```bash
# The MCP server already logs to stderr
./build/zencode 2>&1 | grep MCPServer
```

Check current state:

```bash
# In another terminal, send USR1 signal to dump state
kill -USR1 $(pgrep zencode)
```
