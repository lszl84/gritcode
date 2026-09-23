#pragma once

#include <string>

// Headless agent mode: run a single user prompt through DeepSeek Flash
// (DEEPSEEK_API_KEY env var), with the normal tool set enabled but the grit
// history tools (grit_history_search/fetch) disabled. No GUI, no MCP server.
//
// Prints the final assistant message to stdout; tool activity goes to stderr.
// Returns the process exit code (0 = success).
int RunHeadless(const std::string& prompt);
