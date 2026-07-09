#include "chat_frame.h"
#include "format_u8.h"
#include "inline_parser.h"
#include "tools.h"
#include "preferences.h"
#include "run_config_store.h"
#include "settings_dialog.h"
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/stattext.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/dirdlg.h>
#include <wx/bmpbndl.h>
#include <wx/settings.h>
#include <wx/stdpaths.h>
#include <algorithm>
#include <ctime>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <cstdlib>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#define chdir _chdir
#endif

// Posted from the tool-dispatch worker thread when a batch completes.
// Payload is a shared_ptr<vector<ToolBatchEntry>> — shared_ptr so wxThreadEvent's
// payload copy doesn't slice the move-only vector and the worker can hand off
// ownership cheaply.
wxDEFINE_EVENT(wxEVT_TOOL_BATCH_DONE, wxThreadEvent);

namespace {

constexpr int ID_SEND          = wxID_HIGHEST + 1;
constexpr int ID_INPUT         = wxID_HIGHEST + 2;
constexpr int ID_QUEUE_CONTINUE = wxID_HIGHEST + 3;
constexpr int ID_QUEUE_CLEAR    = wxID_HIGHEST + 4;
constexpr int ID_SESSION  = wxID_HIGHEST + 10;
constexpr int ID_MODEL    = wxID_HIGHEST + 11;
constexpr int ID_SETTINGS = wxID_HIGHEST + 12;
constexpr int ID_PLAY     = wxID_HIGHEST + 13;

// Per-model routing config. Resolved fresh at each StartCompletion so a model
// change during a tool-call loop applies on the next request.
struct ModelRoute {
    const char* url;
    const char* model;
    bool needsApiKey;
    Preferences::Provider provider;  // only valid when needsApiKey
    // Output ceiling. Set to each model's documented maximum — leaving it
    // unset would let deepseek apply its 4096 server-side default, which
    // clips `write_file` arguments mid-JSON and triggers an unrecoverable
    // "missing 'path' argument" loop. DeepSeek V4 docs publish 384K as the
    // hard max; providers clamp silently if a value exceeds the model's own
    // ceiling, so picking the documented top is safe.
    int maxTokens;
    // Total input+output token budget for the model. Used by the context
    // compactor to decide when to summarize the head of history. Set per
    // model from the published context window; conservative values are
    // fine — compaction triggers earlier rather than later.
    int contextWindow;
};

ModelRoute RouteFor(ModelChoice m) {
    switch (m) {
    case ModelChoice::OpencodeFree:
        // OpenCode Zen free tier. Models rotate — currently deepseek-v4-flash-free.
        // No API key needed; endpoint is open.
        return {"https://opencode.ai/zen/v1/chat/completions",
                "deepseek-v4-flash-free", false, Preferences::Provider::DeepSeek,
                384000, 200000};
    case ModelChoice::DeepseekFlash:
        return {"https://api.deepseek.com/chat/completions",
                "deepseek-v4-flash", true, Preferences::Provider::DeepSeek,
                384000, 1000000};
    case ModelChoice::DeepseekPro:
        return {"https://api.deepseek.com/chat/completions",
                "deepseek-v4-pro", true, Preferences::Provider::DeepSeek,
                384000, 1000000};
    }
    return {"https://opencode.ai/zen/v1/chat/completions",
            "deepseek-v4-flash-free", false, Preferences::Provider::DeepSeek,
            384000, 200000};
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

// Resolve the assets directory using wxStandardPaths — the standard
// wxWidgets way to find installed data files cross-platform.
// Dev builds fall back to the source tree path baked in at compile time.
const wxString& GetAssetsDir() {
    static wxString cached = []() -> wxString {
        wxString res = wxStandardPaths::Get().GetResourcesDir();
        if (wxFileName::DirExists(res + "/icons")) return res;
        // Dev fallback: source tree at compile time.
        return wxString(GRITCODE_ASSETS_DIR);
    }();
    return cached;
}

wxString GetCacertPath() {
    static wxString cached = []() -> wxString {
#ifdef _WIN32
        wxString exeDir = wxStandardPaths::Get().GetResourcesDir();
        if (wxFileName::FileExists(exeDir + "/cacert.pem"))
            return exeDir + "/cacert.pem";
        if (wxFileName::FileExists(exeDir + "/../cacert.pem"))
            return exeDir + "/../cacert.pem";
        return exeDir + "/cacert.pem";
#else
        return wxString();  // Linux/macOS use system cert store
#endif
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
    wxString hex = FormatU8("#{:02X}{:02X}{:02X}",
                            accent.Red(), accent.Green(), accent.Blue());
    svg.Replace("#FFFFFF", hex, true);
    svg.Replace("#ffffff", hex, true);
    auto utf8 = svg.utf8_string();
    return wxBitmapBundle::FromSVG(
        reinterpret_cast<const wxByte*>(utf8.data()), utf8.size(), size);
}

// Load the application icon. Tries the installed hicolor theme path first
// (used by DEB), then the source-tree packaging directory.
wxIconBundle LoadAppIcon() {
    wxString path = wxStandardPaths::Get().GetResourcesDir()
                    + "/../../icons/hicolor/scalable/apps/gritcode.svg";
    wxFileName fn(path);
    fn.Normalize();
    if (!fn.FileExists()) {
        // Dev fallback: source-tree packaging directory.
        fn.Assign(wxString(GRITCODE_ASSETS_DIR) + "/../packaging/gritcode.svg");
        fn.Normalize();
    }
    if (fn.FileExists()) {
        wxBitmapBundle bb = wxBitmapBundle::FromSVGFile(fn.GetFullPath(), wxSize(64, 64));
        if (bb.IsOk()) {
            wxIconBundle icons;
            icons.AddIcon(bb.GetIcon(wxSize(16, 16)));
            icons.AddIcon(bb.GetIcon(wxSize(32, 32)));
            icons.AddIcon(bb.GetIcon(wxSize(48, 48)));
            icons.AddIcon(bb.GetIcon(wxSize(64, 64)));
            return icons;
        }
    }
    return wxIconBundle();
}

}  // namespace

ChatFrame::ChatFrame()
    : wxFrame(nullptr, wxID_ANY, "gritcode",
              wxDefaultPosition, wxSize(600, 850)) {

    SetIcons(LoadAppIcon());

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

    wxBitmapBundle bbPlay = LoadThemedSvgIcon("play.svg", kIconSize, accent);
    playBtn_ = new wxBitmapButton(panel, ID_PLAY, bbPlay,
                                  wxDefaultPosition, kBtnSize,
                                  wxBORDER_NONE);
    playBtn_->SetToolTip(wxString::FromUTF8("Build and run project"));

    toolbarRow->Add(sessionLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    // Session and model both get a small proportion so they share resize delta;
    // the stretch spacer absorbs most of it. Once the spacer collapses (narrow
    // window) both dropdowns shrink toward their MinSize.
    toolbarRow->Add(sessionChoice_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    toolbarRow->Add(playBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    toolbarRow->Add(modelLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    toolbarRow->Add(modelChoice_, 1, wxALIGN_CENTER_VERTICAL);
    toolbarRow->AddStretchSpacer(8);
    toolbarRow->Add(settingsBtn_, 0, wxALIGN_CENTER_VERTICAL);

    // Chip row — wraps to multiple lines if the queue gets long. Hidden
    // (via sizer Show) until the queue has at least one entry.
    chipRow_ = new wxPanel(panel);
    chipSizer_ = new wxBoxSizer(wxVERTICAL);
    chipRow_->SetSizer(chipSizer_);
    chipRow_->Hide();

    auto* inputRow = new wxBoxSizer(wxHORIZONTAL);
    input_ = new wxTextCtrl(panel, ID_INPUT, "",
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    sendBtn_ = new wxButton(panel, ID_SEND, "Send");
    // Queue-mode buttons share the input row's slot; hidden until idle-queue.
    continueQueueBtn_ = new wxButton(panel, ID_QUEUE_CONTINUE, "Continue");
    clearQueueBtn_ = new wxButton(panel, ID_QUEUE_CLEAR, "Clear queue");
    continueQueueBtn_->Hide();
    clearQueueBtn_->Hide();

    inputRow->Add(input_, 1, wxEXPAND | wxTOP, 6);
    inputRow->Add(continueQueueBtn_, 1, wxEXPAND | wxRIGHT | wxTOP, 6);
    inputRow->Add(clearQueueBtn_, 0, wxALIGN_CENTER_VERTICAL | wxTOP, 6);
    inputRow->Add(sendBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 6);
    outer->Add(chipRow_, 0, wxEXPAND | wxTOP, 4);
    outer->Add(inputRow, 0, wxEXPAND);
    outer->Add(toolbarRow, 0, wxEXPAND | wxTOP, 4);

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(outer, 1, wxEXPAND | wxALL, FromDIP(2));
    panel->SetSizer(root);

    // Open the most recent session if one exists; otherwise seed one for the
    // default cwd so the dropdown always has at least one entry.
    store_.Init();
    // Open the FTS5 memory index. Failure (e.g. read-only home) is silent;
    // grit_history_search returns "Memory index unavailable" in that case.
    memory_.Open(MemoryDB::DefaultPath());
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
    Bind(wxEVT_BUTTON, &ChatFrame::OnContinueQueue, this, ID_QUEUE_CONTINUE);
    Bind(wxEVT_BUTTON, &ChatFrame::OnClearQueue, this, ID_QUEUE_CLEAR);
    input_->Bind(wxEVT_KEY_DOWN, &ChatFrame::OnInputKey, this);
    // CHAR_HOOK fires at the frame before any focused child sees the key, so
    // Escape cancels regardless of where focus is (input, canvas, dropdowns).
    Bind(wxEVT_CHAR_HOOK, &ChatFrame::OnCharHook, this);
    Bind(wxEVT_CLOSE_WINDOW, &ChatFrame::OnClose, this);
    Bind(wxEVT_BUTTON, &ChatFrame::OnSettings, this, ID_SETTINGS);
    Bind(wxEVT_BUTTON, &ChatFrame::OnPlay, this, ID_PLAY);
    Bind(wxEVT_TOOL_BATCH_DONE, &ChatFrame::OnToolBatchDone, this);
    sessionChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnSessionChoice, this);
    modelChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnModelChoice, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED,
         [this](wxSysColourChangedEvent& e) { ReloadToolbarIcons(); e.Skip(); });

    // Helper: bounce a value-returning closure onto the GUI thread and block
    // the calling (MCP) thread until it has returned. Polls `destroying_` so
    // the MCP thread can unblock during teardown: once ~ChatFrame sets the
    // flag the GUI thread stops pumping events, the CallAfter we just queued
    // will never fire, and a plain future.get() would hang mcp_.Stop()'s
    // join forever.
    auto guiSync = [this](auto fn) -> nlohmann::json {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        CallAfter([promise, fn = std::move(fn)]() mutable {
            try { promise->set_value(fn()); }
            catch (...) { promise->set_exception(std::current_exception()); }
        });
        using namespace std::chrono_literals;
        while (true) {
            if (destroying_.load()) return nlohmann::json::object();
            if (future.wait_for(100ms) == std::future_status::ready)
                return future.get();
        }
    };

    MCPCallbacks cb;
    cb.getStatus = [this, guiSync]() {
        return guiSync([this]() -> nlohmann::json {
            nlohmann::json queue = nlohmann::json::array();
            for (const auto& q : pendingQueue_) queue.push_back(q);
            return {
                {"streaming", streaming_},
                {"toolIter", toolIter_},
                {"historyLen", (int)history_.size()},
                {"pendingQueue", std::move(queue)},
                {"continueQueueVisible", continueQueueBtn_->IsShown()},
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
        // the message was accepted (or queued, or rejected as full).
        return guiSync([this, text]() -> nlohmann::json {
            if (streaming_) {
                if (pendingQueue_.size() >= kMaxQueue_) {
                    return {{"sent", false}, {"reason", "queue_full"}};
                }
                pendingQueue_.push_back(text.ToStdString(wxConvUTF8));
                UpdateQueueUI();
                return {{"sent", true}, {"queued", true},
                        {"queueLen", (int)pendingQueue_.size()}};
            }
            input_->SetValue(text);
            CallAfter([this]() {
                wxCommandEvent ev(wxEVT_BUTTON, ID_SEND);
                OnSend(ev);
            });
            return {{"sent", true}};
        });
    };
    cb.cancelRequest = [this]() {
        CallAfter([this]() { request_.Cancel(); });
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
    cb.hitTest = [this, guiSync](int x, int y) -> nlohmann::json {
        return guiSync([this, x, y]() -> nlohmann::json {
            BlockPos p = canvas_->HitTestPublic(x, y);
            return {{"block", p.block}, {"offset", p.offset},
                    {"valid", p.IsValid()}};
        });
    };
    cb.getSelection = [this, guiSync]() -> nlohmann::json {
        return guiSync([this]() -> nlohmann::json {
            BlockPos a, c;
            canvas_->GetSelection(a, c);
            wxString sel = canvas_->GetSelectedText();
            return {
                {"anchorBlock", a.block}, {"anchorOff", a.offset},
                {"caretBlock", c.block},  {"caretOff", c.offset},
                {"text", sel.ToStdString(wxConvUTF8)},
            };
        });
    };
    cb.setSelection = [this, guiSync](int ab, int ao, int cbi, int co)
        -> nlohmann::json {
        return guiSync([this, ab, ao, cbi, co]() -> nlohmann::json {
            BlockPos a{ab, ao};
            BlockPos c{cbi, co};
            canvas_->SetSelectionExplicit(a, c);
            return {{"ok", true}};
        });
    };
    cb.getGeometry = [this, guiSync]() -> nlohmann::json {
        return guiSync([this]() -> nlohmann::json {
            const auto& blocks = canvas_->Blocks();
            nlohmann::json arr = nlohmann::json::array();
            for (int i = 0; i < (int)blocks.size(); ++i) {
                int y, h;
                canvas_->GetBlockGeometry(i, y, h);
                std::string typ = "other";
                switch (blocks[i].type) {
                    case BlockType::Paragraph:  typ = "paragraph"; break;
                    case BlockType::Heading:    typ = "heading";   break;
                    case BlockType::CodeBlock:  typ = "code";      break;
                    case BlockType::UserPrompt: typ = "user";      break;
                    case BlockType::Table:      typ = "table";     break;
                    case BlockType::ToolCall:   typ = "tool";      break;
                    case BlockType::Thinking:   typ = "thinking";  break;
                }
                arr.push_back({
                    {"index", i}, {"yTop", y}, {"height", h}, {"type", typ},
                    {"toolExpanded", (blocks[i].type == BlockType::ToolCall
                                      || blocks[i].type == BlockType::Thinking)
                                     ? blocks[i].toolExpanded : false},
                    {"visibleLen", (int)blocks[i].visibleText.size()},
                });
            }
            return {{"blocks", std::move(arr)}};
        });
    };
    cb.simulateDrag = [this, guiSync](int x1, int y1, int x2, int y2,
                                      int steps) -> nlohmann::json {
        return guiSync([this, x1, y1, x2, y2, steps]() -> nlohmann::json {
            nlohmann::json trace = nlohmann::json::array();
            int n = std::max(1, steps);
            // Anchor is set by HitTest alone (no through-drag snap on
            // mouse-down); subsequent caret positions go through the same
            // resolver OnMotion uses, including the snap.
            BlockPos anchor = canvas_->HitTestPublic(x1, y1);
            BlockPos caret = anchor;
            canvas_->SetSelectionExplicit(anchor, caret);
            trace.push_back({{"step", 0}, {"x", x1}, {"y", y1},
                             {"block", caret.block}, {"offset", caret.offset}});
            for (int s = 1; s <= n; ++s) {
                int x = x1 + (x2 - x1) * s / n;
                int y = y1 + (y2 - y1) * s / n;
                caret = canvas_->ResolveDragCaret(x, y, anchor);
                canvas_->SetSelectionExplicit(anchor, caret);
                trace.push_back({{"step", s}, {"x", x}, {"y", y},
                                 {"block", caret.block},
                                 {"offset", caret.offset}});
            }
            wxString sel = canvas_->GetSelectedText();
            return {{"trace", std::move(trace)},
                    {"selectedText", sel.ToStdString(wxConvUTF8)}};
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
    // Signal MCP's guiSync callers to bail out of their future.get() polls
    // before we stop the server — otherwise mcp_.Stop()'s join would deadlock
    // on a thread waiting for a CallAfter that can no longer fire.
    destroying_.store(true);
    mcp_.Stop();
    request_.Cancel();
    // ~StreamingWebRequest joins the worker thread, so by the time we return
    // no more callbacks can be posted.

    // Tool worker isn't detached — cancel any in-flight bash subtree and join
    // so the worker can't fire wxQueueEvent on a half-destroyed frame.
    if (toolWorker_.joinable()) {
        if (auto tok = currentToolToken_) {
            tok->cancelled.store(true);
#ifndef _WIN32
            int pgid = tok->activePgid.load();
            if (pgid > 0) ::kill(-pgid, SIGTERM);
#endif
        }
        toolWorker_.join();
    }
    if (playWorker_.joinable()) {
        if (auto tok = currentPlayToken_) {
            tok->cancelled.store(true);
#ifndef _WIN32
            int pgid = tok->activePgid.load();
            if (pgid > 0) ::kill(-pgid, SIGTERM);
#endif
        }
        playWorker_.join();
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
        case BlockType::Thinking:   return "thinking";
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
        } else if (b.type == BlockType::Thinking) {
            e["expanded"] = b.toolExpanded;
            e["singleLine"] = b.thinkingSingleLine;
            e["chars"] = (int)b.rawText.Length();
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
    // currentToolToken_ is non-null exactly while the tool-dispatch worker is
    // running. Closing during a tool batch must veto + cancel just like
    // closing during an in-flight HTTP request — otherwise the worker would
    // outlive the frame and post wxQueueEvent to a dangling `this`.
    if (request_.IsActive() || currentToolToken_) {
        quitRequested_ = true;
        Hide();
        RequestCancel();
        // OnStreamDone / OnToolBatchDone both check quitRequested_ and re-fire
        // Close() once their phase finishes.
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

void ChatFrame::OnCharHook(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE && streaming_) {
        RequestCancel();
        return;
    }
    e.Skip();
}

void ChatFrame::RequestCancel() {
    // Idempotent on a finished request — Cancel just sets an atomic.
    request_.Cancel();
    // Signal the tool worker. Setting `cancelled` makes the worker bail out
    // between tools and the bash poll loop bail mid-tool. Sending SIGTERM to
    // the active pgid kills the bash subtree without waiting for the worker
    // to notice — important when bash is blocked on something slow.
    if (auto tok = currentToolToken_) {
        tok->cancelled.store(true);
#ifndef _WIN32
        int pgid = tok->activePgid.load();
        if (pgid > 0) ::kill(-pgid, SIGTERM);
#endif
    }
    // Same for the Play-button worker (e.g. a running dev server).
    if (auto tok = currentPlayToken_) {
        tok->cancelled.store(true);
#ifndef _WIN32
        int pgid = tok->activePgid.load();
        if (pgid > 0) ::kill(-pgid, SIGTERM);
#endif
    }
}

void ChatFrame::ReloadToolbarIcons() {
    const wxSize kIconSize(20, 20);
    wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    settingsBtn_->SetBitmap(LoadThemedSvgIcon("settings.svg", kIconSize, accent));
    playBtn_->SetBitmap(LoadThemedSvgIcon("play.svg", kIconSize, accent));
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
                         "gritcode", wxOK | wxICON_INFORMATION, this);
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
                     "gritcode", wxOK | wxICON_INFORMATION, this);
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
    historyCompactBaseCount_ = 0;  // fresh compaction gate for the new session
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
    historyCompactBaseCount_ = 0;  // fresh compaction gate for the new session
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
    // Re-index into the FTS5 memory so grit_history_search picks up the
    // turns we just persisted. RebuildSession is idempotent: it deletes
    // every prior row for this session_id and re-inserts the current
    // history. Cheap in absolute terms (a 500-turn rebuild is sub-100ms).
    if (memory_.IsOpen()) {
        auto now = []{
            std::time_t t = std::time(nullptr); std::tm tm;
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            return std::string(buf);
        }();
        nlohmann::json msgs = history_;  // shallow copy → array of json messages
        memory_.RebuildSession(SessionStore::IdForCwd(activeCwd_),
                               activeCwd_, msgs, now);
    }
}

void ChatFrame::SeedSystemPrompt() {
    std::string platformInfo;
#ifdef _WIN32
    platformInfo = "You are running on Windows. The shell tool uses cmd /c — "
                   "use Windows commands (dir, type, findstr, del, etc.). "
                   "Path separators are backslashes.";
#elif defined(__APPLE__)
    platformInfo = "You are running on macOS. The shell tool uses bash. "
                   "Use Unix commands. Path separators are forward slashes.";
#else
    platformInfo = "You are running on Linux. The shell tool uses bash. "
                   "Use Unix commands. Path separators are forward slashes.";
#endif

    history_.push_back({
        {"role", "system"},
        {"content",
         "You are a helpful AI coding assistant with access to local file, "
         "shell, and web tools. Use them when the user's request requires "
         "reading files, running shell commands, or fetching web pages. "
         "Prefer concrete actions over speculation. Use markdown for "
         "formatting and fenced code blocks for code.\n\n"
         + platformInfo + "\n\n"
         "Play button: the ▶ button runs a single stored shell command "
         "directly from the project root — it does NOT invoke the AI. "
         "When clicked with no stored command, you'll be asked to "
         "configure it. Test your command via bash first, then store "
         "it with run_project set.\n\n"
         "Cross-project memory: use grit_history_search whenever the user "
         "references any prior work (\"last time\", \"once again\", \"we "
         "had\", \"how did we\", \"in <project>\"). It searches the "
         "transcripts of every past gritcode session across all "
         "projects and returns short snippets with session_id + "
         "turn_index. Follow up with grit_history_fetch(session_id, "
         "turn_index) to read full turns. Start with ONE broad keyword "
         "query — if it returns no relevant hits, stop and answer from "
         "what you have rather than firing speculative variants."}
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
            // 0. Reasoning is rendered before content/tool blocks so the
            // visual order matches the streamed-turn order.
            if (m.contains("reasoning_content") && m["reasoning_content"].is_string()) {
                std::string reasoning = m["reasoning_content"].get<std::string>();
                if (!reasoning.empty()) {
                    RenderThinkingBlock(wxString::FromUTF8(reasoning));
                }
            }
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


void ChatFrame::OnPlay(wxCommandEvent&) {
    if (streaming_) return;
    auto cfg = RunConfigStore::Get(activeCwd_);
    if (cfg) {
        // Direct execution path — no model inference, just run the stored
        // command. Display-only: the user prompt block and tool result go
        // on the canvas but are NOT appended to history_. The Play button
        // is a local UX affordance; injecting fake tool_call/tool_result
        // messages into the API conversation breaks subsequent requests
        // when the command is long-running (e.g. a dev server) — the API
        // rejects the dangling tool_calls with a 400.
        wxString userMsg = wxString::FromUTF8("▶ Build and run:\n  ") + wxString::FromUTF8(cfg->command);
        Block userBlock;
        userBlock.type = BlockType::UserPrompt;
        userBlock.rawText = userMsg;
        userBlock.visibleText = userMsg;
        userBlock.runs.push_back({userMsg, false, false});
        canvas_->AddBlock(std::move(userBlock));

        // Run the command on a background thread. Use a dedicated playWorker_
        // so a long-running dev server doesn't block the tool dispatch worker
        // — otherwise the model can't execute tool calls while the server runs.
        auto token = std::make_shared<ToolCancelToken>();
        currentPlayToken_ = token;
        if (playWorker_.joinable()) playWorker_.join();
        playWorker_ = std::thread([this, cmd = cfg->command, token, cwd = activeCwd_]() {
            // Wrap the stored command with a cd to the project directory so
            // relative paths work regardless of the process's current cwd.
            // This is the key fix for the Play button: the model stores
            // commands like "cmake --build build && ./build/gritcode" which
            // only work from the project root. The wrapping cd ensures the
            // shell is in the right place before executing.
            // POSIX-safe single-quote escaping for the cwd path.
            std::string qcwd;
            qcwd += '\'';
            for (char ch : cwd) {
                if (ch == '\'') qcwd += "'\\''";
                else qcwd += ch;
            }
            qcwd += '\'';
            std::string wrapped = "cd " + qcwd + " && " + cmd;
            std::string result = ToolBashDirect(wrapped.c_str(), token.get());
            CallAfter([this, result, cmd]() {
                if (destroying_.load()) return;
                currentPlayToken_.reset();
                // Canvas-only display — no history_ modification.
                RenderToolBlock("bash", cmd, result);
            });
        });
    } else {
        // No command stored yet — ask the model to discover the project and
        // configure both a build and run step. The visible prompt is short;
        // hidden instructions tell the model how to handle servers and how
        // to make subsequent Play clicks restart rather than fork.
        wxString visible = wxString::FromUTF8(
            "Configure the Play button for this project.\n\n"
            "1. Examine the project structure and identify the build/entry "
            "point.\n"
            "2. Construct a SINGLE self-contained command that builds (if "
            "compiled) AND runs the project from the root directory.\n"
            "3. Test it with bash — debug until it succeeds.\n"
            "4. Use run_project set to store the working command.\n\n"
            "The Play button runs exactly the stored command every time.");
        std::string hidden =
            "\n\n"
            "[hidden]"
            "When configuring the Play button for server / web-dev projects "
            "(Hugo, Django, Next.js, Flask, Express, Rails, etc.), the "
            "stored command must:"
            "\n- Kill any previous instance of the server before starting a "
            "new one, so repeated Play clicks restart rather than fork. Use "
            "pkill or kill $(lsof -t -i:<port>) for a clean restart."
            "\n- After starting the server, open the default browser to the "
            "appropriate URL (e.g. xdg-open http://localhost:1313 for Hugo, "
            "xdg-open http://localhost:8000 for Django)."
            "\n\nFor static sites (Hugo, Jekyll, etc.), the command should "
            "build AND start the dev server + open the browser, all in one "
            "shot: 'hugo serve && xdg-open http://localhost:1313' is NOT "
            "correct because hugo serve blocks. Instead use "
            "'hugo serve --noBrowser & sleep 1 && xdg-open http://localhost:1313 && wait' "
            "or a similar pattern."
            "\n\nFor compiled projects, just build and run — no browser needed."
            "[/hidden]";

        // Show only the visible prompt on the canvas.
        Block ub;
        ub.type = BlockType::UserPrompt;
        ub.rawText = visible;
        InlineRun r;
        r.text = visible;
        ub.runs.push_back(r);
        ub.visibleText = visible;
        canvas_->AddBlock(std::move(ub));

        // The API sees visible + hidden; the canvas shows only visible.
        history_.push_back({{"role", "user"},
                            {"content", visible.ToStdString(wxConvUTF8) + hidden}});

        streaming_ = true;
        canvas_->SetThinking(true);
        UpdateQueueUI();
        toolIter_ = 0;
        StartCompletion();
    }
}
void ChatFrame::OnSettings(wxCommandEvent&) {
    SettingsDialog dlg(this);
    dlg.ShowModal();
}

void ChatFrame::OnSend(wxCommandEvent&) {
    wxString text = input_->GetValue();
    text.Trim().Trim(false);
    if (text.IsEmpty()) return;

    // Agent busy: append to the queue (the Send button is "Add" while busy).
    // Reaching kMaxQueue_ disables the button — defensive check for a
    // synthesized event from MCP / Enter key.
    if (streaming_) {
        if (pendingQueue_.size() >= kMaxQueue_) return;
        pendingQueue_.push_back(text.ToStdString(wxConvUTF8));
        input_->Clear();
        UpdateQueueUI();
        return;
    }

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
    UpdateQueueUI();  // Send becomes "Add", chip row stays visible if non-empty.
    toolIter_ = 0;

    StartCompletion();
}

void ChatFrame::StartCompletion() {
    // Compaction preflight. If the rendered request would overflow the
    // model's context window, we summarize the head of history first via
    // a separate streaming request; the summary completion path will
    // call DoSendActualRequest itself once history has been rewritten.
    // Skipped while compacting_ is true so a chained ApplyCompaction
    // doesn't re-evaluate the same condition.
    if (!compacting_ && MaybeCompactThenSend()) return;
    DoSendActualRequest();
}

void ChatFrame::DoSendActualRequest() {
    // Reset per-completion stream state.
    activeAssistantText_.clear();
    activeReasoning_.clear();
    activeToolCalls_.clear();
    thinkingEmitted_ = false;
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
            FinalizeTurn(true);
            return;
        }
    }

    nlohmann::json req;
    req["model"] = route.model;
    req["stream"] = true;
    req["max_tokens"] = route.maxTokens;
    req["messages"] = std::move(messages);
    req["tools"] = GetToolDefinitions();

    // error_handler_t::replace silently swaps invalid UTF-8 bytes in any
    // history string (bash output, file contents, model glitches, user paste)
    // for U+FFFD instead of throwing type_error.316.
    std::string body = req.dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace);

    WebRequestSpec spec;
    spec.url = route.url;
    spec.method = "POST";
    spec.body = std::move(body);
    spec.bodyContentType = "application/json";
    spec.headers.push_back({"Accept", "text/event-stream"});
    if (route.needsApiKey) {
        spec.headers.push_back({"Authorization",
                                "Bearer " + std::string(apiKey.utf8_string())});
    }
    // Without an idle watchdog a half-closed SSE stream (server FIN with no
    // [DONE]) would leave the request hanging forever. 60 s is generous enough
    // to ride out a slow first token but tight enough to surface a stuck
    // connection in a recoverable amount of time.
    spec.idleTimeoutSeconds = 60;

    request_ = StreamingWebRequest(
        this, std::move(spec),
        [this](std::string_view chunk) { OnStreamData(chunk); },
        [this](WebResponse resp) { OnStreamDone(std::move(resp)); });
}

void ChatFrame::OnStreamData(std::string_view chunk) {
    if (chunk.empty()) return;
    sseBuf_.append(chunk.data(), chunk.size());

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
                        // First non-reasoning byte of this round: emit the
                        // thinking block now so it lands before any content
                        // block the markdown stream is about to push out.
                        EmitPendingThinking();
                        activeAssistantText_ += content;
                        if (mdStream_) mdStream_->Feed(wxString::FromUTF8(content));
                    }
                }

                // ---- tool_calls ----
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()
                    && !delta["tool_calls"].empty()) {
                    // Same as the content case — render thinking before any
                    // tool block lands. RenderToolBlock fires later in
                    // HandleCompletion, but emitting here keeps the order
                    // consistent regardless of when the model interleaves
                    // tool_calls and content.
                    EmitPendingThinking();
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

void ChatFrame::OnStreamDone(WebResponse resp) {
    // The user closed the window while a stream was in flight. The Cancel
    // already short-circuited the worker; just complete the close now.
    if (quitRequested_) {
        Close();
        return;
    }

    // Flush markdown parser before deciding what to do next — any pending
    // paragraph/code block needs to land before tool blocks or the next turn.
    if (mdStream_) mdStream_->Flush();
    mdStream_.reset();

    if (!resp.ok && resp.error == "cancelled") {
        if (!streaming_) return;
        RenderErrorBlock("Cancelled.");
        FinalizeTurn(true);
        return;
    }

    if (resp.status == 401) {
        wxString detail = FormatU8(
            "Authentication failed (HTTP {}). Check your API key in Settings.",
            resp.status);
        wxString body = ExtractErrorBody();
        if (!body.IsEmpty() && body.length() < 400) {
            detail += "\n\n" + body;
        }
        HandleCompletion(detail);
        return;
    }

    if (!resp.ok) {
        wxString detail;
        if (resp.status > 0) {
            detail = FormatU8("Error: HTTP {}", resp.status);
        } else {
            detail = "Error: " + wxString::FromUTF8(resp.error);
        }
        wxString body = ExtractErrorBody();
        if (!body.IsEmpty()) detail += "\n\n" + body;
        HandleCompletion(detail);
        return;
    }

    HandleCompletion(wxString());
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
    // Pure-reasoning response (model returned reasoning_content but no content
    // or tool_calls): emit the thinking block here so the user sees what the
    // model produced. No-op if a prior content/tool delta already emitted it.
    EmitPendingThinking();

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
        FinalizeTurn(true);
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

    // Dispatch each tool on a worker thread so blocking I/O (bash popen,
    // file reads, web fetches) never freezes the UI. Pre-parse arguments on
    // the GUI thread — cheap, and lets us avoid touching nlohmann from the
    // worker except for what DispatchTool already does internally.
    struct ToolJob {
        std::string id;
        std::string name;
        std::string argsJson;
        nlohmann::json argsParsed;
        std::string parseError;  // non-empty: skip dispatch, surface this as the result
    };
    auto jobs = std::make_shared<std::vector<ToolJob>>();
    jobs->reserve(activeToolCalls_.size());
    for (const auto& tc : activeToolCalls_) {
        ToolJob job;
        job.id = tc.id;
        job.name = tc.name;
        job.argsJson = tc.args;
        if (tc.args.empty()) {
            // Model emitted the call with no arguments at all. Don't dispatch;
            // surface a clear error so the model retries with a populated
            // payload instead of getting "missing X" from each individual tool.
            job.argsParsed = nlohmann::json::object();
            job.parseError = "Error: tool call had no arguments. Re-emit the "
                             "call with the required parameters.";
        } else {
            try {
                job.argsParsed = nlohmann::json::parse(tc.args);
            } catch (const std::exception& e) {
                // Most common cause: the response was clipped by max_tokens
                // mid-string, leaving an unterminated JSON. Tell the model
                // exactly that — silently substituting `{}` triggers a
                // "missing argument" loop that the model can't break out of.
                job.argsParsed = nlohmann::json::object();
                job.parseError =
                    std::string("Error: tool arguments were not valid JSON (") +
                    e.what() + "). The arguments may have been truncated. " +
                    "Retry, splitting large content into smaller calls if needed.";
            }
        }
        jobs->push_back(std::move(job));
    }

    auto token = std::make_shared<ToolCancelToken>();
    currentToolToken_ = token;

    // Snapshot the session id so the worker can ask Memory to exclude this
    // session from grit_history_search results — there's no point recalling
    // the conversation the agent is already in.
    std::string excludeSid = activeCwd_.empty() ? std::string{}
                              : SessionStore::IdForCwd(activeCwd_);

    // OnToolBatchDone only runs after the previous worker exits, so by the
    // time we dispatch a new batch the prior thread is finished — join() is
    // a non-blocking handoff that lets us reuse the std::thread slot without
    // detaching (detach makes destructor cleanup impossible).
    if (toolWorker_.joinable()) toolWorker_.join();
    toolWorker_ = std::thread([this, jobs, token, excludeSid, cwd = activeCwd_]() {
        auto results = std::make_shared<std::vector<ToolBatchEntry>>();
        results->reserve(jobs->size());
        for (auto& job : *jobs) {
            std::string r;
            if (token->cancelled.load()) {
                r = "[cancelled]";
            } else if (!job.parseError.empty()) {
                r = std::move(job.parseError);
            } else {
                r = DispatchTool(job.name, job.argsParsed, token.get(),
                                 &memory_, excludeSid, cwd);
            }
            results->push_back({std::move(job.id), std::move(job.name),
                                std::move(job.argsJson), std::move(r)});
        }
        auto* ev = new wxThreadEvent(wxEVT_TOOL_BATCH_DONE);
        ev->SetPayload(results);
        wxQueueEvent(this, ev);
    });
}

void ChatFrame::OnToolBatchDone(wxThreadEvent& e) {
    auto results = e.GetPayload<std::shared_ptr<std::vector<ToolBatchEntry>>>();
    bool wasCancelled = currentToolToken_ && currentToolToken_->cancelled.load();
    currentToolToken_.reset();

    // User closed the window mid-batch — OnClose vetoed and is waiting for us
    // to complete this phase. Re-fire Close so the second pass through OnClose
    // sees an idle frame and lets the destruction proceed.
    if (quitRequested_) {
        Close();
        return;
    }

    if (!results) return;

    for (auto& r : *results) {
        RenderToolBlock(r.name, r.argsJson, r.result);
        history_.push_back({
            {"role", "tool"},
            {"tool_call_id", r.id},
            {"name", r.name},
            {"content", r.result},
        });
    }

    if (wasCancelled) {
        // User hit Escape during tool execution. The assistant's tool_calls
        // message and any tool results (real or "[cancelled]") are already in
        // history, so the next user turn replays cleanly. Stop the loop here
        // and surface a marker block.
        RenderErrorBlock("Cancelled.");
        FinalizeTurn(true);
        return;
    }

    toolIter_++;

    // Continue the conversation with another completion request.
    StartCompletion();
}

void ChatFrame::FinalizeTurn(bool wasCancelledOrError) {
    canvas_->SetThinking(false);
    streaming_ = false;
    activeAssistantText_.clear();
    activeToolCalls_.clear();
    PersistActive();
    // Title may have changed (first user message defines it) — refresh the
    // dropdown so the new label shows up.
    RefreshSessionChoice();

    // Natural completion: auto-dispatch the next queued message so back-to-
    // back turns flow without a click. DispatchNextQueued -> StartTurn flips
    // streaming_ back on so UpdateQueueUI never observes the (idle, queue
    // non-empty) intermediate state and the input stays visible the whole
    // time. Error/cancel: drop into idle-queue mode (Continue / Clear) if
    // the queue is non-empty, so the user decides what to do.
    if (!wasCancelledOrError && !pendingQueue_.empty()) {
        DispatchNextQueued();
        return;
    }
    UpdateQueueUI();
    if (input_->IsShown()) input_->SetFocus();
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
    // visibleText layout:
    //   [0, headerLen)            "toolName(args)"
    //   [headerLen, headerLen+2)  "\n\n" separator
    //   [headerLen+2, end)        toolResult
    // Header chars are selectable in the rendered header strip; body chars
    // are selectable only when expanded. Selecting across a collapsed block
    // (drag from another block, through this one) still grabs the full
    // visibleText, so Ctrl+C captures both the header and the hidden body.
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

void ChatFrame::RenderThinkingBlock(const wxString& text) {
    if (text.IsEmpty()) return;
    Block b;
    b.type = BlockType::Thinking;
    b.rawText = text;
    b.visibleText = text;
    b.toolExpanded = false;  // collapsed by default per spec
    canvas_->AddBlock(std::move(b));
}

void ChatFrame::EmitPendingThinking() {
    if (thinkingEmitted_) return;
    thinkingEmitted_ = true;
    if (activeReasoning_.empty()) return;
    RenderThinkingBlock(wxString::FromUTF8(activeReasoning_));
}

void ChatFrame::DispatchNextQueued() {
    if (pendingQueue_.empty()) return;
    std::string next = std::move(pendingQueue_.front());
    pendingQueue_.erase(pendingQueue_.begin());
    StartTurn(wxString::FromUTF8(next));
}

void ChatFrame::OnContinueQueue(wxCommandEvent&) {
    DispatchNextQueued();
}

void ChatFrame::OnClearQueue(wxCommandEvent&) {
    pendingQueue_.clear();
    UpdateQueueUI();
    if (input_->IsShown()) input_->SetFocus();
}

void ChatFrame::UpdateQueueUI() {
    const bool busy = streaming_;
    const bool hasQueue = !pendingQueue_.empty();
    const bool idleQueueMode = !busy && hasQueue;

    // Normal vs idle-queue: input + Send swap with Continue + Clear.
    input_->Show(!idleQueueMode);
    sendBtn_->Show(!idleQueueMode);
    continueQueueBtn_->Show(idleQueueMode);
    clearQueueBtn_->Show(idleQueueMode);

    if (busy) {
        sendBtn_->SetLabel("Add");
        sendBtn_->Enable(pendingQueue_.size() < kMaxQueue_);
    } else {
        sendBtn_->SetLabel("Send");
        sendBtn_->Enable(true);
    }
    if (idleQueueMode) {
        continueQueueBtn_->SetLabel(
            FormatU8("Continue ({} queued)", pendingQueue_.size()));
    }

    chipRow_->Show(hasQueue);
    if (hasQueue) RebuildChips();
    if (auto* parent = chipRow_->GetParent()) parent->Layout();
}

void ChatFrame::RebuildChips() {
    chipSizer_->Clear(true);

    // Pull system colors so the chips look at home in both light and dark
    // themes. The chip body is a couple of shades off the window background;
    // the close badge is a bit further off the chip body so it stands out.
    wxColour winBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const bool dark = winBg.Red() + winBg.Green() + winBg.Blue() < 384;
    auto shift = [dark](wxColour c, int delta) {
        int d = dark ? delta : -delta;
        return wxColour(std::clamp(c.Red() + d, 0, 255),
                        std::clamp(c.Green() + d, 0, 255),
                        std::clamp(c.Blue() + d, 0, 255));
    };
    wxColour chipBg = shift(winBg, 22);

    constexpr size_t kMaxChars = 60;
    for (size_t i = 0; i < pendingQueue_.size(); ++i) {
        wxString full = wxString::FromUTF8(pendingQueue_[i]);
        wxString label = full;
        label.Replace("\n", " ");
        label.Replace("\r", " ");
        label.Replace("\t", " ");
        while (label.Replace("  ", " ")) {}
        label.Trim().Trim(false);
        if (label.length() > kMaxChars)
            label = label.Left(kMaxChars - 1) + wxString::FromUTF8("\xE2\x80\xA6");
        if (label.IsEmpty()) label = "(empty)";

        auto* chip = new wxPanel(chipRow_, wxID_ANY);
        chip->SetBackgroundColour(chipBg);
        chip->SetForegroundColour(fg);
        chip->SetToolTip(full);

        auto* inner = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(chip, wxID_ANY, label);
        lbl->SetForegroundColour(fg);
        // Use a wxStaticText for the close badge so there's no native
        // button frame around it. Hit-tested via wxEVT_LEFT_DOWN below.
        auto* close = new wxStaticText(chip, wxID_ANY, wxString::FromUTF8("\xC3\x97"));
        close->SetForegroundColour(fg);
        close->SetCursor(wxCURSOR_HAND);
        close->SetToolTip("Remove from queue");

        inner->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(6));
        inner->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
        chip->SetSizer(inner);

        // Mutation deferred via CallAfter: UpdateQueueUI -> RebuildChips
        // deletes the very chip whose event handler we're inside, and
        // touching it post-Destroy crashes GTK. Match by stored content
        // instead of capturing index `i` — two rapid clicks on different
        // chips would otherwise resolve their indices against a queue that
        // has already shifted under the first CallAfter, and a stale `i`
        // would erase the wrong (or, at end-of-queue, a no-longer-valid)
        // entry.
        std::string entry = pendingQueue_[i];
        close->Bind(wxEVT_LEFT_DOWN, [this, entry](wxMouseEvent&) {
            CallAfter([this, entry]() {
                auto it = std::find(pendingQueue_.begin(),
                                    pendingQueue_.end(), entry);
                if (it == pendingQueue_.end()) return;
                pendingQueue_.erase(it);
                UpdateQueueUI();
                if (!streaming_ && pendingQueue_.empty() && input_->IsShown())
                    input_->SetFocus();
            });
        });

        chipSizer_->Add(chip, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
    }
    chipRow_->Layout();
}

// ---------------------------------------------------------------------------
// Context compaction
//
// Goal: when the rendered history would overflow the model's context window,
// replace the oldest portion with a real LLM-generated summary — the same
// strategy opencode uses. Persisting the summary in history_ (rather than
// only rewriting the wire request) keeps every subsequent turn within
// budget; without that, history would keep growing and we'd re-"compact"
// the same head on every send.
//
// Flow:
//   1. StartCompletion → MaybeCompactThenSend. If history fits the budget,
//      return false → caller proceeds to DoSendActualRequest.
//   2. Over budget: pick the most recent user message as the split point.
//      Head = history_[0..split), tail = history_[split..] stays intact
//      (so we never break tool_call / tool_result pairs).
//   3. Fire an async summary request on `request_` with the summary
//      callbacks (OnSummaryStreamData / OnSummaryStreamDone). The UI
//      shows a notice so the extra round-trip doesn't look like a hang.
//   4. ApplyCompaction (on the GUI thread, after the summary returns)
//      replaces the head with a single isSummary user-role message,
//      persists, and calls DoSendActualRequest to fire the real request.
//   5. historyCompactBaseCount_ is bumped to the post-compaction size so
//      MaybeCompactThenSend won't re-compact until enough new messages
//      have accumulated.
// ---------------------------------------------------------------------------

bool ChatFrame::MaybeCompactThenSend() {
    int histSize = (int)history_.size();

    // Hysteresis: require some growth since last compaction before retrying.
    // Without this, every subsequent over-budget request would fire its own
    // summary call even though history hasn't meaningfully grown.
    int growth = histSize - historyCompactBaseCount_;
    if (growth < 5 && historyCompactBaseCount_ > 0) return false;

    // ~4 chars per token. Walk history summing content + reasoning + tool
    // call argument strings; this is the same heuristic the request builder
    // ends up implicitly using when serializing.
    size_t totalChars = 0;
    for (const auto& m : history_) {
        if (!m.is_object()) continue;
        if (m.contains("content") && m["content"].is_string())
            totalChars += m["content"].get_ref<const std::string&>().size();
        if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
            totalChars += m["reasoning_content"].get_ref<const std::string&>().size();
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (tc.is_object() && tc.contains("function")
                    && tc["function"].is_object()
                    && tc["function"].contains("arguments")
                    && tc["function"]["arguments"].is_string()) {
                    totalChars += tc["function"]["arguments"]
                                    .get_ref<const std::string&>().size();
                }
            }
        }
    }
    // Fixed overhead for the system prompt + tool definitions (~8K tokens
    // worth: enough to cover both the seeded prompt and the JSON tool defs).
    totalChars += 32000;
    int estTokens = (int)(totalChars / 4);

    ModelRoute route = RouteFor(currentModel_);
    // Reserve room for the response. route.maxTokens is the *upper-bound*
    // per-request cap (e.g. 384K for DeepSeek) — larger than the entire
    // context window for some models — so it can't be used as a reservation.
    // 32K is a realistic ceiling for what any single turn actually produces;
    // anything bigger gets server-clipped anyway.
    constexpr int kResponseReserve = 32000;
    int threshold = route.contextWindow - kResponseReserve - 8000;
    if (threshold < 8000) threshold = 8000;

    if (estTokens <= threshold) return false;

    // Find the most recent non-summary user message — that's where the
    // current turn starts, and everything before it is fair game to summarize.
    // Splitting at a user message never leaves an orphan tool_call in the
    // head or an orphan tool_result in the tail.
    int splitIdx = -1;
    for (int i = histSize - 1; i >= 0; --i) {
        if (!history_[i].is_object()) continue;
        if (history_[i].value("role", std::string{}) != "user") continue;
        if (history_[i].value("isSummary", false)) continue;
        splitIdx = i;
        break;
    }
    // Nothing older than the current turn to summarize — give up gracefully.
    if (splitIdx <= 0) return false;

    RunSummaryThenSend(splitIdx);
    return true;
}

void ChatFrame::RunSummaryThenSend(int splitIdx) {
    compacting_ = true;
    compactionSplitIdx_ = splitIdx;
    compactionHeadCount_ = splitIdx;
    summarySseBuf_.clear();
    summaryText_.clear();

    RenderErrorBlock(FormatU8(
        "📦 Compacting context — summarizing {} older messages before "
        "continuing. This usually takes a few seconds…",
        compactionHeadCount_));

    // Render the head as plain text. We feed the summary model a flat
    // transcript rather than the raw tool_calls / tool_result structure —
    // (a) it works identically for any wire protocol, and (b) the summary
    // model doesn't need machine-readable tool shape, just what happened.
    std::string headText;
    headText.reserve(16384);
    for (int i = 0; i < splitIdx; ++i) {
        const auto& m = history_[i];
        if (!m.is_object()) continue;
        std::string role = m.value("role", std::string{});
        if (role == "system") continue;  // omit our own seed prompt
        headText += "--- ";
        if (m.value("isSummary", false)) headText += "earlier summary";
        else headText += role;
        headText += " ---\n";
        if (m.contains("content") && m["content"].is_string()) {
            const auto& c = m["content"].get_ref<const std::string&>();
            if (!c.empty()) { headText += c; headText += '\n'; }
        }
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function")) continue;
                const auto& fn = tc["function"];
                headText += "[tool ";
                if (fn.contains("name") && fn["name"].is_string())
                    headText += fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string()) {
                    headText += ' ';
                    headText += fn["arguments"].get<std::string>();
                }
                headText += "]\n";
            }
        }
        headText += '\n';
    }

    // Cap the summary-call input so the summary request itself doesn't
    // overflow. Budget = context window − response budget − prompt overhead.
    ModelRoute route = RouteFor(currentModel_);
    size_t maxChars = (size_t)(route.contextWindow - 6000) * 4;
    if (maxChars < 40000) maxChars = 40000;
    if (headText.size() > maxChars) {
        size_t drop = headText.size() - maxChars;
        headText = "[... much older history truncated to fit this "
                   "summarization call ...]\n\n" + headText.substr(drop);
    }

    const std::string summarySystem =
        "You are helping to compact a long coding-assistant conversation "
        "so it fits within the model's context window for future turns. "
        "Produce a detailed, faithful summary that preserves:\n"
        "  - The user's overall goal(s) and any sub-tasks.\n"
        "  - Concrete decisions made and their rationale.\n"
        "  - Files touched, functions edited, and the substance of each change.\n"
        "  - Results of commands run (build pass/fail, test outcomes, error messages).\n"
        "  - Open questions, blockers, and what should happen next.\n"
        "  - Any user preferences, constraints, or corrections given.\n"
        "Write a compact past-tense narrative. Do not invent details, do "
        "not add a sign-off, do not ask questions. Output only the summary.";

    std::string summaryUser =
        "Summarize this conversation so a fresh session can continue the "
        "work without re-reading it:\n\n" + headText;

    nlohmann::json req;
    req["model"] = route.model;
    req["stream"] = true;
    req["max_tokens"] = 2500;
    req["messages"] = nlohmann::json::array({
        {{"role", "system"}, {"content", summarySystem}},
        {{"role", "user"},   {"content", summaryUser}},
    });

    wxString apiKey;
    if (route.needsApiKey) {
        apiKey = Preferences::GetApiKey(route.provider);
        if (apiKey.IsEmpty()) {
            // No key for the summary — fall back to dropping the head
            // without a summary, then send the actual request. The user
            // already saw the compaction notice; ApplyCompaction will add
            // a follow-up explaining the fallback.
            ApplyCompaction(false, std::string{}, "no API key configured");
            return;
        }
    }

    std::string body = req.dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace);

    WebRequestSpec spec;
    spec.url = route.url;
    spec.method = "POST";
    spec.body = std::move(body);
    spec.bodyContentType = "application/json";
    spec.headers.push_back({"Accept", "text/event-stream"});
    if (route.needsApiKey) {
        spec.headers.push_back({"Authorization",
                                "Bearer " + std::string(apiKey.utf8_string())});
    }
    spec.idleTimeoutSeconds = 60;

    request_ = StreamingWebRequest(
        this, std::move(spec),
        [this](std::string_view chunk) { OnSummaryStreamData(chunk); },
        [this](WebResponse resp) { OnSummaryStreamDone(std::move(resp)); });
}

void ChatFrame::OnSummaryStreamData(std::string_view chunk) {
    if (chunk.empty()) return;
    summarySseBuf_.append(chunk.data(), chunk.size());

    size_t pos;
    while ((pos = summarySseBuf_.find("\n\n")) != std::string::npos) {
        std::string event = summarySseBuf_.substr(0, pos);
        summarySseBuf_.erase(0, pos + 2);
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
                if (delta.contains("content") && delta["content"].is_string()) {
                    summaryText_ += delta["content"].get<std::string>();
                }
            } catch (...) {
                // Tolerate keepalives / partial JSON / malformed events.
            }
        }
    }
}

void ChatFrame::OnSummaryStreamDone(WebResponse resp) {
    if (quitRequested_) { Close(); return; }
    // User hit Escape during the summary call — abort the whole turn rather
    // than dropping the head with no summary and continuing.
    if (!resp.ok && resp.error == "cancelled") {
        compacting_ = false;
        compactionSplitIdx_ = -1;
        compactionHeadCount_ = 0;
        summaryText_.clear();
        summarySseBuf_.clear();
        RenderErrorBlock("Cancelled.");
        FinalizeTurn(true);
        return;
    }
    if (!resp.ok) {
        std::string err = resp.error.empty() ? "HTTP error" : resp.error;
        ApplyCompaction(false, std::string{}, err);
        return;
    }
    ApplyCompaction(!summaryText_.empty(), summaryText_, std::string{});
}

void ChatFrame::ApplyCompaction(bool success, const std::string& summary,
                                const std::string& error) {
    int splitIdx = compactionSplitIdx_;
    int origHeadCount = compactionHeadCount_;
    compactionSplitIdx_ = -1;
    compactionHeadCount_ = 0;
    summaryText_.clear();
    summarySseBuf_.clear();

    // Defensive: history could have changed shape under us (shouldn't on
    // the GUI thread, but be safe).
    if (splitIdx <= 0 || splitIdx > (int)history_.size()) {
        compacting_ = false;
        DoSendActualRequest();
        return;
    }

    std::string summaryBody;
    if (success) {
        summaryBody =
            "[Prior conversation summary — the earlier turns have been "
            "compacted into this summary to fit the model's context "
            "window. Treat it as authoritative background for continuing "
            "the current task.]\n\n" + summary;
        RenderErrorBlock(FormatU8(
            "📦 Context compacted: {} older messages replaced by a summary.",
            origHeadCount));
    } else {
        // Fallback — even a failed summary should shrink history,
        // otherwise the very next request would hit the same overflow.
        // historyCompactBaseCount_ still gates re-entry.
        summaryBody =
            "[Prior conversation context was dropped to fit the model's "
            "context window. Summary unavailable" +
            (error.empty() ? std::string{} : (": " + error)) + ".]";
        RenderErrorBlock(FormatU8(
            "⚠️ Compaction summary failed{} — dropping {} older messages "
            "without a summary so the next request can fit.",
            error.empty() ? std::string{} : (" (" + error + ")"),
            origHeadCount));
    }

    nlohmann::json summaryMsg = {
        {"role", "user"},
        {"content", std::move(summaryBody)},
        {"isSummary", true},
    };

    nlohmann::json newHist = nlohmann::json::array();
    newHist.push_back(std::move(summaryMsg));
    for (int i = splitIdx; i < (int)history_.size(); ++i) {
        newHist.push_back(std::move(history_[i]));
    }
    history_ = std::move(newHist);
    historyCompactBaseCount_ = (int)history_.size();
    PersistActive();

    compacting_ = false;
    DoSendActualRequest();
}
