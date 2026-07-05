#pragma once
#include <nlohmann/json.hpp>
#include <string>

// Per-project "run" configuration — one command per project directory.
// Stored as a JSON file next to the memory database:
//   $XDG_DATA_HOME/gritcode/run_configs.json
//
// Keyed by absolute project path. Each entry holds the shell command the
// model discovered to build/run the project, plus metadata.
class RunConfigStore {
public:
    struct Config {
        std::string command;
        std::string discoveredBy;  // "model" or "user"
        std::string lastUsed;      // ISO-8601 timestamp
    };

    // Load from disk. Returns an empty map if the file doesn't exist.
    static std::map<std::string, Config> Load();

    // Save to disk.
    static void Save(const std::map<std::string, Config>& map);

    // Get the config for a project dir. Returns nullopt if none.
    static std::optional<Config> Get(const std::string& cwd);

    // Set (or replace) the config for a project dir. Auto-saves.
    static void Set(const std::string& cwd, const std::string& command,
                    const std::string& discoveredBy = "model");

    // Remove the config for a project dir. Auto-saves.
    static void Forget(const std::string& cwd);

    static std::string StoragePath();
};
