#pragma once
#include "glfw_window.h"
#include "scroll_view.h"
#include "widgets.h"
#include "curl_http.h"
#include "keychain.h"
#include "markdown_renderer.h"
#include <string>
#include <vector>
#include <mutex>
#include <queue>
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

struct ChatMessage {
    std::string role;
    std::string content;
    std::vector<json> toolCalls;
    std::string toolCallId;
};

class App {
public:
    bool Init();
    void Run();

private:
    GlfwWindow window_;
    ScrollView scrollView_;
    GLRenderer renderer_;
    EventQueue events_;

    // Widgets
    Dropdown providerDropdown_;
    Dropdown modelDropdown_;
    TextInput messageInput_;
    Button sendButton_;
    Button apiKeyButton_;
    Label statusLabel_;

    // Backend state
    net::CurlHttpClient httpClient_;
    std::string activeProvider_ = "zen";  // "zen" or "claude"
    std::string activeModel_;
    std::vector<ChatMessage> history_;
    bool connected_ = false;
    bool requestInProgress_ = false;
    int toolRound_ = 0;

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
    void OnModelsReceived(std::vector<net::ModelInfo> models);
    void OnProviderChanged(int idx, const std::string& id);
    void OnModelChanged(int idx, const std::string& id);
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
};
