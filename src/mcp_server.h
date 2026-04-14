// FastCode Native — GPU-rendered AI coding harness
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
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Callbacks the MCP server uses to interact with the App
struct MCPCallbacks {
    std::function<void(const std::string& msg)> sendMessage;
    std::function<json()> getStatus;
    std::function<json()> getConversation;     // Full conversation blocks text
    std::function<json()> getLastAssistant;    // Just the last assistant response
    std::function<void(const std::string& provider, const std::string& model)> setProvider;
    // Trigger Select-All and return the text the scroll view would place on
    // the clipboard. Used for end-to-end copy verification in tests.
    std::function<std::string()> selectAllText;
};

// Simple TCP-based JSON-RPC server for controlling FCN programmatically.
// Listens on port 8765 (or next available), accepts one client at a time.
class MCPServer {
public:
    MCPServer();
    ~MCPServer();

    void Start(MCPCallbacks callbacks);
    void Stop();
    int Port() const { return port_; }

private:
    void ServerLoop();
    void HandleClient(int clientFd);
    json HandleRequest(const json& request);

    MCPCallbacks cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int serverFd_ = -1;
    int port_ = 0;
    std::mutex cbMutex_;
};
