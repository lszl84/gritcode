#include "chat_frame.h"
#include "inline_parser.h"
#include "tools.h"
#include "preferences.h"
#include "settings_dialog.h"
#include <wx/sizer.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/dirdlg.h>
#include <wx/bmpbndl.h>
#include <wx/settings.h>
#include <wx/stdpaths.h>
#include <future>
#include <sstream>
#include <cstdlib>
#include <unistd.h>

namespace {

constexpr int ID_SEND     = wxID_HIGHEST + 1;
constexpr int ID_INPUT    = wxID_HIGHEST + 2;
constexpr int ID_SESSION  = wxID_HIGHEST + 10;
constexpr int ID_MODEL    = wxID_HIGHEST + 11;
constexpr int ID_SETTINGS = wxID_HIGHEST + 12;
constexpr int kMaxToolIters = 10;

// Per-model routing config. Resolved fresh at each StartCompletion so a model
// change during a tool-call loop applies on the next request.
struct ModelRoute {
    const char* url;
    const char* model;
    bool needsApiKey;
    Preferences::Provider provider;  // only valid when needsApiKey
};

ModelRoute RouteFor(ModelChoice m) {
    switch (m) {
    case ModelChoice::OpencodeFree:
        return {"https://opencode.ai/zen/v1/chat/completions",
                "minimax-m2.5-free", false, Preferences::Provider::DeepSeek};
    case ModelChoice::DeepseekFlash:
        return {"https://api.deepseek.com/chat/completions",
                "deepseek-v4-flash", true, Preferences::Provider::DeepSeek};
    case ModelChoice::DeepseekPro:
        return {"https://api.deepseek.com/chat/completions",
                "deepseek-v4-pro", true, Preferences::Provider::DeepSeek};
    }
    return {"https://opencode.ai/zen/v1/chat/completions",
            "minimax-m2.5-free", false, Preferences::Provider::DeepSeek};
}

// chdir() into the session's directory so tool subprocesses (bash,
// list_directory with relative paths, etc.) operate against it. If the
// directory no longer exists or isn't accessible, fall back to $HOME so
// we don't strand subprocesses in some inherited cwd.
void ChdirToCwd(const std::string& cwd) {
    if (cwd.empty()) return;
    if (chdir(cwd.c_str()) == 0) return;
    if (const char* home = std::getenv("HOME")) {
        chdir(home);
    }
}

// Resolve $HOME as the default cwd when the index has no last-active entry.
std::string DefaultCwd() {
    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') return home;
    }
    return ".";
}

// Format an absolute path for display in the dropdown. Replaces $HOME with
// "~" and drops trailing slashes — matches the typical shell/IDE convention.
wxString DisplayPath(const std::string& cwd) {
    if (const char* home = std::getenv("HOME")) {
        std::string h = home;
        if (!h.empty() && cwd.rfind(h, 0) == 0) {
            std::string rest = cwd.substr(h.size());
            return wxString::FromUTF8("~" + rest);
        }
    }
    return wxString::FromUTF8(cwd);
}

// Resolve the assets directory. Installed builds put SVGs under
// <prefix>/share/wx_gritcode (resolved via wxStandardPaths::GetResourcesDir,
// which derives the prefix from the executable path). Dev builds fall back to
// the source tree path baked in at compile time.
const wxString& GetAssetsDir() {
    static wxString cached = []() -> wxString {
        wxString installed = wxStandardPaths::Get().GetResourcesDir();
        if (wxFileName::DirExists(installed + "/icons")) return installed;
        return wxString(WXG_ASSETS_DIR);
    }();
    return cached;
}

// Load an SVG icon from disk and recolor its #FFFFFF fills with `accent`.
// The original assets were authored white for dark mode; substituting at load
// time lets the icons follow the system theme without shipping a second set.
wxBitmapBundle LoadThemedSvgIcon(const wxString& name, const wxSize& size,
                                 const wxColour& accent) {
    wxString path = GetAssetsDir() + "/icons/" + name;
    wxFile f(path);
    wxString svg;
    if (!f.IsOpened() || !f.ReadAll(&svg)) {
        return wxBitmapBundle::FromSVGFile(path, size);  // fallback
    }
    wxString hex = wxString::Format("#%02X%02X%02X",
                                    accent.Red(), accent.Green(), accent.Blue());
    svg.Replace("#FFFFFF", hex, true);
    svg.Replace("#ffffff", hex, true);
    auto utf8 = svg.utf8_string();
    return wxBitmapBundle::FromSVG(
        reinterpret_cast<const wxByte*>(utf8.data()), utf8.size(), size);
}

}  // namespace

ChatFrame::ChatFrame()
    : wxFrame(nullptr, wxID_ANY, "wx_gritcode",
              wxDefaultPosition, wxSize(600, 850)) {

    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    canvas_ = new ChatCanvas(panel);
    outer->Add(canvas_, 1, wxEXPAND);

    // Toolbar row below the input:
    //   Session: [▾]   Model: [▾]   <stretch>   [⚙]
    const wxSize kIconSize(20, 20);
    const wxSize kBtnSize(FromDIP(32), -1);
    wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    wxBitmapBundle bbSettings = LoadThemedSvgIcon("settings.svg", kIconSize, accent);

    auto* toolbarRow = new wxBoxSizer(wxHORIZONTAL);
    auto* sessionLabel = new wxStaticText(panel, wxID_ANY, "Session:");
    sessionChoice_ = new wxChoice(panel, ID_SESSION);
    sessionChoice_->SetMinSize(FromDIP(wxSize(220, -1)));
    auto* modelLabel = new wxStaticText(panel, wxID_ANY, "Model:");
    modelChoice_ = new wxChoice(panel, ID_MODEL);
    modelChoice_->Append("OpenCode Free");
    modelChoice_->Append("DeepSeek V4 Flash");
    modelChoice_->Append("DeepSeek V4 Pro");
    currentModel_ = static_cast<ModelChoice>(Preferences::GetLastModelIndex());
    modelChoice_->SetSelection(static_cast<int>(currentModel_));
    settingsBtn_ = new wxBitmapButton(panel, ID_SETTINGS, bbSettings,
                                      wxDefaultPosition, kBtnSize,
                                      wxBORDER_NONE);
    settingsBtn_->SetToolTip(wxString::FromUTF8("Settings…"));

    toolbarRow->Add(sessionLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    // Session and model both get a small proportion so they share resize delta;
    // the stretch spacer absorbs most of it. Once the spacer collapses (narrow
    // window) both dropdowns shrink toward their MinSize.
    toolbarRow->Add(sessionChoice_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    toolbarRow->Add(modelLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    toolbarRow->Add(modelChoice_, 1, wxALIGN_CENTER_VERTICAL);
    toolbarRow->AddStretchSpacer(8);
    toolbarRow->Add(settingsBtn_, 0, wxALIGN_CENTER_VERTICAL);

    auto* inputRow = new wxBoxSizer(wxHORIZONTAL);
    input_ = new wxTextCtrl(panel, ID_INPUT, "",
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    sendBtn_ = new wxButton(panel, ID_SEND, "Send");
    inputRow->Add(input_, 1, wxEXPAND | wxTOP, 6);
    inputRow->Add(sendBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 6);
    outer->Add(inputRow, 0, wxEXPAND);
    outer->Add(toolbarRow, 0, wxEXPAND | wxTOP, 4);

    panel->SetSizer(outer);

    // Open the most recent session if one exists; otherwise seed one for the
    // default cwd so the dropdown always has at least one entry.
    store_.Init();
    bool restored = false;
    if (auto last = store_.LastActiveCwd()) {
        std::vector<nlohmann::json> hist;
        if (store_.Load(*last, hist)) {
            history_ = std::move(hist);
            activeCwd_ = *last;
            restored = true;
        }
    }
    if (!restored && !store_.List().empty()) {
        const auto& e = store_.List().front();  // most recent
        std::vector<nlohmann::json> hist;
        if (store_.Load(e.cwd, hist)) {
            history_ = std::move(hist);
            activeCwd_ = e.cwd;
            restored = true;
        }
    }
    if (!restored) {
        activeCwd_ = DefaultCwd();
        SeedSystemPrompt();
        store_.Save(activeCwd_, history_);
        store_.SetLastActiveCwd(activeCwd_);
    }
    ChdirToCwd(activeCwd_);
    RefreshSessionChoice();
    RestoreCanvasFromHistory();

    Bind(wxEVT_BUTTON, &ChatFrame::OnSend, this, ID_SEND);
    input_->Bind(wxEVT_KEY_DOWN, &ChatFrame::OnInputKey, this);
    Bind(wxEVT_CLOSE_WINDOW, &ChatFrame::OnClose, this);
    Bind(wxEVT_WEBREQUEST_DATA, &ChatFrame::OnWebRequestData, this);
    Bind(wxEVT_WEBREQUEST_STATE, &ChatFrame::OnWebRequestState, this);
    Bind(wxEVT_BUTTON, &ChatFrame::OnSettings, this, ID_SETTINGS);
    sessionChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnSessionChoice, this);
    modelChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnModelChoice, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED,
         [this](wxSysColourChangedEvent& e) { ReloadToolbarIcons(); e.Skip(); });

    // Helper: bounce a value-returning closure onto the GUI thread and block
    // the calling (MCP) thread until it has returned.
    auto guiSync = [this](auto fn) -> nlohmann::json {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        CallAfter([promise, fn = std::move(fn)]() mutable {
            try { promise->set_value(fn()); }
            catch (...) { promise->set_exception(std::current_exception()); }
        });
        return future.get();
    };

    MCPCallbacks cb;
    cb.getStatus = [this, guiSync]() {
        return guiSync([this]() -> nlohmann::json {
            return {
                {"streaming", streaming_},
                {"toolIter", toolIter_},
                {"historyLen", (int)history_.size()},
            };
        });
    };
    cb.getConversation = [this, guiSync]() {
        return guiSync([this]() { return BuildConversationSnapshot(); });
    };
    cb.getLastAssistant = [this, guiSync]() {
        return guiSync([this]() -> nlohmann::json {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                if (it->value("role", std::string{}) != "assistant") continue;
                if (!it->contains("content") || !(*it)["content"].is_string()) continue;
                return {{"text", (*it)["content"].get<std::string>()}};
            }
            return {{"text", ""}};
        });
    };
    cb.sendMessage = [this, guiSync](const std::string& msg) -> nlohmann::json {
        wxString text = wxString::FromUTF8(msg);
        // Synchronous part: validate state and stage the input. Done on the
        // GUI thread under guiSync so the caller learns immediately whether
        // the message was accepted.
        return guiSync([this, text]() -> nlohmann::json {
            if (streaming_) {
                return {{"sent", false}, {"reason", "streaming"}};
            }
            input_->SetValue(text);
            // Defer the actual OnSend → StartCompletion → wxWebRequest::Start
            // chain to a later event-loop iteration. This lets any pending
            // wxWebRequest state cleanup (especially after a recent Cancel())
            // fully drain before a new TLS connection is opened — without
            // the deferral, libcurl-multi has been observed to reuse a
            // freshly-closed fd in ways that scramble unrelated sockets.
            CallAfter([this]() {
                wxCommandEvent ev(wxEVT_BUTTON, ID_SEND);
                OnSend(ev);
            });
            return {{"sent", true}};
        });
    };
    cb.cancelRequest = [this]() {
        CallAfter([this]() {
            if (!request_.IsOk()) return;
            auto s = request_.GetState();
            if (s == wxWebRequest::State_Active ||
                s == wxWebRequest::State_Unauthorized) {
                request_.Cancel();
            }
        });
    };
    cb.getBlocks = [this, guiSync]() {
        return guiSync([this]() { return BuildBlocksSnapshot(); });
    };
    cb.toggleTool = [this](int idx) {
        CallAfter([this, idx]() { canvas_->ToggleToolCall(idx); });
    };
    cb.listSessions = [this, guiSync]() {
        return guiSync([this]() -> nlohmann::json {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : store_.List()) {
                arr.push_back({
                    {"id", e.id},
                    {"cwd", e.cwd},
                    {"lastUsed", e.lastUsed},
                });
            }
            return {{"sessions", arr}, {"activeCwd", activeCwd_}};
        });
    };
    cb.switchSession = [this, guiSync](const std::string& cwd) -> nlohmann::json {
        return guiSync([this, cwd]() -> nlohmann::json {
            if (streaming_) return {{"ok", false}, {"reason", "streaming"}};
            if (cwd == activeCwd_) return {{"ok", true}};
            SwitchToCwd(cwd);
            return {{"ok", true}};
        });
    };
    cb.setModel = [this, guiSync](int idx) -> nlohmann::json {
        return guiSync([this, idx]() -> nlohmann::json {
            if (idx < 0 || idx > 2) return {{"ok", false}, {"reason", "out of range"}};
            currentModel_ = static_cast<ModelChoice>(idx);
            modelChoice_->SetSelection(idx);
            Preferences::SetLastModelIndex(idx);
            return {{"ok", true}, {"modelIndex", idx}};
        });
    };
    cb.getPreferences = [guiSync]() -> nlohmann::json {
        return guiSync([]() -> nlohmann::json {
            return {
                {"modelIndex", Preferences::GetLastModelIndex()},
                {"hasDeepseekKey",
                 Preferences::HasApiKey(Preferences::Provider::DeepSeek)},
            };
        });
    };
    cb.newSession = [this, guiSync]() -> nlohmann::json {
        return guiSync([this]() -> nlohmann::json {
            if (streaming_) return {{"ok", false}, {"reason", "streaming"}};
            // MCP-driven new session uses the index's default cwd as a fresh
            // sentinel rather than popping a directory dialog (the dialog
            // would block the GUI thread and is awkward to drive from tests).
            // The interactive flow goes through OnSessionChoice → directory
            // picker. Tests can use switchSession with an arbitrary cwd to
            // create entries on demand.
            std::string newCwd = DefaultCwd();
            PersistActive();
            std::vector<nlohmann::json> hist;
            if (store_.Load(newCwd, hist)) {
                activeCwd_ = newCwd;
                history_ = std::move(hist);
            } else {
                activeCwd_ = newCwd;
                history_.clear();
                SeedSystemPrompt();
                store_.Save(activeCwd_, history_);
            }
            store_.SetLastActiveCwd(activeCwd_);
            ChdirToCwd(activeCwd_);
            canvas_->Clear();
            RestoreCanvasFromHistory();
            RefreshSessionChoice();
            return {{"ok", true}, {"cwd", activeCwd_}};
        });
    };
    mcp_.Start(std::move(cb));

    input_->SetFocus();
    SetMinSize(wxSize(500, 400));
}

ChatFrame::~ChatFrame() {
    mcp_.Stop();
    if (request_.IsOk() && request_.GetState() == wxWebRequest::State_Active) {
        request_.Cancel();
    }
}

nlohmann::json ChatFrame::BuildBlocksSnapshot() const {
    auto typeName = [](BlockType t) -> const char* {
        switch (t) {
        case BlockType::Paragraph:  return "paragraph";
        case BlockType::Heading:    return "heading";
        case BlockType::CodeBlock:  return "code";
        case BlockType::UserPrompt: return "user";
        case BlockType::Table:      return "table";
        case BlockType::ToolCall:   return "tool";
        }
        return "?";
    };
    auto alignName = [](TableAlign a) -> const char* {
        switch (a) {
        case TableAlign::Left:   return "L";
        case TableAlign::Center: return "C";
        case TableAlign::Right:  return "R";
        }
        return "?";
    };

    nlohmann::json out = nlohmann::json::array();
    constexpr int kPreview = 200;
    for (const auto& b : canvas_->Blocks()) {
        nlohmann::json e;
        e["type"] = typeName(b.type);
        e["height"] = b.cachedHeight;
        e["nLines"] = (int)b.lines.size();
        wxString preview = b.visibleText.Length() > kPreview
            ? b.visibleText.Left(kPreview) + "..."
            : b.visibleText;
        e["preview"] = preview.ToStdString();
        if (b.type == BlockType::Heading) {
            e["level"] = b.headingLevel;
        } else if (b.type == BlockType::CodeBlock) {
            e["lang"] = b.lang.ToStdString();
            e["bodyChars"] = (int)b.rawText.Length();
        } else if (b.type == BlockType::Table) {
            e["rows"] = (int)b.tableRows.size();
            e["cols"] = b.tableRows.empty() ? 0 : (int)b.tableRows[0].size();
            nlohmann::json aligns = nlohmann::json::array();
            for (auto a : b.tableAligns) aligns.push_back(alignName(a));
            e["aligns"] = std::move(aligns);
        } else if (b.type == BlockType::ToolCall) {
            e["toolName"] = b.toolName.ToStdString();
            e["toolArgs"] = b.toolArgs.ToStdString();
            wxString rprev = b.toolResult.Length() > kPreview
                ? b.toolResult.Left(kPreview) + "..."
                : b.toolResult;
            e["toolResultPreview"] = rprev.ToStdString();
            e["toolResultChars"] = (int)b.toolResult.Length();
            e["expanded"] = b.toolExpanded;
        }
        out.push_back(std::move(e));
    }
    return out;
}

nlohmann::json ChatFrame::BuildConversationSnapshot() const {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& m : history_) {
        nlohmann::json e;
        e["role"] = m.value("role", std::string{});
        if (m.contains("content") && m["content"].is_string()) {
            e["content"] = m["content"].get<std::string>();
        } else {
            e["content"] = nullptr;
        }
        if (m.contains("tool_calls")) e["tool_calls"] = m["tool_calls"];
        if (m.contains("tool_call_id")) e["tool_call_id"] = m["tool_call_id"];
        if (m.contains("name")) e["name"] = m["name"];
        if (m.contains("reasoning_content"))
            e["reasoning_content"] = m["reasoning_content"];
        out.push_back(std::move(e));
    }
    return out;
}

void ChatFrame::OnClose(wxCloseEvent& evt) {
    if (request_.IsOk() && request_.GetState() == wxWebRequest::State_Active) {
        quitRequested_ = true;
        Hide();
        CallAfter([this]() { request_.Cancel(); });
        evt.Veto();
    } else {
        PersistActive();
        evt.Skip();
    }
}

void ChatFrame::OnInputKey(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_RETURN && !e.ShiftDown() && !e.ControlDown()) {
        wxCommandEvent ev(wxEVT_BUTTON, ID_SEND);
        OnSend(ev);
        return;
    }
    e.Skip();
}

void ChatFrame::ReloadToolbarIcons() {
    const wxSize kIconSize(20, 20);
    wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    settingsBtn_->SetBitmap(LoadThemedSvgIcon("settings.svg", kIconSize, accent));
}

void ChatFrame::RefreshSessionChoice() {
    sessionChoice_->Clear();
    sessionCwds_.clear();
    sessionChoice_->Append(wxString::FromUTF8("New Session\xE2\x80\xA6"));
    int activeRow = -1;
    for (const auto& e : store_.List()) {
        sessionChoice_->Append(DisplayPath(e.cwd));
        sessionCwds_.push_back(e.cwd);
        if (e.cwd == activeCwd_) activeRow = (int)sessionCwds_.size();
    }
    // Fall back to first real session if active cwd wasn't found in the list
    // (shouldn't happen — every saved session is in the index).
    if (activeRow < 1 && sessionCwds_.size() >= 1) activeRow = 1;
    if (activeRow >= 1) sessionChoice_->SetSelection(activeRow);
    // Tooltip shows the absolute path even when the dropdown collapses ~/ .
    if (activeRow >= 1) {
        sessionChoice_->SetToolTip(
            wxString::FromUTF8(sessionCwds_[activeRow - 1]));
    }
}

void ChatFrame::OnSessionChoice(wxCommandEvent& evt) {
    int sel = evt.GetSelection();
    if (sel == 0) {
        // "New Session…" sentinel — snap selection back so it never sticks,
        // then start the new-session flow.
        for (int i = 0; i < (int)sessionCwds_.size(); ++i) {
            if (sessionCwds_[i] == activeCwd_) {
                sessionChoice_->SetSelection(i + 1);
                break;
            }
        }
        if (streaming_) {
            wxMessageBox("Cannot switch sessions while streaming.",
                         "wx_gritcode", wxOK | wxICON_INFORMATION, this);
            return;
        }
        CreateNewSession();
        return;
    }
    int idx = sel - 1;
    if (idx < 0 || idx >= (int)sessionCwds_.size()) return;
    const std::string& target = sessionCwds_[idx];
    if (target == activeCwd_) return;
    if (streaming_) {
        // Restore prior selection and warn.
        for (int i = 0; i < (int)sessionCwds_.size(); ++i) {
            if (sessionCwds_[i] == activeCwd_) {
                sessionChoice_->SetSelection(i + 1);
                break;
            }
        }
        wxMessageBox("Cannot switch sessions while streaming.",
                     "wx_gritcode", wxOK | wxICON_INFORMATION, this);
        return;
    }
    SwitchToCwd(target);
}

void ChatFrame::CreateNewSession() {
    // Pop a directory picker so the user can name the session by folder.
    // Default to $HOME so they start at a familiar root.
    wxString defaultDir = wxString::FromUTF8(DefaultCwd());
    wxDirDialog dlg(this, "Choose a folder for the new session",
                    defaultDir,
                    wxDD_DEFAULT_STYLE);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string chosen = dlg.GetPath().ToStdString(wxConvUTF8);
    if (chosen.empty()) return;
    if (chosen == activeCwd_) return;  // already on this session

    PersistActive();
    activeCwd_ = chosen;
    std::vector<nlohmann::json> hist;
    if (store_.Load(activeCwd_, hist)) {
        // Existing session for this folder — restore it.
        history_ = std::move(hist);
    } else {
        // Brand new folder: seed a fresh system prompt.
        history_.clear();
        SeedSystemPrompt();
        store_.Save(activeCwd_, history_);
    }
    store_.SetLastActiveCwd(activeCwd_);
    canvas_->Clear();
    RestoreCanvasFromHistory();
    RefreshSessionChoice();
}

void ChatFrame::SwitchToCwd(const std::string& cwd) {
    PersistActive();
    std::vector<nlohmann::json> hist;
    bool existed = store_.Load(cwd, hist);
    activeCwd_ = cwd;
    history_ = std::move(hist);
    if (!existed) {
        // Brand new folder: seed a fresh system prompt and persist so the
        // session shows up in the index immediately.
        history_.clear();
        SeedSystemPrompt();
        store_.Save(activeCwd_, history_);
    }
    store_.SetLastActiveCwd(activeCwd_);
    ChdirToCwd(activeCwd_);
    canvas_->Clear();
    RestoreCanvasFromHistory();
    RefreshSessionChoice();
}

void ChatFrame::PersistActive() {
    if (activeCwd_.empty()) return;
    // Always save — keeps the lastUsed timestamp current so the dropdown
    // ordering is stable and the index reflects the most recent activity.
    store_.Save(activeCwd_, history_);
}

void ChatFrame::SeedSystemPrompt() {
    history_.push_back({
        {"role", "system"},
        {"content",
         "You are a helpful AI coding assistant with access to local file, "
         "shell, and web tools. Use them when the user's request requires "
         "reading files, running shell commands, or fetching web pages. "
         "Prefer concrete actions over speculation. Use markdown for "
         "formatting and fenced code blocks for code."}
    });
}

void ChatFrame::RestoreCanvasFromHistory() {
    // Walk the message list and re-emit blocks. Tool call/result pairs are
    // stitched back together: an assistant message's tool_calls are matched
    // against the immediately-following "tool" messages by tool_call_id.
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto& m = history_[i];
        std::string role = m.value("role", std::string{});

        if (role == "user") {
            if (!m.contains("content") || !m["content"].is_string()) continue;
            wxString text = wxString::FromUTF8(m["content"].get<std::string>());
            Block ub;
            ub.type = BlockType::UserPrompt;
            ub.rawText = text;
            InlineRun r; r.text = text;
            ub.runs.push_back(r);
            ub.visibleText = text;
            canvas_->AddBlock(std::move(ub));
            continue;
        }

        if (role == "assistant") {
            // 1. Render content (if any) through the markdown stream.
            if (m.contains("content") && m["content"].is_string()) {
                std::string content = m["content"].get<std::string>();
                if (!content.empty()) {
                    MdStream md([this](Block b) {
                        canvas_->AddBlock(std::move(b));
                    });
                    md.Feed(wxString::FromUTF8(content));
                    md.Flush();
                }
            }
            // 2. Render tool_calls paired with subsequent tool results.
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    if (!tc.is_object() || !tc.contains("function")) continue;
                    std::string id = tc.value("id", std::string{});
                    const auto& fn = tc["function"];
                    std::string name = fn.value("name", std::string{});
                    std::string args = fn.value("arguments", std::string{});

                    // Find the matching tool result. It should be in the next
                    // few messages with role=tool and matching tool_call_id.
                    std::string result;
                    for (size_t j = i + 1; j < history_.size(); ++j) {
                        const auto& r = history_[j];
                        if (r.value("role", std::string{}) != "tool") continue;
                        if (r.value("tool_call_id", std::string{}) != id) continue;
                        if (r.contains("content") && r["content"].is_string()) {
                            result = r["content"].get<std::string>();
                        }
                        break;
                    }
                    RenderToolBlock(name, args, result);
                }
            }
            continue;
        }

        // role == "system" or "tool": skipped (system not visible; tool was
        // consumed above by the matching assistant pairing).
    }
}

void ChatFrame::OnModelChoice(wxCommandEvent& evt) {
    int sel = evt.GetSelection();
    if (sel < 0 || sel > 2) return;
    currentModel_ = static_cast<ModelChoice>(sel);
    Preferences::SetLastModelIndex(sel);
}

void ChatFrame::OnSettings(wxCommandEvent&) {
    SettingsDialog dlg(this);
    dlg.ShowModal();
}

void ChatFrame::OnSend(wxCommandEvent&) {
    if (streaming_) return;
    wxString text = input_->GetValue();
    text.Trim().Trim(false);
    if (text.IsEmpty()) return;
    input_->Clear();
    StartTurn(text);
}

void ChatFrame::StartTurn(const wxString& userText) {
    // Render the user prompt block immediately.
    Block ub;
    ub.type = BlockType::UserPrompt;
    ub.rawText = userText;
    InlineRun r;
    r.text = userText;
    ub.runs.push_back(r);
    ub.visibleText = userText;
    canvas_->AddBlock(std::move(ub));

    history_.push_back({{"role", "user"},
                        {"content", userText.ToStdString(wxConvUTF8)}});

    streaming_ = true;
    canvas_->SetThinking(true);
    sendBtn_->Disable();
    toolIter_ = 0;

    StartCompletion();
}

void ChatFrame::StartCompletion() {
    // Reset per-completion stream state.
    activeAssistantText_.clear();
    activeReasoning_.clear();
    activeToolCalls_.clear();
    sseBuf_.clear();
    mdStream_ = std::make_unique<MdStream>([this](Block b) {
        canvas_->AddBlock(std::move(b));
    });

    ModelRoute route = RouteFor(currentModel_);

    // Build an outbound copy of history with the active cwd appended to the
    // system prompt. Done per-request rather than baked into stored history
    // so the cwd stays current if the user switches sessions mid-conversation
    // and doesn't accumulate across turns.
    nlohmann::json messages = history_;
    if (!activeCwd_.empty() && !messages.empty() && messages[0].is_object()
        && messages[0].value("role", std::string{}) == "system") {
        std::string base = messages[0].value("content", std::string{});
        messages[0]["content"] =
            base + "\n\nWorking directory: " + activeCwd_;
    }

    // Defensive: drop consecutive same-role user/assistant messages. Both
    // OpenAI and DeepSeek 400 if two user (or two assistant) messages are
    // adjacent — keep only the last in any such run so a previously poisoned
    // history (e.g. from a series of failed attempts) still sends cleanly.
    {
        nlohmann::json deduped = nlohmann::json::array();
        for (auto& m : messages) {
            if (!deduped.empty() && deduped.back().is_object() && m.is_object()) {
                std::string prev = deduped.back().value("role", std::string{});
                std::string cur = m.value("role", std::string{});
                if (prev == cur && (cur == "user" || cur == "assistant")) {
                    deduped.back() = m;
                    continue;
                }
            }
            deduped.push_back(m);
        }
        messages = std::move(deduped);
    }

    // DeepSeek's reasoning models reject any request whose history has an
    // assistant message without a `reasoning_content` field (even an empty
    // one). For OpenCode-originated history or assistant turns where we never
    // captured reasoning, inject an empty string to keep the request valid.
    if (route.provider == Preferences::Provider::DeepSeek
        && route.needsApiKey) {
        for (auto& m : messages) {
            if (m.is_object()
                && m.value("role", std::string{}) == "assistant"
                && !m.contains("reasoning_content")) {
                m["reasoning_content"] = "";
            }
        }
    }

    // For paid providers, refuse to send if no key is configured. The user
    // gets a clear nudge instead of an opaque HTTP 401 from the upstream.
    wxString apiKey;
    if (route.needsApiKey) {
        apiKey = Preferences::GetApiKey(route.provider);
        if (apiKey.IsEmpty()) {
            RenderErrorBlock(
                "No API key configured for the selected model. "
                "Open Settings (gear icon) to add one.");
            FinalizeTurn();
            return;
        }
    }

    nlohmann::json req;
    req["model"] = route.model;
    req["stream"] = true;
    req["max_tokens"] = 4096;
    req["messages"] = std::move(messages);
    req["tools"] = GetToolDefinitions();

    // error_handler_t::replace silently swaps invalid UTF-8 bytes in any
    // history string (bash output, file contents, model glitches, user paste)
    // for U+FFFD instead of throwing type_error.316.
    std::string body = req.dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace);

    request_ = wxWebSession::GetDefault().CreateRequest(this, route.url);
    if (!request_.IsOk()) {
        RenderErrorBlock("wxWebRequest creation failed");
        FinalizeTurn();
        return;
    }

    request_.SetMethod("POST");
    request_.SetHeader("Content-Type", "application/json");
    request_.SetHeader("Accept", "text/event-stream");
    if (route.needsApiKey) {
        request_.SetHeader("Authorization", "Bearer " + apiKey);
    }
    request_.SetData(wxString::FromUTF8(body), "application/json");
    request_.SetStorage(wxWebRequest::Storage_None);

    request_.Start();
}

void ChatFrame::OnWebRequestData(wxWebRequestEvent& evt) {
    const void* buf = evt.GetDataBuffer();
    size_t size = evt.GetDataSize();
    if (!buf || !size) return;

    sseBuf_.append(static_cast<const char*>(buf), size);

    size_t pos;
    while ((pos = sseBuf_.find("\n\n")) != std::string::npos) {
        std::string event = sseBuf_.substr(0, pos);
        sseBuf_.erase(0, pos + 2);

        std::istringstream stream(event);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data: ", 0) != 0) continue;
            std::string payload = line.substr(6);
            if (payload == "[DONE]") continue;
            try {
                auto j = nlohmann::json::parse(payload);
                if (!j.contains("choices") || j["choices"].empty()) continue;
                const auto& choice = j["choices"][0];
                if (!choice.contains("delta")) continue;
                const auto& delta = choice["delta"];

                // ---- text content ----
                // DeepSeek streams reasoning bytes in `reasoning_content` and
                // visible bytes in `content` (potentially in the same delta).
                // Other providers may use `reasoning` or `reasoning_text`. We
                // capture reasoning into activeReasoning_ so it can be round-
                // tripped on the next request — DeepSeek requires this on every
                // assistant message in history or returns 400.
                for (const char* f : {"reasoning_content", "reasoning", "reasoning_text"}) {
                    if (delta.contains(f) && delta[f].is_string()) {
                        activeReasoning_ += delta[f].get<std::string>();
                        break;
                    }
                }
                if (delta.contains("content") && delta["content"].is_string()) {
                    std::string content = delta["content"].get<std::string>();
                    if (!content.empty()) {
                        activeAssistantText_ += content;
                        if (mdStream_) mdStream_->Feed(wxString::FromUTF8(content));
                    }
                }

                // ---- tool_calls ----
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto& tc : delta["tool_calls"]) {
                        int idx = tc.value("index", 0);
                        if ((int)activeToolCalls_.size() <= idx)
                            activeToolCalls_.resize(idx + 1);
                        auto& cur = activeToolCalls_[idx];
                        if (tc.contains("id") && tc["id"].is_string())
                            cur.id = tc["id"].get<std::string>();
                        if (tc.contains("function") && tc["function"].is_object()) {
                            const auto& fn = tc["function"];
                            if (fn.contains("name") && fn["name"].is_string())
                                cur.name = fn["name"].get<std::string>();
                            if (fn.contains("arguments") && fn["arguments"].is_string())
                                cur.args += fn["arguments"].get<std::string>();
                        }
                    }
                }
            } catch (...) {
                // Ignore malformed events (keepalives, partial JSON, etc.).
            }
        }
    }
}

void ChatFrame::OnWebRequestState(wxWebRequestEvent& evt) {
    auto state = evt.GetState();

    if (state == wxWebRequest::State_Active) return;

    if (state == wxWebRequest::State_Cancelled) {
        if (quitRequested_) {
            Close();
            return;
        }
        // streaming_=false means we already finalized this turn through
        // another path (e.g. State_Unauthorized → Cancel()) and the Cancelled
        // event is a tail signal from that internal cleanup. Don't double-
        // render the "Cancelled." block on top of the real error.
        if (!streaming_) return;
        // User-initiated cancel: flush whatever streamed so far, render an
        // error block, and unlock the UI. Without this the send button stays
        // disabled and streaming_ stays true forever.
        if (mdStream_) mdStream_->Flush();
        mdStream_.reset();
        RenderErrorBlock("Cancelled.");
        FinalizeTurn();
        return;
    }

    // Flush markdown parser before deciding what to do next — any pending
    // paragraph/code block needs to land before tool blocks or the next turn.
    if (mdStream_) mdStream_->Flush();
    mdStream_.reset();

    if (state == wxWebRequest::State_Failed) {
        // wxWebRequest reports HTTP >= 400 as State_Failed when the body is
        // received via wxEVT_WEBREQUEST_DATA (Storage_None). The body bytes
        // are sitting in sseBuf_ — surface them so the user sees the actual
        // upstream error instead of an empty "(400)".
        wxString detail = "Error: " + evt.GetErrorDescription();
        wxString body = ExtractErrorBody();
        if (!body.IsEmpty()) detail += "\n\n" + body;
        HandleCompletion(detail);
        return;
    }

    if (state == wxWebRequest::State_Unauthorized) {
        // wxWebRequest expects us to call SetCredentials() to retry, but for
        // an OpenAI-style API the only meaningful response to 401 is to fail
        // hard and tell the user to fix their key. Cancel() so the request
        // moves to a terminal state and doesn't tie up the connection.
        request_.Cancel();
        wxString detail;
        auto resp = evt.GetResponse();
        if (resp.IsOk()) {
            wxString body = resp.AsString();
            detail = wxString::Format(
                "Authentication failed (HTTP %d). Check your API key in "
                "Settings.", resp.GetStatus());
            if (!body.IsEmpty() && body.length() < 400) {
                detail += "\n\n" + body;
            }
        } else {
            detail = "Authentication failed. Check your API key in Settings.";
        }
        HandleCompletion(detail);
        return;
    }

    if (state == wxWebRequest::State_Completed) {
        auto resp = evt.GetResponse();
        if (resp.GetStatus() >= 400) {
            wxString detail = wxString::Format("Error: HTTP %d", resp.GetStatus());
            wxString body = ExtractErrorBody();
            if (!body.IsEmpty()) detail += "\n\n" + body;
            HandleCompletion(detail);
            return;
        }
        HandleCompletion(wxString());
    }
}

wxString ChatFrame::ExtractErrorBody() const {
    if (sseBuf_.empty()) return wxString();
    // Try to pretty-print a JSON error like
    // {"error":{"message":"...","type":"...","code":"..."}}.
    try {
        auto j = nlohmann::json::parse(sseBuf_);
        if (j.is_object() && j.contains("error")) {
            const auto& err = j["error"];
            if (err.is_object() && err.contains("message")
                && err["message"].is_string()) {
                return wxString::FromUTF8(err["message"].get<std::string>());
            }
            if (err.is_string()) return wxString::FromUTF8(err.get<std::string>());
        }
    } catch (...) {}
    // Fall back to the raw body, capped so we don't dump megabytes.
    std::string body = sseBuf_;
    if (body.size() > 800) body = body.substr(0, 800) + "…";
    return wxString::FromUTF8(body);
}

void ChatFrame::HandleCompletion(const wxString& errorIfFailed) {
    if (!errorIfFailed.IsEmpty()) {
        RenderErrorBlock(errorIfFailed);
        // Roll back the failed turn from history_: drop any tool messages plus
        // the trailing user message that triggered this turn. Without this,
        // every retry would carry a growing tail of orphan user messages and
        // every subsequent request would 400 (consecutive same-role rule).
        // The canvas keeps the rendered prompt + error blocks so the user sees
        // what happened — only the model's view of history is rewound.
        while (!history_.empty()) {
            std::string role = history_.back().value("role", std::string{});
            if (role == "tool" || role == "assistant") {
                history_.pop_back();
                continue;
            }
            if (role == "user") {
                history_.pop_back();
            }
            break;
        }
        FinalizeTurn();
        return;
    }

    if (activeToolCalls_.empty()) {
        // Plain assistant message — record and finish.
        if (!activeAssistantText_.empty() || !activeReasoning_.empty()) {
            nlohmann::json msg = {{"role", "assistant"},
                                  {"content", activeAssistantText_}};
            if (!activeReasoning_.empty())
                msg["reasoning_content"] = activeReasoning_;
            history_.push_back(std::move(msg));
        }
        FinalizeTurn();
        return;
    }

    // Record the assistant message that triggered the tool calls. The model
    // may have produced both content and tool_calls in one turn, so include
    // both. content can be null per OpenAI spec when only tool_calls exist.
    nlohmann::json assistantMsg = {{"role", "assistant"}};
    if (activeAssistantText_.empty())
        assistantMsg["content"] = nullptr;
    else
        assistantMsg["content"] = activeAssistantText_;
    if (!activeReasoning_.empty())
        assistantMsg["reasoning_content"] = activeReasoning_;
    assistantMsg["tool_calls"] = nlohmann::json::array();
    for (const auto& tc : activeToolCalls_) {
        assistantMsg["tool_calls"].push_back({
            {"id", tc.id},
            {"type", "function"},
            {"function", {{"name", tc.name}, {"arguments", tc.args}}},
        });
    }
    history_.push_back(std::move(assistantMsg));

    // Dispatch each tool, render a block, and append the tool result message.
    for (const auto& tc : activeToolCalls_) {
        nlohmann::json args = nlohmann::json::object();
        if (!tc.args.empty()) {
            try {
                args = nlohmann::json::parse(tc.args);
            } catch (...) {
                // Leave args as empty object; DispatchTool will report missing args.
            }
        }

        std::string result = DispatchTool(tc.name, args);
        RenderToolBlock(tc.name, tc.args, result);

        history_.push_back({
            {"role", "tool"},
            {"tool_call_id", tc.id},
            {"name", tc.name},
            {"content", result},
        });
    }

    if (++toolIter_ > kMaxToolIters) {
        RenderErrorBlock(wxString::Format(
            "Tool iteration limit (%d) reached — stopping.", kMaxToolIters));
        FinalizeTurn();
        return;
    }

    // Continue the conversation with another completion request.
    StartCompletion();
}

void ChatFrame::FinalizeTurn() {
    canvas_->SetThinking(false);
    streaming_ = false;
    sendBtn_->Enable();
    activeAssistantText_.clear();
    activeToolCalls_.clear();
    PersistActive();
    // Title may have changed (first user message defines it) — refresh the
    // dropdown so the new label shows up.
    RefreshSessionChoice();
}

void ChatFrame::RenderToolBlock(const std::string& name,
                                const std::string& argsJson,
                                const std::string& result) {
    // Compact the args JSON for display: parse + dump to drop whitespace.
    std::string displayArgs = argsJson;
    try {
        if (!argsJson.empty()) {
            displayArgs = nlohmann::json::parse(argsJson).dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
    } catch (...) { /* show raw */ }

    std::string preview = result;
    constexpr size_t kPreviewCap = 4000;
    if (preview.size() > kPreviewCap) {
        preview.resize(kPreviewCap);
        preview += "\n... [preview truncated; full output sent to model]";
    }

    Block b;
    b.type = BlockType::ToolCall;
    b.toolName = wxString::FromUTF8(name);
    b.toolArgs = wxString::FromUTF8(displayArgs);
    b.toolResult = wxString::FromUTF8(preview);
    b.toolExpanded = false;
    // visibleText is what Ctrl+A → Ctrl+C will yield. Include the body so
    // selection captures the result text even though the block doesn't
    // support per-character mouse selection.
    b.visibleText = b.toolName + "(" + b.toolArgs + ")\n\n" + b.toolResult;
    canvas_->AddBlock(std::move(b));
}

void ChatFrame::RenderErrorBlock(const wxString& msg) {
    Block b;
    b.type = BlockType::Paragraph;
    b.rawText = msg;
    b.visibleText = msg;
    InlineRun r; r.text = msg; r.italic = true;
    b.runs.push_back(r);
    canvas_->AddBlock(std::move(b));
}
