#pragma once
#include "glfw_window.h"
#include "scroll_view.h"
#include "gl_renderer.h"
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

class EventQueue {
public:
    void Push(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        q_.push(std::move(fn));
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

    // Backend state
    net::CurlHttpClient httpClient_;
    std::string activeProvider_ = "zen";
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

    // ImGui state
    char msgBuf_[4096] = {};
    int providerIdx_ = 0;
    int modelIdx_ = 0;
    std::vector<net::ModelInfo> models_;
    std::string statusText_ = "Disconnected";

    // ImGui UI
    void DrawImGui();

    // Actions
    void Connect(const std::string& apiKey = "");
    void SendMessage();
    void DoSendToProvider();
    void OnModelsReceived(std::vector<net::ModelInfo> models);
    void AppendSystem(const std::string& text);

    // Tool execution
    void ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content);
    void RenderMarkdownToBlocks(bool isFinal = false);
    std::string BuildRequestJson();

    bool dirty_ = true;
    void MarkDirty() { dirty_ = true; }
};
