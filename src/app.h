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
#include "glfw_window.h"
#include "scroll_view.h"
#include "widgets.h"
#include "curl_http.h"
#include "keychain.h"
#include "markdown_renderer.h"
#include "session.h"
#include "mcp_server.h"
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Thread-safe event queue for bg→main thread communication
class EventQueue {
public:
    void Push(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        q_.push(std::move(fn));
        glfwPostEmptyEvent();  // Wake up WaitEvents
    }
    void Drain() {
        std::lock_guard<std::mutex> lock(mu_);
        while (!q_.empty()) { q_.front()(); q_.pop(); }
    }
    bool Empty() {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.empty();
    }
private:
    std::mutex mu_;
    std::queue<std::function<void()>> q_;
};

class App {
public:
    bool Init(bool sessionChooser = false);
    void Run();

private:
    GlfwWindow window_;
    ScrollView scrollView_;
    GLRenderer renderer_;
    EventQueue events_;

    // Widgets
    Dropdown workspaceDropdown_;
    Dropdown providerDropdown_;
    Dropdown modelDropdown_;
    TextInput messageInput_;
    Button sendButton_;
    Button apiKeyButton_;
    TextInput apiKeyInput_;
    Button apiKeyAccept_;
    Button apiKeyCancel_;
    bool apiKeyEditing_ = false;
    TextInput* FocusedInput() {
        if (apiKeyInput_.focused) return &apiKeyInput_;
        if (messageInput_.focused) return &messageInput_;
        return nullptr;
    }
    void UnfocusAllInputs() {
        apiKeyInput_.focused = false;
        messageInput_.focused = false;
    }
    Label statusLabel_;
    Label versionLabel_;

    // MCP server
    MCPServer mcpServer_;
    void StartMCP();

    // Backend state
    net::CurlHttpClient httpClient_;
    SessionManager session_;
    std::string activeProvider_ = "zen";
    std::string activeModel_;
    // Set when a session is restored so OnModelsReceived can prefer the
    // restored model over the dropdown default. Cleared after first use.
    std::string restoredModelPref_;
    bool connected_ = false;
    bool requestInProgress_ = false;
    int toolRound_ = 0;

    // Claude ACP: child pid of running `claude --print` (so Escape can kill it),
    // and per-request generation so a stale finalizer from a cancelled request
    // doesn't clobber the UI state of a newer one.
    std::atomic<pid_t> claudePid_{-1};
    std::atomic<uint64_t> requestGen_{0};

    // Streaming state
    std::string responseBuffer_;
    bool receivingThinking_ = false;
    size_t responseStartBlock_ = 0;
    size_t lastMarkdownLen_ = 0;
    double lastMarkdownTime_ = 0;

    // Waiting indicator (plain dots, not a block)
    double requestStartTime_ = 0;
    bool waitingForResponse_ = false;  // True between send and first chunk
    float waitingDotAnim_ = 0;

    // Layout
    float barHeight_ = 40;
    float inputHeight_ = 50;
    void LayoutWidgets();
    void PaintBottomBar();

    // Actions
    void Connect(const std::string& apiKey = "");
    void SendMessage();
    void DoSendToProvider();
    void OnModelsReceived(std::vector<net::ModelInfo> models, int httpStatus);
    void OnWorkspaceChanged(int idx, const std::string& id);
    void OnProviderChanged(int idx, const std::string& id);
    void OnModelChanged(int idx, const std::string& id);
    void PopulateWorkspaceDropdown();
    void ShowApiKeyDialog();
    void AppendSystem(const std::string& text);

    // Tool execution
    void ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content);
    void RenderMarkdownToBlocks(bool isFinal = false);
    std::string BuildRequestJson();

    // Input handling
    void OnMouseDown(float x, float y, bool shift);
    void OnMouseUp(float x, float y);
    void OnMouseMove(float x, float y, bool leftDown);
    void OnScroll(float delta);
    void OnKey(int key, int mods, bool pressed);
    void OnChar(uint32_t codepoint);

    bool dirty_ = true;
    void MarkDirty() { dirty_ = true; }

    // Session chooser
    bool chooserMode_ = false;
    std::vector<SessionInfo> chooserSessions_;
    int chooserHovered_ = -1;
    float chooserScroll_ = 0;
    void PaintChooser();
    void ChooserSelect(int idx);
    void ChooserSelectPath(const std::string& path);
    void RestoreSessionToView();
    void StartConnect();
};
