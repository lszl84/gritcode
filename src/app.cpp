#include "app.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <array>

// Tool execution (unchanged)
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

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Customize style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.WindowBorderSize = 0;
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 4);

    ImGui_ImplGlfw_InitForOpenGL(window_.Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Init scroll view (upper portion)
    int barH = 90;  // ImGui bottom bar height in pixels
    if (!scrollView_.Init(window_.Width(), window_.Height() - barH, window_.Scale()))
        return false;

    scrollView_.SetAutoScroll(true);
    scrollView_.SetClipboardFunc([&](const std::string& t) { window_.SetClipboard(t); });

    if (!renderer_.Init()) return false;

    // Window callbacks — ImGui handles input via its own GLFW callbacks
    // We only need to forward scroll view events for the chat area
    float currentScale = window_.Scale();
    window_.OnResize([&, currentScale](int w, int h, float scale) mutable {
        int barH = 90;
        int viewH = h - barH;
        if (viewH < 1) viewH = 1;
        if (scale != currentScale) {
            currentScale = scale;
            scrollView_.Init(w, viewH, scale);
        } else {
            scrollView_.OnResize(w, viewH);
        }
        MarkDirty();
    });

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
// ImGui UI
// ============================================================================

void App::DrawImGui() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x;
    float h = io.DisplaySize.y;
    float barH = 90;

    // Bottom panel
    ImGui::SetNextWindowPos(ImVec2(0, h - barH));
    ImGui::SetNextWindowSize(ImVec2(w, barH));
    ImGui::Begin("##bottombar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse);

    // Message input
    float sendW = 70;
    ImGui::PushItemWidth(w - sendW - 24);
    bool submitted = ImGui::InputText("##msg", msgBuf_, sizeof(msgBuf_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    bool canSend = !requestInProgress_ && connected_ && msgBuf_[0] != '\0';
    if (!canSend) ImGui::BeginDisabled();
    if (ImGui::Button("Send", ImVec2(sendW, 0)) || (submitted && canSend)) {
        SendMessage();
    }
    if (!canSend) ImGui::EndDisabled();

    // Provider selector
    const char* providers[] = {"OpenCode Zen", "Claude (ACP)"};
    if (ImGui::Combo("##provider", &providerIdx_, providers, 2)) {
        activeProvider_ = (providerIdx_ == 0) ? "zen" : "claude";
        if (providerIdx_ == 0) {
            Connect(keychain::LoadApiKey());
        } else {
            statusText_ = "Claude (ACP)";
            connected_ = true;
            models_.clear();
            models_.push_back({"claude-opus-4-6", "Claude Opus 4.6"});
            models_.push_back({"claude-sonnet-4-6", "Claude Sonnet 4.6"});
            models_.push_back({"claude-haiku-4-5", "Claude Haiku 4.5"});
            modelIdx_ = 1;
            activeModel_ = "claude-sonnet-4-6";
        }
    }

    ImGui::SameLine();

    // Model selector
    if (!models_.empty()) {
        // Build combo items
        std::string preview = (modelIdx_ >= 0 && modelIdx_ < (int)models_.size())
            ? models_[modelIdx_].name : "Select...";
        if (ImGui::BeginCombo("##model", preview.c_str())) {
            for (int i = 0; i < (int)models_.size(); i++) {
                bool selected = (i == modelIdx_);
                if (ImGui::Selectable(models_[i].name.c_str(), selected)) {
                    modelIdx_ = i;
                    activeModel_ = models_[i].id;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusText_.c_str());

    // API Key button (only for Zen)
    if (providerIdx_ == 0) {
        ImGui::SameLine(w - 100);
        if (ImGui::Button("API Key...")) {
            AppendSystem("API key management not yet implemented in this build.");
        }
    }

    ImGui::End();

    // Forward mouse events to scroll view when not over ImGui
    if (!io.WantCaptureMouse) {
        float mx = io.MousePos.x * window_.Scale();
        float my = io.MousePos.y * window_.Scale();

        if (ImGui::IsMouseClicked(0)) {
            scrollView_.OnMouseDown(mx, my, io.KeyShift);
        }
        if (ImGui::IsMouseReleased(0)) {
            scrollView_.OnMouseUp(mx, my);
        }
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
            scrollView_.OnMouseMove(mx, my, ImGui::IsMouseDown(0));
        }
        if (io.MouseWheel != 0) {
            scrollView_.OnScroll(io.MouseWheel);
        }
    }

    // Keyboard to scroll view when ImGui doesn't want it
    if (!io.WantCaptureKeyboard) {
        // Escape cancels request
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && requestInProgress_) {
            httpClient_.Abort();
            requestInProgress_ = false;
            if (!history_.empty() && history_.back().role == "user")
                history_.push_back({"assistant", "[cancelled]", {}, {}});
            scrollView_.StopAllAnimations();
            AppendSystem("Cancelled");
        }
        // Ctrl+A / Ctrl+C
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
            scrollView_.OnKey(0x61, 4);  // 'a' + ctrl
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            scrollView_.OnKey(0x63, 4);  // 'c' + ctrl
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ============================================================================
// Connection & Chat (same as glfw branch)
// ============================================================================

void App::Connect(const std::string& apiKey) {
    httpClient_.SetBaseUrl("https://opencode.ai/zen/v1");
    httpClient_.SetApiKey(apiKey);
    connected_ = true;
    statusText_ = apiKey.empty() ? "Zen (Anonymous)" : "Zen (API Key)";

    httpClient_.FetchModels([this](std::vector<net::ModelInfo> m) {
        events_.Push([this, m]() { OnModelsReceived(m); });
    });
}

void App::OnModelsReceived(std::vector<net::ModelInfo> fetchedModels) {
    models_.clear();
    modelIdx_ = 0;

    for (auto& m : fetchedModels) {
        bool isFree = m.allowAnonymous || m.id.find("free") != std::string::npos
                      || m.id.find("big-pickle") != std::string::npos;
        if (httpClient_.IsAnonymous() && !isFree) continue;
        models_.push_back(m);
        if (m.id == "kimi-k2.5") modelIdx_ = models_.size() - 1;
    }

    if (models_.empty()) {
        models_.push_back({"big-pickle", "Big Pickle"});
        models_.push_back({"kimi-k2.5", "Kimi K2.5"});
    }

    activeModel_ = models_[modelIdx_].id;
    AppendSystem("Models loaded. Active: " + activeModel_);
}

void App::AppendSystem(const std::string& text) {
    scrollView_.AppendStream(BlockType::THINKING, text);
    MarkDirty();
}

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
            for (auto& tc : m.toolCalls)
                tcs.push_back({{"id", tc["id"]}, {"type", "function"},
                               {"function", {{"name", tc["name"]}, {"arguments", tc["arguments"]}}}});
            msg["tool_calls"] = tcs;
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(msg);
    }
    j["messages"] = msgs;
    j["tools"] = json::parse(ToolDefsJson());
    j["tool_choice"] = "auto";
    return j.dump();
}

void App::SendMessage() {
    std::string msg = msgBuf_;
    if (msg.empty() || !connected_ || requestInProgress_) return;

    scrollView_.AppendStream(BlockType::USER_PROMPT, msg);
    msgBuf_[0] = '\0';

    history_.push_back({"user", msg, {}, {}});
    requestInProgress_ = true;
    toolRound_ = 0;
    responseBuffer_.clear();
    receivingThinking_ = false;
    lastMarkdownLen_ = 0;
    lastMarkdownTime_ = GetMonotonicTime();
    MarkDirty();

    DoSendToProvider();
}

void App::DoSendToProvider() {
    if (activeProvider_ == "claude") {
        // TODO: proper ACP
        AppendSystem("Claude ACP not yet implemented in this build.");
        requestInProgress_ = false;
        return;
    }

    std::string requestJson = BuildRequestJson();

    httpClient_.SendStreaming(requestJson,
        [this](const std::string& chunk, bool isThinking) {
            events_.Push([this, chunk, isThinking]() {
                if (isThinking) {
                    if (!receivingThinking_) {
                        receivingThinking_ = true;
                        scrollView_.AppendStream(BlockType::THINKING, chunk);
                        scrollView_.StartThinking(scrollView_.BlockCount() - 1);
                    } else {
                        scrollView_.ContinueStream(chunk);
                    }
                } else {
                    if (receivingThinking_) {
                        receivingThinking_ = false;
                        scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                        responseStartBlock_ = scrollView_.BlockCount();
                        responseBuffer_.clear();
                        lastMarkdownLen_ = 0;
                    }
                    if (responseBuffer_.empty())
                        responseStartBlock_ = scrollView_.BlockCount();
                    responseBuffer_ += chunk;

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
        [this](bool ok, const std::string& content, const std::string& error,
               const std::vector<json>& toolCalls, int, int) {
            events_.Push([this, ok, content, error, toolCalls]() {
                if (!ok) {
                    if (!history_.empty() && history_.back().role == "user")
                        history_.pop_back();
                    AppendSystem("Error: " + error);
                    requestInProgress_ = false;
                    MarkDirty();
                    return;
                }
                if (!toolCalls.empty() && toolRound_ < 10) {
                    ExecuteToolCalls(toolCalls, content);
                    return;
                }
                if (receivingThinking_) {
                    receivingThinking_ = false;
                    scrollView_.StopThinking(scrollView_.BlockCount() - 1);
                }
                if (!responseBuffer_.empty()) {
                    RenderMarkdownToBlocks(true);
                    responseBuffer_.clear();
                }
                if (!content.empty())
                    history_.push_back({"assistant", content, {}, {}});
                requestInProgress_ = false;
                scrollView_.StopAllAnimations();
                MarkDirty();
            });
        }
    );
}

void App::RenderMarkdownToBlocks(bool isFinal) {
    if (responseBuffer_.empty()) return;
    std::string toRender = responseBuffer_;
    if (!isFinal) {
        size_t lastBoundary = toRender.rfind("\n\n");
        if (lastBoundary == std::string::npos || lastBoundary < 10) return;
        toRender = toRender.substr(0, lastBoundary);
    }
    scrollView_.RemoveBlocksFrom(responseStartBlock_);
    MarkdownRenderer mdRenderer(14);
    auto mdBlocks = mdRenderer.Render(toRender, true);
    scrollView_.BeginBatch();
    scrollView_.AddBlocks(std::move(mdBlocks));
    scrollView_.EndBatch();
}

void App::ExecuteToolCalls(const std::vector<json>& toolCalls, const std::string& content) {
    toolRound_++;
    history_.push_back({"assistant", content, toolCalls, {}});

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

    auto tcCopy = toolCalls;
    std::thread([this, tcCopy]() {
        struct Result { std::string id, output; };
        std::vector<Result> results;
        for (auto& tc : tcCopy)
            results.push_back({tc.value("id", ""), ExecuteTool(tc.value("name", ""), tc.value("arguments", "{}"))});

        events_.Push([this, results, tcCopy]() {
            std::string resultText;
            for (size_t i = 0; i < results.size(); i++)
                resultText += "\n" + tcCopy[i].value("name", "") + ":\n" + StripAnsi(results[i].output) + "\n";
            scrollView_.ContinueStream(resultText);

            for (size_t i = 0; i < results.size(); i++)
                history_.push_back({"tool", results[i].output, {}, results[i].id});

            scrollView_.StopThinking(scrollView_.BlockCount() - 1);
            DoSendToProvider();
            MarkDirty();
        });
    }).detach();
}

// ============================================================================
// Main loop
// ============================================================================

void App::Run() {
    while (!window_.ShouldClose()) {
        window_.PollEvents();
        events_.Drain();

        scrollView_.Update(0.016f);

        // GL render
        int fbW = window_.Width(), fbH = window_.Height();
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render scroll view via our GL renderer
        int barH = 90;
        int viewH = fbH - barH;
        glViewport(0, barH, fbW, viewH);
        renderer_.BeginFrame(fbW, viewH, scrollView_.Fonts());
        scrollView_.Paint(renderer_);
        renderer_.EndFrame();

        // Render ImGui (bottom bar)
        glViewport(0, 0, fbW, fbH);
        DrawImGui();

        window_.SwapBuffers();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}
