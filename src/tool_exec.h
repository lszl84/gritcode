// Gritcode — tool execution extracted from app.cpp so the GTK4 frontend
// can reuse it. The GLFW frontend still keeps its own file-scoped copies.
#pragma once
#include <string>
#include <atomic>
#include <sys/types.h>

namespace toolexec {

// Process-group id of the currently-running bash tool, or 0. The cancel
// path (Escape / MCP cancelRequest) can read this and send SIGKILL to
// the whole group to unblock the read loop fast.
extern std::atomic<pid_t> g_toolPgid;

// Run a tool by OpenAI-function-call name with a JSON-encoded arguments
// string. Returns the textual result (stdout+stderr for bash, file body
// for read_file, status string for write/edit/etc.). Blocks on the
// calling thread — never call this from the UI thread.
std::string ExecuteTool(const std::string& name, const std::string& argsJson);

// OpenAI-format tool definitions as a JSON array string. Paste into the
// request body's "tools" field. The Anthropic adapter can transform
// these in place since the schema overlaps.
std::string ToolDefsJson();

// Kill the running bash tool group (if any). Idempotent. Called from
// the cancel path.
void KillRunningTool();

// Strip ANSI CSI / ESC codes. Tool output passes through this before
// being shown in the transcript.
std::string StripAnsi(const std::string& s);

} // namespace toolexec
