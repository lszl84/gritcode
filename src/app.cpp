#include "app.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <array>
#include <xkbcommon/xkbcommon-keysyms.h>

// Tool execution (same as before)
static std::string RunCommand(const std::string& cmd) {
    std::string fullCmd = cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    if (!pipe) return "Error: failed to execute command";
    std::string result;
    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe)) {
        result += buf.data();
        if (result.size() > 32768) { result += "\n...(truncated)"; break; }
    }
    int status = pclose(pipe);
    if (status != 0) result += "\n[exit code: " + std::to_string(WEXITSTATUS(status)) + "]";
    return result;
}

static std::string ExpandTilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return home + path.substr(1);
    return path;
}

static std::string ExecuteTool(const std::string& name, const std::string& argsJson) {
    try {
        auto args = json::parse(argsJson);
        if (name == "bash") return RunCommand(args.value("command", ""));
        if (name == "read_file") return RunCommand("cat -- '" + ExpandTilde(args.value("path", "")) + "'");
        if (name == "list_directory") return RunCommand("ls -la -- '" + ExpandTilde(args.value("path", ".")) + "'");
    } catch (...) {}
    return "Error: unknown tool " + name;
}

static std::string StripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] >= 0x20 && s[i] <= 0x3F) ++i;
            continue;
        }
        if (s[i] == '\033') { ++i; continue; }
        out += s[i];
    }
    return out;
}

// Tool definitions JSON
static std::string ToolDefsJson() {
    json tools = json::array();
    tools.push_back({{"type","function"},{"function",{
        {"name","bash"},{"description","Execute a shell command"},
        {"parameters",{{"type","object"},{"properties",{{"command",{{"type","string"}}}}},{"required",json::array({"command"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","read_file"},{"description","Read file contents"},
        {"parameters",{{"type","object"},{"properties",{{"path",{{"type","string"}}}}},{"required",json::array({"path"})}}}
    }}});
    tools.push_back({{"type","function"},{"function",{
        {"name","list_directory"},{"description","List directory contents"},
        {"parameters",{{"type","object"},{"properties",{{"path",{{"type","string"}}}}},{"required",json::array()}}}
    }}});
    return tools.dump();
}

// ============================================================================
// Init
// ============================================================================

bool App::Init() {
    if (!window_.Init(1000, 750, "FastCode Native")) return false;

    if (!scrollView_.Init(window_.Width(), window_.Height() - (int)(barHeight_ + inputHeight_) * window_.Scale(),
                          window_.Scale()))
        return false;

    scrollView_.SetAutoScroll(true);
    scrollView_.SetClipboardFunc([&](const std::string& t) { window_.SetClipboard(t); });

    // Provider dropdown
    providerDropdown_.items = {{"zen", "OpenCode Zen"}, {"claude", "Claude (ACP)"}};
    providerDropdown_.selectedIndex = 0;
    providerDropdown_.onSelect = [&](int i, const std::string& id) { OnProviderChanged(i, id); };

    // Model dropdown (populated after connect)
    modelDropdown_.items = {};
    modelDropdown_.onSelect = [&](int i, const std::string& id) { OnModelChanged(i, id); };

    // Send button
    sendButton_.text = "Send";
    sendButton_.onClick = [&]() { SendMessage(); };

    // API Key button
    apiKeyButton_.text = "API Key...";
    apiKeyButton_.onClick = [&]() { ShowApiKeyDialog(); };

    // Message input
    messageInput_.placeholder = "Type a message...";
    messageInput_.onSubmit = [&](const std::string&) { SendMessage(); };

    statusLabel_.text = "Disconnected";

    // Window callbacks
    float currentScale = window_.Scale();
    window_.OnResize([&](int w, int h, float scale) {
        int viewH = h - (int)((barHeight_ + inputHeight_) * scale);
        if (scale != currentScale) {
            currentScale = scale;
            scrollView_.Init(w, viewH, scale);
        } else {
            scrollView_.OnResize(w, viewH);
        }
        LayoutWidgets();
        MarkDirty();
    });
    window_.OnMouseButton([&](float x, float y, bool pressed, bool shift) {
        if (pressed) OnMouseDown(x, y, shift);
        else OnMouseUp(x, y);
    });
    window_.OnMouseMove([&](float x, float y, bool left) { OnMouseMove(x, y, left); });
    window_.OnScrollEvent([&](float delta) { OnScroll(delta); });
    window_.OnKeyEvent([&](int key, int mods, bool pressed) {
        if (pressed) OnKey(key, mods, pressed);
    });

    LayoutWidgets();

    // Auto-connect
    std::string savedKey = keychain::LoadApiKey();
    if (!savedKey.empty()) {
        AppendSystem("Connecting with saved API key...");
        Connect(savedKey);
    } else {
        AppendSystem("Connecting anonymously...");
        Connect("");
    }

    return true;
}

// ============================================================================
// Layout
// ============================================================================

void App::LayoutWidgets() {
    float s = window_.Scale();
    float w = window_.Width() / s;
    float h = window_.Height() / s;

    float barY = h - barHeight_;
    float inputY = barY - inputHeight_;

    messageInput_.bounds = {8, inputY + 5, w - 80, inputHeight_ - 10};
    sendButton_.bounds = {w - 68, inputY + 5, 60, inputHeight_ - 10};

    float bx = 8;
    providerDropdown_.bounds = {bx, barY + 5, 150, barHeight_ - 10}; bx += 158;
    modelDropdown_.bounds = {bx, barY + 5, 160, barHeight_ - 10}; bx += 168;
    statusLabel_.bounds = {bx, barY + 5, 200, barHeight_ - 10}; bx += 208;

    apiKeyButton_.bounds = {w - 100, barY + 5, 90, barHeight_ - 10};
    apiKeyButton_.visible = (activeProvider_ == "zen");
}

// ============================================================================
// Paint
// ============================================================================

void App::PaintBottomBar() {
    float s = window_.Scale();
    float w = window_.Width() / s;
    float h = window_.Height() / s;
    auto& fm = scrollView_.Fonts();

    Color barBg{0.10f, 0.10f, 0.11f};
    Color inputAreaBg{0.13f, 0.13f, 0.14f};

    float barY = h - barHeight_;
    float inputY = barY - inputHeight_;

    renderer_.DrawRect(0, inputY * s, w * s, inputHeight_ * s, inputAreaBg);
    renderer_.DrawRect(0, barY * s, w * s, barHeight_ * s, barBg);

    // Scale transform: widgets use logical coords, renderer uses physical
    // For now, widgets draw at 1:1 since renderer works in physical pixels
    // and fonts are already scaled. Just offset coordinates.
    // TODO: proper scaling
    providerDropdown_.Paint(renderer_, fm);
    modelDropdown_.Paint(renderer_, fm);
    statusLabel_.Paint(renderer_, fm);
    apiKeyButton_.Paint(renderer_, fm);
    messageInput_.Paint(renderer_, fm, 0);  // TODO: pass real time for cursor blink
    sendButton_.Paint(renderer_, fm);
}

// ============================================================================
// Connection & Chat
// ============================================================================

void App::Connect(const std::string& apiKey) {
    httpClient_.SetBaseUrl("https://opencode.ai/zen/v1");
    httpClient_.SetApiKey(apiKey);
    connected_ = true;
    statusLabel_.text = apiKey.empty() ? "Zen (Anonymous)" : "Zen (API Key)";
    MarkDirty();

    httpClient_.FetchModels([this](std::vector<net::ModelInfo> models) {
        events_.Push([this, models]() { OnModelsReceived(models); });
    });
}

void App::OnModelsReceived(std::vector<net::ModelInfo> models) {
    modelDropdown_.items.clear();
    int defaultIdx = 0;

    for (size_t i = 0; i < models.size(); i++) {
        bool isFree = models[i].allowAnonymous ||
                      models[i].id.find("free") != std::string::npos ||
                      models[i].id.find("big-pickle") != std::string::npos;
        if (httpClient_.IsAnonymous() && !isFree) continue;
        modelDropdown_.items.push_back({models[i].id, models[i].name});
        if (models[i].id == "kimi-k2.5") defaultIdx = modelDropdown_.items.size() - 1;
    }

    if (modelDropdown_.items.empty()) {
        modelDropdown_.items = {
            {"big-pickle", "Big Pickle"},
            {"kimi-k2.5", "Kimi K2.5"},
        };
    }

    modelDropdown_.selectedIndex = defaultIdx;
    activeModel_ = modelDropdown_.SelectedId();
    AppendSystem("Models loaded. Active: " + activeModel_);
    MarkDirty();
}

void App::OnProviderChanged(int, const std::string& id) {
    activeProvider_ = id;
    apiKeyButton_.visible = (id == "zen");
    AppendSystem("Switched to " + id);
    LayoutWidgets();

    if (id == "zen") {
        std::string key = keychain::LoadApiKey();
        Connect(key);
    } else {
        statusLabel_.text = "Claude (ACP)";
        connected_ = true;
        modelDropdown_.items = {
            {"claude-opus-4-6", "Claude Opus 4.6"},
            {"claude-sonnet-4-6", "Claude Sonnet 4.6"},
            {"claude-haiku-4-5", "Claude Haiku 4.5"},
        };
        modelDropdown_.selectedIndex = 1;
        activeModel_ = "claude-sonnet-4-6";
    }
    MarkDirty();
}

void App::OnModelChanged(int, const std::string& id) {
    activeModel_ = id;
    MarkDirty();
}

void App::ShowApiKeyDialog() {
    // For now just toggle — full dialog needs text input popup
    // TODO: modal dialog
    AppendSystem("Use the message input to type your API key, then click API Key button again to save.");
}

void App::AppendSystem(const std::string& text) {
    scrollView_.AppendStream(BlockType::THINKING, text);
    MarkDirty();
}

// ============================================================================
// Sending messages
// ============================================================================

std::string App::BuildRequestJson() {
    json j;
    j["model"] = activeModel_;
    j["stream"] = true;

    json msgs = json::array();
    for (auto& m : history_) {
        json msg;
        msg["role"] = m.role;
        if (m.role == "tool") {
            msg["tool_call_id"] = m.toolCallId;
            msg["content"] = m.content;
        } else if (m.role == "assistant" && !m.toolCalls.empty()) {
            msg["content"] = m.content.empty() ? json(nullptr) : json(m.content);
            json tcs = json::array();
            for (auto& tc : m.toolCalls) {
                tcs.push_back({{"id", tc["id"]}, {"type", "function"},
                               {"function", {{"name", tc["name"]}, {"arguments", tc["arguments"]}}}});
            }
            msg["tool_calls"] = tcs;
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(msg);
    }
    j["messages"] = msgs;

    // Tools
    j["tools"] = json::parse(ToolDefsJson());
    j["tool_choice"] = "auto";

    return j.dump();
}

void App::SendMessage() {
    std::string msg = messageInput_.text;
    if (msg.empty() || !connected_ || requestInProgress_) return;

    // Show in UI
    scrollView_.AppendStream(BlockType::USER_PROMPT, msg);
    messageInput_.Clear();

    history_.push_back({"user", msg, {}, {}});
    requestInProgress_ = true;
    sendButton_.enabled = false;
    toolRound_ = 0;
    MarkDirty();

    DoSendToProvider();
}

void App::DoSendToProvider() {
    if (activeProvider_ == "claude") {
        // Claude ACP: spawn claude binary
        // Build prompt with history
        std::string prompt;
        if (history_.size() > 1) {
            prompt = "<conversation_history>\n";
            for (size_t i = 0; i < history_.size() - 1; i++) {
                prompt += "<" + history_[i].role + ">\n" + history_[i].content + "\n</" + history_[i].role + ">\n";
            }
            prompt += "</conversation_history>\n\n";
        }
        if (!history_.empty() && history_.back().role == "user")
            prompt += history_.back().content;

        std::string cmd = "claude --print --verbose --output-format stream-json --dangerously-skip-permissions --model " + activeModel_;

        std::thread([this, cmd, prompt]() {
            FILE* pipe = popen(cmd.c_str(), "w");
            // TODO: proper ACP streaming. For now, use simple print mode
            // This is a placeholder — real ACP needs process management
            if (pipe) {
                fwrite(prompt.c_str(), 1, prompt.size(), pipe);
                pclose(pipe);
            }
            events_.Push([this]() {
                requestInProgress_ = false;
                sendButton_.enabled = true;
                MarkDirty();
            });
        }).detach();
        return;
    }

    // Zen provider: HTTP streaming
    std::string requestJson = BuildRequestJson();

    httpClient_.SendStreaming(requestJson,
        // onChunk (bg thread)
        [this](const std::string& chunk, bool isThinking) {
            events_.Push([this, chunk, isThinking]() {
                if (isThinking) {
                    scrollView_.AppendStream(BlockType::THINKING, chunk);
                } else {
                    // Accumulate markdown for later render
                    // For simplicity, stream as plain text for now
                    scrollView_.ContinueStream(chunk);
                }
                MarkDirty();
            });
        },
        // onComplete (bg thread)
        [this](bool ok, const std::string& content, const std::string& error,
               const std::vector<json>& toolCalls, int, int) {
            events_.Push([this, ok, content, error, toolCalls]() {
                if (!ok) {
                    if (!history_.empty() && history_.back().role == "user")
                        history_.pop_back();
                    AppendSystem("Error: " + error);
                    requestInProgress_ = false;
                    sendButton_.enabled = true;
                    MarkDirty();
                    return;
                }

                if (!toolCalls.empty() && toolRound_ < 10) {
                    ExecuteToolCalls(toolCalls, content);
                    return;
                }

                // Final response
                if (!content.empty())
                    history_.push_back({"assistant", content, {}, {}});
                requestInProgress_ = false;
                sendButton_.enabled = true;
                scrollView_.StopAllAnimations();
                MarkDirty();
            });
        }
    );
}

void App::ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content) {
    toolRound_++;

    // Add assistant message with tool calls
    history_.push_back({"assistant", content, toolCalls, {}});

    // Show tool info in thinking block
    std::string info;
    for (auto& tc : toolCalls) {
        info += "Tool: " + tc.value("name", "") + "\n";
        try {
            auto args = json::parse(tc.value("arguments", "{}"));
            for (auto& [k, v] : args.items()) info += "  " + k + ": " + v.dump() + "\n";
        } catch (...) {}
    }
    scrollView_.AppendStream(BlockType::THINKING, info);
    scrollView_.StartThinking(scrollView_.BlockCount() - 1);
    MarkDirty();

    // Execute in background
    auto tcCopy = toolCalls;
    std::thread([this, tcCopy]() {
        struct Result { std::string id, output; };
        std::vector<Result> results;
        for (auto& tc : tcCopy) {
            results.push_back({
                tc.value("id", ""),
                ExecuteTool(tc.value("name", ""), tc.value("arguments", "{}"))
            });
        }

        events_.Push([this, results, tcCopy]() {
            // Show results
            std::string resultText;
            for (size_t i = 0; i < results.size(); i++) {
                resultText += "\n" + tcCopy[i].value("name", "") + ":\n" + StripAnsi(results[i].output) + "\n";
            }
            scrollView_.ContinueStream(resultText);

            // Add to history
            for (size_t i = 0; i < results.size(); i++) {
                history_.push_back({"tool", results[i].output, {}, results[i].id});
            }

            scrollView_.StopThinking(scrollView_.BlockCount() - 1);
            DoSendToProvider();  // Continue the loop
            MarkDirty();
        });
    }).detach();
}

// ============================================================================
// Input handling
// ============================================================================

void App::OnMouseDown(float x, float y, bool shift) {
    float s = window_.Scale();
    float lx = x / s, ly = y / s;

    // Check dropdowns first (they have popups)
    if (providerDropdown_.OnMouseDown(lx, ly)) { MarkDirty(); return; }
    if (modelDropdown_.OnMouseDown(lx, ly)) { MarkDirty(); return; }

    // Close any open dropdowns
    providerDropdown_.Close();
    modelDropdown_.Close();

    if (sendButton_.OnMouseDown(lx, ly)) { MarkDirty(); return; }
    if (apiKeyButton_.OnMouseDown(lx, ly)) { MarkDirty(); return; }
    if (messageInput_.OnMouseDown(lx, ly)) { MarkDirty(); return; }

    // Scroll view gets physical coords
    float viewH = window_.Height() - (barHeight_ + inputHeight_) * s;
    if (y < viewH) {
        scrollView_.OnMouseDown(x, y, shift);
        messageInput_.focused = false;
        MarkDirty();
    }
}

void App::OnMouseUp(float x, float y) {
    float s = window_.Scale();
    float lx = x / s, ly = y / s;

    sendButton_.OnMouseUp(lx, ly);
    apiKeyButton_.OnMouseUp(lx, ly);
    scrollView_.OnMouseUp(x, y);
    MarkDirty();
}

void App::OnMouseMove(float x, float y, bool leftDown) {
    float s = window_.Scale();
    float lx = x / s, ly = y / s;

    sendButton_.OnMouseMove(lx, ly);
    apiKeyButton_.OnMouseMove(lx, ly);
    providerDropdown_.OnMouseMove(lx, ly);
    modelDropdown_.OnMouseMove(lx, ly);

    float viewH = window_.Height() - (barHeight_ + inputHeight_) * s;
    if (y < viewH) {
        scrollView_.OnMouseMove(x, y, leftDown);
    }
    MarkDirty();
}

void App::OnScroll(float delta) {
    scrollView_.OnScroll(delta);
    MarkDirty();
}

void App::OnKey(int key, int mods, bool pressed) {
    if (!pressed) return;

    // Escape cancels request
    if (key == XKB_KEY_Escape && requestInProgress_) {
        httpClient_.Abort();
        requestInProgress_ = false;
        sendButton_.enabled = true;
        if (!history_.empty() && history_.back().role == "user") {
            // Keep message but mark as cancelled
            history_.push_back({"assistant", "[cancelled]", {}, {}});
        }
        scrollView_.StopAllAnimations();
        AppendSystem("Cancelled");
        MarkDirty();
        return;
    }

    // Forward to text input if focused
    if (messageInput_.focused) {
        messageInput_.OnKey(key, mods);
        MarkDirty();
        return;
    }

    // Scroll view keyboard
    scrollView_.OnKey(key, mods);
    MarkDirty();
}

void App::OnChar(uint32_t codepoint) {
    if (messageInput_.focused) {
        messageInput_.OnChar(codepoint);
        MarkDirty();
    }
}

// ============================================================================
// Main loop
// ============================================================================

void App::Run() {
    double lastTime = GetMonotonicTime();

    while (!window_.ShouldClose()) {
        // Process events
        if (dirty_ || !events_.Empty()) {
            window_.PollEvents();
        } else {
            window_.WaitEvents();
        }

        // Drain bg thread events
        events_.Drain();

        double now = GetMonotonicTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        // Update
        messageInput_.Update(dt);
        scrollView_.Update(dt);

        if (!dirty_ && !scrollView_.NeedsRedraw()) continue;
        dirty_ = false;
        scrollView_.ClearDirty();

        // Render
        uint32_t* pixels = window_.BeginFrame();
        if (!pixels) continue;

        renderer_.BeginFrame(pixels, window_.Width(), window_.Height(), scrollView_.Fonts());

        // Background
        Color bg{0.12f, 0.12f, 0.13f};
        renderer_.DrawRect(0, 0, window_.Width(), window_.Height(), bg);

        // Scroll view (top portion)
        scrollView_.Paint(renderer_);

        // Bottom bar + input
        PaintBottomBar();

        window_.EndFrame();
    }
}
