#include "mcp_server.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using nlohmann::json;

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
#ifdef SOCK_CLOEXEC
    serverFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ >= 0) fcntl(serverFd_, F_SETFD, FD_CLOEXEC);
#endif
    if (serverFd_ < 0) {
        fprintf(stderr, "MCP: socket() failed\n");
        return;
    }

    int opt = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool bound = false;
    for (int p = 8765; p <= 8770; ++p) {
        addr.sin_port = htons(p);
        if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            port_ = p;
            bound = true;
            break;
        }
    }
    if (!bound) {
        fprintf(stderr, "MCP: bind failed on ports 8765-8770\n");
        return;
    }

    listen(serverFd_, 2);
    fprintf(stderr, "MCP server listening on 127.0.0.1:%d\n", port_);

    while (running_) {
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
    int idleTicks = 0;

    while (running_) {
        struct pollfd pfd = {clientFd, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) break;
        if (ret == 0) {
            // Drop idle clients after 30s so others can connect.
            if (++idleTicks > 60) break;
            continue;
        }
        idleTicks = 0;

        ssize_t n = recv(clientFd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buffer.append(chunk, n);

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
                response = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {{"code", -32700},
                               {"message", std::string("Parse error: ") + e.what()}}},
                };
            }

            std::string out = response.dump(
                -1, ' ', false, json::error_handler_t::replace) + "\n";
            send(clientFd, out.c_str(), out.size(), MSG_NOSIGNAL);
        }
    }
}

json MCPServer::HandleRequest(const json& request) {
    std::string method = request.value("method", "");
    json id = request.contains("id") ? request["id"] : json(nullptr);
    json params = request.value("params", json::object());

    auto okResult = [&](json r) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(r)}};
    };
    auto err = [&](int code, const std::string& msg) {
        return json{{"jsonrpc", "2.0"}, {"id", id},
                    {"error", {{"code", code}, {"message", msg}}}};
    };

    std::lock_guard<std::mutex> lock(cbMutex_);

    if (method == "getStatus") {
        if (!cb_.getStatus) return err(-32603, "not ready");
        return okResult(cb_.getStatus());
    }
    if (method == "getConversation") {
        if (!cb_.getConversation) return err(-32603, "not ready");
        return okResult(cb_.getConversation());
    }
    if (method == "getLastAssistant") {
        if (!cb_.getLastAssistant) return err(-32603, "not ready");
        return okResult(cb_.getLastAssistant());
    }
    if (method == "sendMessage") {
        std::string msg = params.value("message", "");
        if (msg.empty()) return err(-32602, "missing 'message' param");
        if (!cb_.sendMessage) return err(-32603, "not ready");
        return okResult(cb_.sendMessage(msg));
    }
    if (method == "cancelRequest") {
        if (!cb_.cancelRequest) return err(-32603, "not ready");
        cb_.cancelRequest();
        return okResult({{"cancelled", true}});
    }
    if (method == "getBlocks") {
        if (!cb_.getBlocks) return err(-32603, "not ready");
        return okResult(cb_.getBlocks());
    }
    if (method == "toggleTool") {
        if (!cb_.toggleTool) return err(-32603, "not ready");
        if (!params.contains("index") || !params["index"].is_number_integer())
            return err(-32602, "missing/invalid 'index' param");
        cb_.toggleTool(params["index"].get<int>());
        return okResult({{"toggled", true}});
    }
    if (method == "listSessions") {
        if (!cb_.listSessions) return err(-32603, "not ready");
        return okResult(cb_.listSessions());
    }
    if (method == "switchSession") {
        if (!cb_.switchSession) return err(-32603, "not ready");
        std::string cwd = params.value("cwd", "");
        if (cwd.empty()) return err(-32602, "missing 'cwd' param");
        return okResult(cb_.switchSession(cwd));
    }
    if (method == "newSession") {
        if (!cb_.newSession) return err(-32603, "not ready");
        return okResult(cb_.newSession());
    }
    if (method == "setModel") {
        if (!cb_.setModel) return err(-32603, "not ready");
        if (!params.contains("index") || !params["index"].is_number_integer())
            return err(-32602, "missing/invalid 'index' param");
        return okResult(cb_.setModel(params["index"].get<int>()));
    }
    if (method == "getPreferences") {
        if (!cb_.getPreferences) return err(-32603, "not ready");
        return okResult(cb_.getPreferences());
    }
    return err(-32601, "unknown method: " + method);
}
