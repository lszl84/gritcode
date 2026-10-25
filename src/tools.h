#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>

class MemoryDB;

// Returns the OpenAI-compatible "tools" array describing all tools the agent
// can call. Pass this as the "tools" field of the chat completions request.
// When includeGritHistory is false, grit_history_search/fetch are omitted so
// the model cannot see or call them. Top-level threads also get `task`, which
// the chat frame runs as a child thread (a subagent); subagents get no `task`
// and none of the tools that change files (see IsWriteTool).
nlohmann::json GetToolDefinitions(bool includeGritHistory, bool subagent = false);

// Tools that change files or project config. Subagents run concurrently with
// their parent and each other in one folder, so they are read-only: these
// tools are neither offered to them nor run for them.
bool IsWriteTool(const std::string& name);

// Cancellation handle threaded into the worker that runs a tool batch.
// The GUI flips `cancelled` from the GUI thread (Escape key); the worker
// observes it between tools and inside long-running ones (bash). `activePgid`
// holds the process-group id of an in-flight bash child so the GUI can
// SIGTERM the whole group without waiting for the worker to notice.
struct ToolCancelToken {
    std::atomic<bool> cancelled{false};
    std::atomic<int> activePgid{0};
};

// Dispatches a tool by name with parsed JSON arguments. Returns the textual
// result (already capped to ~30 KB) that should be sent back to the model as
// the content of a `role: tool` message. Always returns a non-empty string;
// errors are reported as "Error: ..." text rather than thrown.
//
// `token` may be null for callers that don't support cancellation; when set,
// long-running tools (bash) periodically check it and abort cleanly.
// `memory` backs the grit_history_search/fetch tools.
// Pass nullptr for `memory` if the index isn't open (those tools return a
// "Memory index unavailable" string in that case). When enableGritHistory is
// false, grit_history_search/fetch return a "disabled by the user" error.
// `currentCwd` is the calling thread's folder: relative paths resolve
// against it and bash runs there (the process cwd is never used); it is also
// the folder of grit_history_search's scope "folder". `threadId` (the
// calling thread, or empty) is left out of grit_history_search results.
std::string DispatchTool(const std::string& name,
                         const nlohmann::json& args,
                         ToolCancelToken* token,
                         MemoryDB* memory,
                         const std::string& currentCwd,
                         bool enableGritHistory,
                         const std::string& threadId = {});

// Run a shell command without the 30s timeout. Used by the Play button for
// long-running builds. Same cancellation semantics as ToolBash (SIGTERM on
// cancelled flag, SIGKILL after 500ms grace). Returns stdout+stderr.
std::string ToolBashDirect(const std::string& command,
                           ToolCancelToken* token = nullptr);
