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
    scrollView_.SetClipboardFunc([&](const std::string& t) {
        // Use wl-copy for cross-app clipboard on Wayland
        FILE* p = popen("wl-copy 2>/dev/null", "w");
        if (p) { fwrite(t.c_str(), 1, t.size(), p); pclose(p); }
    });

    if (!renderer_.Init()) {
        fprintf(stderr, "GL renderer init failed\n");
        return false;
    }

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
    messageInput_.onPaste = [&]() -> std::string {
        // GLFW can't read cross-app clipboard on Wayland, use wl-paste
        FILE* p = popen("wl-paste --no-newline 2>/dev/null", "r");
        if (!p) return "";
        std::string result;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) result += buf;
        pclose(p);
        return result;
    };

    statusLabel_.text = "Disconnected";
    versionLabel_.text = "v" FCN_VERSION;
    versionLabel_.color = {0.35f, 0.35f, 0.37f};

    // Window callbacks
    float currentScale = window_.Scale();
    window_.OnResize([&, currentScale](int w, int h, float scale) mutable {
        int viewH = h - (int)((barHeight_ + inputHeight_) * scale);
        if (viewH < 1) viewH = 1;
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
    window_.OnCharEvent([&](uint32_t cp) {
        OnChar(cp);
    });

    LayoutWidgets();

    AppendSystem("Starting...");
    window_.Show();

    // Load API key and connect in background
    std::thread([this]() {
        std::string savedKey = keychain::LoadApiKey();
        events_.Push([this, savedKey]() {
            if (!savedKey.empty()) {
                AppendSystem("Connecting with saved API key...");
                Connect(savedKey);
            } else {
                AppendSystem("Connecting anonymously...");
                Connect("");
            }
        });
    }).detach();

    return true;
}

// ============================================================================
// Layout
// ============================================================================

void App::LayoutWidgets() {
    // All coordinates in physical pixels (matching WaylandWindow and ScrollView)
    float w = window_.Width();
    float h = window_.Height();
    float s = window_.Scale();
    float bar = barHeight_ * s;
    float inp = inputHeight_ * s;

    float barY = h - bar;
    float inputY = barY - inp;

    messageInput_.bounds = {8, inputY + 5, w - 100, inp - 10};
    sendButton_.bounds = {w - 88, inputY + 5, 80, inp - 10};

    float bx = 8;
    providerDropdown_.bounds = {bx, barY + 5, 170 * s, bar - 10}; bx += 178 * s;
    modelDropdown_.bounds = {bx, barY + 5, 180 * s, bar - 10}; bx += 188 * s;
    statusLabel_.bounds = {bx, barY + 5, 220 * s, bar - 10}; bx += 228 * s;

    apiKeyButton_.bounds = {w - 110 * s, barY + 5, 100 * s, bar - 10};
    apiKeyButton_.visible = (activeProvider_ == "zen");
    versionLabel_.bounds = {w - 110 * s - 80 * s, barY + 5, 70 * s, bar - 10};
}

// ============================================================================
// Paint
// ============================================================================

void App::PaintBottomBar() {
    float s = window_.Scale();
    float w = window_.Width();
    float h = window_.Height();
    auto& fm = scrollView_.Fonts();

    Color barBg{0.10f, 0.10f, 0.11f};
    Color inputAreaBg{0.13f, 0.13f, 0.14f};

    float bar = barHeight_ * s;
    float inp = inputHeight_ * s;
    float barY = h - bar;
    float inputY = barY - inp;

    renderer_.DrawRect(0, inputY, w, inp, inputAreaBg);
    renderer_.DrawRect(0, barY, w, bar, barBg);

    // Scale transform: widgets use logical coords, renderer uses physical
    // For now, widgets draw at 1:1 since renderer works in physical pixels
    // and fonts are already scaled. Just offset coordinates.
    // TODO: proper scaling
    providerDropdown_.Paint(renderer_, fm);
    modelDropdown_.Paint(renderer_, fm);
    statusLabel_.Paint(renderer_, fm);
    versionLabel_.Paint(renderer_, fm);
    apiKeyButton_.Paint(renderer_, fm);
    messageInput_.Paint(renderer_, fm, 0);
    sendButton_.Paint(renderer_, fm);

    // Dropdown popups last (z-order: on top of everything)
    providerDropdown_.PaintPopup(renderer_, fm);
    modelDropdown_.PaintPopup(renderer_, fm);
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
    requestStartTime_ = GetMonotonicTime();
    waitingForResponse_ = true;
    waitingDotAnim_ = 0;
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

    responseBuffer_.clear();
    receivingThinking_ = false;
    lastMarkdownLen_ = 0;
    lastMarkdownTime_ = GetMonotonicTime();

    httpClient_.SendStreaming(requestJson,
        // onChunk (bg thread)
        [this](const std::string& chunk, bool isThinking) {
            events_.Push([this, chunk, isThinking]() {
                // First chunk received — stop waiting dots
                waitingForResponse_ = false;

                if (isThinking) {
                    if (!receivingThinking_) {
                        receivingThinking_ = true;
                        scrollView_.AppendStream(BlockType::THINKING, chunk);
                        scrollView_.StartThinking(scrollView_.BlockCount() - 1);
                    } else {
                        scrollView_.ContinueStream(chunk);
                    }
                } else {
                    // Content — finalize thinking if transitioning
                    if (receivingThinking_) {
                        receivingThinking_ = false;
                        scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                        responseStartBlock_ = scrollView_.BlockCount();
                        responseBuffer_.clear();
                        lastMarkdownLen_ = 0;
                    }

                    // Set start block on first content chunk
                    if (responseBuffer_.empty()) {
                        responseStartBlock_ = scrollView_.BlockCount();
                    }

                    responseBuffer_ += chunk;

                    // Progressive markdown: re-render every 500ms if enough new text
                    double now = GetMonotonicTime();
                    if (now - lastMarkdownTime_ > 0.5 &&
                        responseBuffer_.size() > lastMarkdownLen_ + 20) {
                        RenderMarkdownToBlocks();
                        lastMarkdownTime_ = now;
                        lastMarkdownLen_ = responseBuffer_.size();
                    }
                }
                MarkDirty();
            });
        },
        // onComplete (bg thread)
        [this](bool ok, const std::string& content, const std::string& error,
               const std::vector<json>& toolCalls, int, int) {
            events_.Push([this, ok, content, error, toolCalls]() {
                waitingForResponse_ = false;

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

                // Finalize thinking
                if (receivingThinking_) {
                    receivingThinking_ = false;
                    scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                }

                // Final markdown render (complete buffer, no truncation)
                if (!responseBuffer_.empty()) {
                    RenderMarkdownToBlocks(true);
                    responseBuffer_.clear();
                }

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

void App::RenderMarkdownToBlocks(bool isFinal) {
    if (responseBuffer_.empty()) return;

    // During streaming: only render up to the last paragraph boundary
    // to avoid cutting off incomplete markdown constructs (headings, fences)
    std::string toRender = responseBuffer_;
    if (!isFinal) {
        size_t lastBoundary = toRender.rfind("\n\n");
        if (lastBoundary == std::string::npos || lastBoundary < 10) {
            // Not enough complete paragraphs yet — skip this render
            return;
        }
        toRender = toRender.substr(0, lastBoundary);
    }

    // Remove old content blocks from responseStartBlock_ onwards
    scrollView_.RemoveBlocksFrom(responseStartBlock_);

    // Render markdown
    MarkdownRenderer mdRenderer(14);
    auto mdBlocks = mdRenderer.Render(toRender, true);

    scrollView_.BeginBatch();
    scrollView_.AddBlocks(std::move(mdBlocks));
    scrollView_.EndBatch();
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
    // All coords are physical pixels from WaylandWindow

    // Check dropdowns first (they have popups)
    if (providerDropdown_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (modelDropdown_.OnMouseDown(x, y)) { MarkDirty(); return; }

    // Close any open dropdowns
    providerDropdown_.Close();
    modelDropdown_.Close();

    if (sendButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (apiKeyButton_.OnMouseDown(x, y)) { MarkDirty(); return; }
    if (messageInput_.OnMouseDown(x, y, scrollView_.Fonts())) {
        scrollView_.ClearSelection();
        MarkDirty();
        return;
    }

    // Scroll view
    float s = window_.Scale();
    float viewH = window_.Height() - (barHeight_ + inputHeight_) * s;
    if (y < viewH) {
        scrollView_.OnMouseDown(x, y, shift);
        messageInput_.focused = false;
        messageInput_.selStart = messageInput_.selEnd = 0;  // Clear text input selection
        MarkDirty();
    }
}

void App::OnMouseUp(float x, float y) {
    sendButton_.OnMouseUp(x, y);
    apiKeyButton_.OnMouseUp(x, y);
    scrollView_.OnMouseUp(x, y);
    MarkDirty();
}

void App::OnMouseMove(float x, float y, bool leftDown) {
    // Mouse drag in text input — skip all other hover updates
    if (leftDown && messageInput_.focused) {
        messageInput_.OnMouseDrag(x, y, scrollView_.Fonts());
        MarkDirty();
        return;
    }

    sendButton_.OnMouseMove(x, y);
    apiKeyButton_.OnMouseMove(x, y);
    providerDropdown_.OnMouseMove(x, y);
    modelDropdown_.OnMouseMove(x, y);

    float s = window_.Scale();
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

    // Ctrl+C: copy from whichever has a selection
    if ((mods & Mod::Ctrl) && key == Key::C) {
        if (messageInput_.focused && messageInput_.selStart != messageInput_.selEnd) {
            std::string sel = messageInput_.GetSelectedText();
            if (!sel.empty()) {
                FILE* p = popen("wl-copy 2>/dev/null", "w");
                if (p) { fwrite(sel.c_str(), 1, sel.size(), p); pclose(p); }
            }
        } else {
            scrollView_.OnKey(key, mods);
        }
        MarkDirty();
        return;
    }

    // Ctrl+A: select all in scroll view (unless text input focused)
    if ((mods & Mod::Ctrl) && key == Key::A && !messageInput_.focused) {
        scrollView_.OnKey(key, mods);
        MarkDirty();
        return;
    }

    // Forward to text input if focused
    if (messageInput_.focused) {
        messageInput_.OnKey(key, mods, scrollView_.Fonts());
        MarkDirty();
        return;
    }

    // Scroll view keyboard
    scrollView_.OnKey(key, mods);
    MarkDirty();
}

void App::OnChar(uint32_t codepoint) {
    if (messageInput_.focused) {
        messageInput_.OnChar(codepoint, scrollView_.Fonts());
        MarkDirty();
    }
}

// ============================================================================
// Main loop
// ============================================================================

void App::Run() {
    double lastTime = GetMonotonicTime();

    while (!window_.ShouldClose()) {
        // Block until something happens (poll during waiting animation)
        bool showingDots = waitingForResponse_ && (GetMonotonicTime() - requestStartTime_ > 1.5);
        if (!dirty_ && events_.Empty() && !scrollView_.NeedsRedraw() && !showingDots) {
            window_.WaitEvents();
        } else {
            window_.PollEvents();
        }

        // Drain bg thread events
        events_.Drain();

        double now = GetMonotonicTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        messageInput_.Update(dt, scrollView_.Fonts());
        scrollView_.Update(dt);

        // Animate waiting dots
        if (waitingForResponse_) {
            waitingDotAnim_ += dt;
            double elapsed = now - requestStartTime_;
            if (elapsed > 1.5) MarkDirty();  // Keep redrawing for animation
        }

        if (!dirty_ && !scrollView_.NeedsRedraw()) continue;
        dirty_ = false;
        scrollView_.ClearDirty();

        // Drain again in case events arrived during update
        events_.Drain();

        // Render
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        renderer_.BeginFrame(window_.Width(), window_.Height(), scrollView_.Fonts());

        // Scroll view (top portion)
        scrollView_.Paint(renderer_);

        // Waiting dots (plain, below last block, after 3s)
        if (waitingForResponse_ && (now - requestStartTime_ > 1.5)) {
            auto& fm = scrollView_.Fonts();
            float dotR = fm.LineHeight(FontStyle::Regular) * 0.15f;
            float spacing = dotR * 3;
            float baseX = 20;
            float baseY = scrollView_.ContentBottom() + fm.LineHeight(FontStyle::Regular);
            int frame = (int)(waitingDotAnim_ * 4) % 4;  // 0-3 animation frame
            for (int d = 0; d < 3; d++) {
                float alpha = (d == (frame % 3)) ? 1.0f : 0.3f;
                Color c{0.5f, 0.5f, 0.5f, alpha};
                float cx = baseX + d * spacing;
                renderer_.DrawRect(cx - dotR, baseY - dotR, dotR * 2, dotR * 2, c);
            }
        }

        // Bottom bar + input
        PaintBottomBar();

        renderer_.EndFrame();
        window_.SwapBuffers();

        // Throttle to ~60fps since vsync is off (Wayland frame callback issue)
        struct timespec sleepTime = {0, 8000000};  // 8ms (~120fps headroom)
        nanosleep(&sleepTime, nullptr);

        if (!events_.Empty()) dirty_ = true;

        // Perf tracking
        clock_gettime(CLOCK_MONOTONIC, &t1);
        static int frameCount = 0;
        static long totalUs = 0;
        static double perfStart = now;
        long frameUs = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        totalUs += frameUs;
        frameCount++;
        if (now - perfStart > 2.0 && frameCount > 0) {
            FILE* pf = fopen("/tmp/fcn-native-perf.log", "a");
            if (pf) {
                fprintf(pf, "PERF: %d frames in %.1fs (%.0f/s) | avg %.0fus (%.2fms)\n",
                        frameCount, now - perfStart,
                        frameCount / (now - perfStart),
                        (double)totalUs / frameCount,
                        (double)totalUs / frameCount / 1000.0);
                fclose(pf);
            }
            frameCount = 0; totalUs = 0; perfStart = now;
        }
    }
}
