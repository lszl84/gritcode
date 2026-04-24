// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Run grit as a stdio MCP server (JSON-RPC over stdin/stdout, newline-delimited)
// exposing just the memory_search tool. Intended to be spawned as a child
// process by an MCP-aware agent client (Claude CLI via --mcp-config, etc).
//
// Blocks on stdin; returns when the peer closes stdin or sends an unknown
// request that can't be handled. Exit code is the value to return from main().
int RunMcpStdioServer();
