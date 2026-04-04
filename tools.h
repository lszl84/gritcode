#pragma once

#include "chat_provider.h"
#include <string>
#include <vector>

namespace fcn {

// Returns the default set of read-only tools available to models.
std::vector<ToolDefinition> GetDefaultTools();

// Execute a tool by name with JSON arguments. Returns the output string.
std::string ExecuteTool(const std::string& name, const std::string& argsJson);

// Strip ANSI escape sequences for display in thinking blocks.
std::string StripAnsi(const std::string& s);

} // namespace fcn
