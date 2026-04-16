// Gritcode — GPU-rendered AI coding harness
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

#include "mcp_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <poll.h>
#include <fcntl.h>

MCPServer::MCPServer() {}

MCPServer::~MCPServer() { Stop(); }

void MCPServer::Start(MCPCallbacks callbacks) {
    cb_ = std::move(callbacks);
    running_ = true;
    thread_ = std::thread(&MCPServer::ServerLoop, this);
}

void MCPServer::Stop() {
    running_ = false;
    if (serverFd_ >= 0) {
        shutdown(serverFd_, SHUT_RDWR);
        close(serverFd_);
        serverFd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void MCPServer::ServerLoop() {
    // SOCK_CLOEXEC so spawned tool subprocesses (wl-copy, etc.) don't inherit
    // the listen fd. Without this, backgrounded helpers daemonize holding the
    // port and block the next grit instance from binding.
#ifdef SOCK_CLOEXEC
    serverFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ >= 0) fcntl(serverFd_, F_SETFD, FD_CLOEXEC);
#endif
    if (serverFd_ < 0) { fprintf(stderr, "MCP: socket failed\n"); return; }

    int opt = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Try ports 8765-8770
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool bound = false;
    for (int p = 8765; p <= 8770; p++) {
        addr.sin_port = htons(p);
        if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            port_ = p;
            bound = true;
            break;
        }
    }
    if (!bound) { fprintf(stderr, "MCP: bind failed on ports 8765-8770\n"); return; }

    listen(serverFd_, 2);
    fprintf(stderr, "MCP server listening on port %d\n", port_);

    while (running_) {
        // Poll with timeout so we can check running_ flag
        struct pollfd pfd = {serverFd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        struct sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
#ifdef SOCK_CLOEXEC
        int clientFd = accept4(serverFd_, (struct sockaddr*)&clientAddr, &len, SOCK_CLOEXEC);
#else
        int clientFd = accept(serverFd_, (struct sockaddr*)&clientAddr, &len);
        if (clientFd >= 0) fcntl(clientFd, F_SETFD, FD_CLOEXEC);
#endif
        if (clientFd < 0) continue;

        HandleClient(clientFd);
        close(clientFd);
    }
}

void MCPServer::HandleClient(int clientFd) {
    std::string buffer;
    char chunk[4096];
    int idleCount = 0;

    while (running_) {
        struct pollfd pfd = {clientFd, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) break;
        if (ret == 0) {
            // Disconnect idle clients after 30s so other clients can connect
            if (++idleCount > 60) break;
            continue;
        }
        idleCount = 0;  // Reset on activity

        ssize_t n = recv(clientFd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buffer.append(chunk, n);

        // Process complete lines (newline-delimited JSON-RPC)
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            json response;
            try {
                auto request = json::parse(line);
                response = HandleRequest(request);
            } catch (const std::exception& e) {
                response = {{"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}}};
            }

            std::string responseStr = response.dump() + "\n";
            send(clientFd, responseStr.c_str(), responseStr.size(), MSG_NOSIGNAL);
        }
    }
}

json MCPServer::HandleRequest(const json& request) {
    std::string method = request.value("method", "");
    auto id = request.contains("id") ? request["id"] : json(nullptr);
    json params = request.value("params", json::object());

    json result;

    if (method == "getStatus") {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.getStatus) result = cb_.getStatus();
        else result = {{"error", "not ready"}};

    } else if (method == "sendMessage") {
        std::string msg = params.value("message", "");
        if (msg.empty()) {
            return {{"error", {{"code", -32602}, {"message", "missing 'message' param"}}}, {"id", id}};
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.sendMessage) {
            cb_.sendMessage(msg);
            result = {{"sent", true}};
        } else {
            result = {{"error", "not ready"}};
        }

    } else if (method == "getConversation") {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.getConversation) result = cb_.getConversation();
        else result = json::array();

    } else if (method == "getLastAssistant") {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.getLastAssistant) result = cb_.getLastAssistant();
        else result = {{"text", ""}};

    } else if (method == "selectAllText") {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.selectAllText) result = {{"text", cb_.selectAllText()}};
        else result = {{"text", ""}};

    } else if (method == "cancelRequest") {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.cancelRequest) { cb_.cancelRequest(); result = {{"cancelled", true}}; }
        else result = {{"error", "not ready"}};

    } else if (method == "setWorkspace") {
        std::string cwd = params.value("cwd", "");
        if (cwd.empty()) {
            return {{"error", {{"code", -32602}, {"message", "missing 'cwd' param"}}}, {"id", id}};
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.setWorkspace) {
            cb_.setWorkspace(cwd);
            result = {{"set", true}};
        } else {
            result = {{"error", "not ready"}};
        }

    } else if (method == "setProvider") {
        std::string provider = params.value("provider", "");
        std::string model = params.value("model", "");
        if (provider.empty()) {
            return {{"error", {{"code", -32602}, {"message", "missing 'provider' param"}}}, {"id", id}};
        }
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (cb_.setProvider) {
            cb_.setProvider(provider, model);
            result = {{"set", true}};
        } else {
            result = {{"error", "not ready"}};
        }

    } else {
        return {{"error", {{"code", -32601}, {"message", "unknown method: " + method}}}, {"id", id}};
    }

    return {{"result", result}, {"id", id}};
}
