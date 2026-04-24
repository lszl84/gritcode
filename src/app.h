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

#pragma once
#include "window.h"
#include "scroll_view.h"
#include "widgets.h"
#include "curl_http.h"
#include "keychain.h"
#include "markdown_renderer.h"
#include "session.h"
#ifdef GRIT_ENABLE_MCP
#include "mcp_server.h"
#endif
#include <string>
#include <vector>
#include <map>
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
        hasEvents_ = true;
    }
    void Drain() {
        std::lock_guard<std::mutex> lock(mu_);
        while (!q_.empty()) { q_.front()(); q_.pop(); }
        hasEvents_ = false;
    }
    bool Empty() {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.empty();
    }
    bool HasEvents() const {
        std::lock_guard<std::mutex> lock(mu_);
        return hasEvents_;
    }
private:
    mutable std::mutex mu_;
    std::queue<std::function<void()>> q_;
    bool hasEvents_ = false;
};

class App {
public:
    bool Init(bool sessionChooser = false);
    void Run();

private:
    AppWindow window_;
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
    // Window-level focus tracking. The compositor may briefly send a
    // keyboard leave+enter pair during xdg_toplevel_move (titlebar drag
    // grab) — we debounce so the message-box outline doesn't flash.
    bool windowFocused_ = true;
    uint64_t focusCbGen_ = 0;
    // One-shot: the click that raised the window from unfocused→focused also
    // fires OnMouseDown; if it lands in the scroll view we'd immediately
    // unfocus the message input the focus-gained handler just focused. This
    // flag lets that first click keep the message input focused. Cleared on
    // the next mousedown regardless of target.
    bool redirectNextClickToInput_ = false;

    // Where the current mouse drag originated — mousemove is routed to the
    // same widget regardless of keyboard focus, so selecting text in the
    // transcript works even while the message input holds focus.
    enum class DragTarget { None, ScrollView, MessageInput, ApiKeyInput };
    DragTarget dragTarget_ = DragTarget::None;
    Label statusLabel_;
    Label versionLabel_;

    // MCP server — dev/agent automation channel. Compiled out of Release
    // builds (see CMakeLists.txt) so production grit doesn't listen on any
    // port. Only present in Debug/RelWithDebInfo.
#ifdef GRIT_ENABLE_MCP
    MCPServer mcpServer_;
#endif
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

    // Compaction state.
    // historyCompactBaseCount_: number of messages left in history immediately
    //   after the last successful compaction. The preflight only attempts
    //   another compaction once history has grown beyond this by a hysteresis
    //   margin — without this guard we'd re-compact on every single request
    //   since history never shrinks between requests (it just grows by one
    //   assistant + tool pair per round).
    // compacting_: true while a summary LLM call is in flight. The main request
    //   will fire only after the summary returns (or fails and we fall back).
    int historyCompactBaseCount_ = 0;
    bool compacting_ = false;

    // models.dev registry — canonical source for the Zen and OpenCode Go
    // provider/model lists. Fetched once at startup (with a disk cache at
    // $XDG_CACHE_HOME/gritcode/models.json). Used to populate the
    // model dropdown and to filter out models whose wire protocol grit
    // doesn't speak (e.g. @ai-sdk/anthropic — /messages instead of
    // /chat/completions).
    nlohmann::json modelsRegistry_;
    bool registryLoaded_ = false;

    // Claude ACP: child pid of running `claude --print` (so Escape can kill it),
    // and per-request generation so a stale finalizer from a cancelled request
    // doesn't clobber the UI state of a newer one.
    std::atomic<pid_t> claudePid_{-1};
    std::atomic<uint64_t> requestGen_{0};
    std::atomic<int> retryCount_{0};

    // Streaming state
    std::string responseBuffer_;
    std::string reasoningBuffer_;  // Accumulated thinking/reasoning from models like Kimi K2.5
    bool receivingThinking_ = false;
    size_t responseStartBlock_ = 0;
    size_t lastMarkdownLen_ = 0;
    double lastMarkdownTime_ = 0;

    // Claude ACP tool-call streaming state. One THINKING block per tool round
    // (matches Zen path visually): each "Tool: <name>\n  <arg>: <val>\n..." as
    // tool_use blocks stream in, then "\n<name>:\n<output>\n" when tool_result
    // user messages arrive. acpToolBlockIdx_ == -1 means no open tool block.
    int acpToolBlockIdx_ = -1;
    std::map<int, std::string> acpToolInputBuf_;    // content-block index → partial JSON
    std::map<int, std::string> acpToolNames_;       // content-block index → tool name
    std::map<std::string, std::string> acpToolUseIdToName_;  // tool_use_id → name

    // Waiting indicator (plain dots, not a block)
    float waitingDotTimer_ = 0;
    int waitingDotFrame_ = -1;  // current animation frame (-1 = not showing)

    // Layout
    float barHeight_ = 40;
    float inputHeight_ = 50;
    float chromeTopPad_ = 4;  // Padding above the message row inside the bottom chrome
    void LayoutWidgets();
    void PaintBottomBar();

    // Actions
    void Connect(const std::string& apiKey = "");
    void SendMessage();
    void DoSendToProvider();
    // Compaction preflight (Zen/HTTP path only — Claude subprocess manages
    // its own context). If history is over budget, synthesizes a summary
    // via the model, replaces the head of history with an isSummary=true
    // marker message, then falls through to DoSendActualRequest(). If under
    // budget, calls DoSendActualRequest() directly.
    void MaybeCompactThenSend();
    void RunSummaryThenResend(int splitIdx);
    void ApplyCompaction(int splitIdx, bool success, const std::string& summary,
                         const std::string& error, int origHeadCount);
    void DoSendActualRequest();
    void OnModelsReceived(std::vector<net::ModelInfo> models, int httpStatus);
    void StartFetchRegistry();
    void OnRegistryReceived(nlohmann::json registry, int httpStatus);
    void PopulateModelsFromRegistry(const std::string& providerId);
    void PopulateClaudeModels();
    std::string GetRegistryCachePath();
    void OnWorkspaceChanged(int idx, const std::string& id);
    void OnProviderChanged(int idx, const std::string& id);
    void OnModelChanged(int idx, const std::string& id);
    void PopulateWorkspaceDropdown();
    void ShowApiKeyDialog();
    void AppendSystem(const std::string& text);
    void AppendSystemExpandable(const std::string& summary, const std::string& detail);

    // Tool execution
    void ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content);
    // Cancel the in-flight request, kill any running tool child, and fix up
    // history so the next turn's wire request stays well-formed. Shared by
    // the Escape key path and the MCP cancelRequest test hook.
    void CancelInFlight();
    void RenderMarkdownToBlocks(bool isFinal = false);
    std::string BuildRequestJson();
    std::string BuildAnthropicRequestJson();
    net::CurlHttpClient::Protocol ProtocolForActiveModel();

    // Model context limits from the models.dev registry
    struct ModelLimits {
        int contextWindow = 0;
        int maxOutput = 0;
    };
    ModelLimits GetModelLimits();

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
