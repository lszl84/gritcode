#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>

class MemoryDB;

// Returns the OpenAI-compatible "tools" array describing all tools the agent
// can call. Pass this as the "tools" field of the chat completions request.
nlohmann::json GetToolDefinitions();

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
// `memory` + `currentSessionId` back the grit_history_search/fetch tools.
// Pass nullptr for `memory` if the index isn't open (those tools return a
// "Memory index unavailable" string in that case).
std::string DispatchTool(const std::string& name,
                         const nlohmann::json& args,
                         ToolCancelToken* token = nullptr,
                         MemoryDB* memory = nullptr,
                         const std::string& currentSessionId = {});
