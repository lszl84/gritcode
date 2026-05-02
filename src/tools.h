#pragma once
#include <nlohmann/json.hpp>
#include <string>

// Returns the OpenAI-compatible "tools" array describing all tools the agent
// can call. Pass this as the "tools" field of the chat completions request.
nlohmann::json GetToolDefinitions();

// Dispatches a tool by name with parsed JSON arguments. Returns the textual
// result (already capped to ~30 KB) that should be sent back to the model as
// the content of a `role: tool` message. Always returns a non-empty string;
// errors are reported as "Error: ..." text rather than thrown.
std::string DispatchTool(const std::string& name, const nlohmann::json& args);
