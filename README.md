# ZenCode - OpenCode Zen Agent Harness

A fast, native, multi-platform agent harness built with wxWidgets and C++26.
Alternative to bloated, web-based TUI applications.

## Features

- ✅ **Anonymous OpenCode Zen access** - No API key needed for free models
- ✅ **Native GUI** - Built with wxWidgets 3.3, no web dependencies
- ✅ **MCP Server** - Built-in programmatic control API for testing
- ✅ **Free Models** - Big Pickle, MiMo, MiniMax, Nemotron, Trinity
- ✅ **Cross-platform** - Windows, macOS, Linux

## Building

```bash
# Configure with Ninja (recommended)
cmake -B build -G Ninja

# Build
cmake --build build

# Run
./build/zencode
```

## Requirements

- CMake 3.24+
- C++26 compatible compiler
- Ninja build system (optional but recommended)

## MCP Server (Programmatic Control)

ZenCode includes an MCP (Model Context Protocol) server for automated testing:

```cpp
#include "mcp/mcp_server.h"

// Send message programmatically
zencode::mcp::MCPServer::Instance().SendChatMessage("Hello!");

// Wait for response
if (zencode::mcp::MCPServer::Instance().WaitForResponse(30000)) {
    wxString response = zencode::mcp::MCPServer::Instance().GetLastResponse();
}
```

The MCP server starts automatically on port 8765. See [docs/MCP_SERVER.md](docs/MCP_SERVER.md) for full API documentation.

## Anonymous Access

Connect to OpenCode Zen without an API key to use free models:
- Big Pickle
- MiMo V2 Flash Free
- MiniMax M2.5 Free
- Nemotron 3 Super Free
- Trinity Large Preview Free

## License

GPL v3 - See LICENSE file
