#include "tools.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <array>
#include <wx/log.h>

namespace fcn {

using json = nlohmann::json;

std::vector<ToolDefinition> GetDefaultTools() {
  return {
    {
      "bash",
      "Execute a shell command and return its stdout and stderr. "
      "Use this for running commands like ls, pwd, cat, grep, find, git, etc.",
      R"json({
        "type": "object",
        "properties": {
          "command": {
            "type": "string",
            "description": "The shell command to execute"
          }
        },
        "required": ["command"]
      })json"
    },
    {
      "read_file",
      "Read the contents of a file at the given path.",
      R"json({
        "type": "object",
        "properties": {
          "path": {
            "type": "string",
            "description": "Absolute or relative path to the file"
          }
        },
        "required": ["path"]
      })json"
    },
    {
      "list_directory",
      "List the contents of a directory.",
      R"json({
        "type": "object",
        "properties": {
          "path": {
            "type": "string",
            "description": "Path to the directory (default: current directory)"
          }
        },
        "required": []
      })json"
    }
  };
}

static std::string RunCommand(const std::string& cmd, int maxBytes = 32768) {
  std::string fullCmd = cmd + " 2>&1";
  FILE* pipe = popen(fullCmd.c_str(), "r");
  if (!pipe) return "Error: failed to execute command";

  std::string result;
  std::array<char, 4096> buf;
  while (fgets(buf.data(), buf.size(), pipe)) {
    result += buf.data();
    if (static_cast<int>(result.size()) > maxBytes) {
      result += "\n... (output truncated at " + std::to_string(maxBytes) + " bytes)";
      break;
    }
  }
  int status = pclose(pipe);
  if (status != 0) {
    result += "\n[exit code: " + std::to_string(WEXITSTATUS(status)) + "]";
  }
  return result;
}

std::string ExecuteTool(const std::string& name, const std::string& argsJson) {
  json args = json::parse(argsJson, nullptr, false);
  if (args.is_discarded()) return "Error: invalid JSON arguments";

  wxLogMessage("ExecuteTool: %s(%s)", name.c_str(), argsJson.c_str());

  if (name == "bash") {
    std::string command = args.value("command", "");
    if (command.empty()) return "Error: no command provided";
    return RunCommand(command);
  }

  if (name == "read_file") {
    std::string path = args.value("path", "");
    if (path.empty()) return "Error: no path provided";
    return RunCommand("cat -- '" + path + "'");
  }

  if (name == "list_directory") {
    std::string path = args.value("path", ".");
    return RunCommand("ls -la -- '" + path + "'");
  }

  return "Error: unknown tool '" + name + "'";
}

} // namespace fcn
