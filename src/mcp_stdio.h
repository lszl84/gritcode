#pragma once

// Run gritcode as a stdio MCP server (JSON-RPC over stdin/stdout,
// newline-delimited) exposing grit_history_search and grit_history_fetch.
// Intended to be spawned as a child process by an MCP-aware agent client
// (Claude CLI via --mcp-config, etc).
//
// Blocks on stdin; returns when the peer closes stdin. Return value is
// the exit code to use from main().
int RunMcpStdioServer();
