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

// Strip ANSI escape sequences (colors, cursor movement, etc.)
// so tool output is clean text in thinking blocks and model context.
static std::string StripAnsi(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
      // Skip CSI sequence: ESC [ ... <final byte>
      i += 2;
      while (i < s.size() && s[i] >= 0x20 && s[i] <= 0x3F) ++i;  // parameter bytes
      // skip final byte (0x40-0x7E)
      continue;
    }
    if (s[i] == '\033') {
      // Skip other ESC sequences (2-char)
      ++i;
      continue;
    }
    out += s[i];
  }
  return out;
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
  return StripAnsi(result);
}

// Expand leading ~ to $HOME so paths work outside of a login shell.
static std::string ExpandTilde(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const char* home = getenv("HOME");
  if (!home) return path;
  if (path.size() == 1) return home;            // just "~"
  if (path[1] == '/') return home + path.substr(1);  // "~/..."
  return path;  // "~user/..." — leave as-is
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
    std::string path = ExpandTilde(args.value("path", ""));
    if (path.empty()) return "Error: no path provided";
    return RunCommand("cat -- '" + path + "'");
  }

  if (name == "list_directory") {
    std::string path = ExpandTilde(args.value("path", "."));
    return RunCommand("ls -la -- '" + path + "'");
  }

  return "Error: unknown tool '" + name + "'";
}

} // namespace fcn
