#include "chat_frame.h"
#include "omarchy_theme.h"
#include "editor_indent.h"
#include "format_u8.h"
#include "inline_parser.h"
#include "tools.h"
#include "preferences.h"
#include "run_config_store.h"
#include "settings_dialog.h"
#include "image_store.h"
#include "debug_window.h"
#include "syntax.h"
#ifdef __WXOSX__
#include "mac_style.h"
#endif
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>
#include "perf_log.h"
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/stattext.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/dirdlg.h>
#include <wx/bmpbndl.h>
#include <wx/settings.h>
#include <wx/stdpaths.h>
#include <wx/filedlg.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/utils.h>
#include <wx/textdlg.h>
#include <wx/tokenzr.h>
#include <wx/base64.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <chrono>
#include <ctime>
#include <fstream>
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
constexpr int ID_EXPORT   = wxID_HIGHEST + 14;
constexpr int ID_HAMBURGER = wxID_HIGHEST + 15;
constexpr int ID_EDITOR   = wxID_HIGHEST + 16;
constexpr int ID_TREE_NEW_FILE      = wxID_HIGHEST + 17;
constexpr int ID_TREE_RENAME        = wxID_HIGHEST + 18;
constexpr int ID_TREE_SHOW_IN_FILES = wxID_HIGHEST + 19;
constexpr int ID_TREE_NEW_FOLDER    = wxID_HIGHEST + 20;
constexpr int ID_TREE_REFRESH       = wxID_HIGHEST + 21;
constexpr int ID_TREE_TOGGLE_HIDDEN = wxID_HIGHEST + 27;
constexpr int ID_EDITOR_SAVE        = wxID_HIGHEST + 21;
constexpr int ID_EDITOR_SAVE_AS     = wxID_HIGHEST + 22;
constexpr int ID_EDITOR_RELOAD      = wxID_HIGHEST + 23;
constexpr int ID_EDITOR_CLOSE       = wxID_HIGHEST + 24;
constexpr int ID_EDITOR_SHOW_IN_FILES = wxID_HIGHEST + 25;
constexpr int ID_MODEL_REFRESH        = wxID_HIGHEST + 26;

// Side-panel widths (pixels). The import pane lives in the outer splitter and
// the editor pane lives in the inner (chat | editor) splitter, so the window
// grows by each pane width plus its splitter sash to keep the chat pane fixed.
constexpr int kImportPaneWidth  = 400;
constexpr int kFileTreeWidth    = 280;   // fixed (non-resizable) file tree width
constexpr int kEditorTextWidth  = kFileTreeWidth * 5 / 2;  // editor text = 2.5x tree
constexpr int kEditorPaneWidth  = kFileTreeWidth + kEditorTextWidth;  // whole right pane
constexpr int kMainMinClientW   = 220;   // min chat-pane width with no panels
// Chat-pane widths below which toolbar items hide, least important first.
constexpr int kToolbarLabelsW   = 640;   // "Session:" / "Model:" captions
constexpr int kToolbarSettingsW = 500;   // settings (and debug Log) button
constexpr int kToolbarExportW   = 466;   // export button
constexpr int kToolbarModelW    = 430;   // model dropdown

// Payload attached to each file-tree node.
class FileTreeItemData : public wxTreeItemData {
public:
    FileTreeItemData(const wxString& p, bool d) : path(p), isDir(d) {}
    wxString path;
    bool isDir;
};

// ---- Context management (compaction.md) ----
// Tail retention: keep the most recent ~15K tokens of conversation verbatim;
// everything older is the "head" that gets summarized. This mirrors OpenCode's
// preserveRecentBudget (which caps at 15K for every model we route to). The
// tail is the only part re-sent every round, so bounding it controls cost.
constexpr int kTailBudgetTokens      = 15'000;
constexpr int kSummaryMaxTokens      = 8'000;
// Output ceiling, matching OpenCode's OUTPUT_TOKEN_MAX (32K). Replaces the old
// 384K cap so a runaway reasoning/write stream can't bill 384K in one response.
constexpr int kOutputTokenMax        = 32'000;
// DeepSeek at "max" reasoning effort can think long enough that the reasoning
// alone would use up a 32K budget before any answer; give it the 384K output
// DeepSeek recommends for agents. The context-window clamp still applies.
constexpr int kOutputTokenMaxDeepSeekMax = 384'000;
// Reserve kept free of the input estimate when deciding to compact (OpenCode's
// COMPACTION_BUFFER). Compaction fires when the rendered view reaches
// contextWindow - kBufferTokens.
constexpr int kBufferTokens          = 20'000;
constexpr int kToolOutputMaxChars    = 2'000;
// Cap on any single tool-call argument re-sent in the tail (e.g. a giant
// write_file `content`). A1 ages oversized args out of the 15K tail, but this
// bounds the residual case where one oversized call sits inside the retained
// tail. The model already executed the call (its result follows), so it only
// needs the shape of the args, not the full payload.
constexpr int kToolCallArgsMaxChars  = 32'000;
constexpr int kPruneProtectTokens    = 40'000;
constexpr int kPruneMinFreedTokens   = 20'000;
// Tool-output pruning protects outputs from the most recent kPruneFreshTurns
// user turns (OpenCode's prune keeps the last 2 turns verbatim).
constexpr int kPruneFreshTurns       = 2;

int EstimateMessageTokens(const nlohmann::json& m) {
    if (!m.is_object()) return 0;
    int chars = 0;
    if (m.contains("content") && m["content"].is_string())
        chars += (int)m["content"].get_ref<const std::string&>().size();
    if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
        chars += (int)m["reasoning_content"].get_ref<const std::string&>().size();
    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) {
            if (tc.is_object() && tc.contains("function")
                && tc["function"].is_object()
                && tc["function"].contains("arguments")
                && tc["function"]["arguments"].is_string())
                chars += (int)tc["function"]["arguments"]
                             .get_ref<const std::string&>().size();
        }
    }
    return (chars + 3) / 4;
}

// Estimate the input tokens of a rendered message array (chars/3 ≈ 3.3
// tokens/char, plus fixed overhead for tool definitions + JSON structure).
// Used both to clamp max_tokens and to decide when compaction must fire.
int EstimatePromptTokens(const nlohmann::json& messages) {
    size_t promptChars = 0;
    for (const auto& m : messages) {
        if (!m.is_object()) continue;
        if (m.contains("content") && m["content"].is_string())
            promptChars += m["content"].get_ref<const std::string&>().size();
        if (m.contains("reasoning_content") && m["reasoning_content"].is_string())
            promptChars += m["reasoning_content"].get_ref<const std::string&>().size();
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (tc.is_object() && tc.contains("function")
                    && tc["function"].is_object()
                    && tc["function"].contains("arguments")
                    && tc["function"]["arguments"].is_string())
                    promptChars += tc["function"]["arguments"]
                                     .get_ref<const std::string&>().size();
            }
        }
    }
    promptChars += 32000;  // tool definitions + JSON structural overhead
    return (int)(promptChars / 3);
}


std::string MimeForImageFile(const std::string& path) {
    std::string ext = wxFileName(path).GetExt().Lower().ToStdString(wxConvUTF8);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    return {};
}

// Scale `img` to fill a square `size` x `size`, center-cropping any overflow,
// so thumbnails keep a uniform square shape without distortion.
wxBitmap SquareThumbnail(const wxImage& img, int size) {
    int w = img.GetWidth();
    int h = img.GetHeight();
    if (w <= 0 || h <= 0) return wxNullBitmap;
    int side = std::min(w, h);
    int x = (w - side) / 2;
    int y = (h - side) / 2;
    wxImage crop = img.GetSubImage(wxRect(x, y, side, side));
    return wxBitmap(crop.Scale(size, size, wxIMAGE_QUALITY_HIGH));
}

class FrameFileDropTarget : public wxFileDropTarget {
public:
    explicit FrameFileDropTarget(ChatFrame* frame) : frame_(frame) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override {
        frame_->AddDroppedFiles(filenames);
        return true;
    }
private:
    ChatFrame* frame_;
};

// Thumbnail with a small "x" badge overlaid on its top-right corner. The
// badge is the delete affordance; the rest of the tile is inert for now.
class ThumbnailItem : public wxPanel {
public:
    ThumbnailItem(wxWindow* parent, const wxBitmap& bmp,
                  std::function<void()> onRemove)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kSize, kSize)),
          bmp_(bmp), onRemove_(std::move(onRemove)) {
        SetMinSize(wxSize(kSize, kSize));
        SetMaxSize(wxSize(kSize, kSize));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetToolTip("Remove image");
        Bind(wxEVT_PAINT, &ThumbnailItem::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &ThumbnailItem::OnLeftDown, this);
    }

private:
    static constexpr int kSize = 64;
    static constexpr int kPad = 4;
    static constexpr int kBadgeR = 9;

    wxBitmap bmp_;
    std::function<void()> onRemove_;

    wxPoint BadgeCenter() const {
        return wxPoint(kSize - kBadgeR - 1, kBadgeR + 1);
    }

    bool InBadge(const wxPoint& p) const {
        wxPoint c = BadgeCenter();
        int dx = p.x - c.x, dy = p.y - c.y;
        return dx * dx + dy * dy <= (kBadgeR + 2) * (kBadgeR + 2);
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(46, 46, 52)));
        dc.Clear();
        dc.DrawBitmap(bmp_, kPad, kPad, true);

        wxPoint c = BadgeCenter();
        dc.SetBrush(wxBrush(wxColour(28, 28, 32)));
        dc.SetPen(wxPen(wxColour(90, 90, 98)));
        dc.DrawCircle(c, kBadgeR);
        dc.SetPen(wxPen(wxColour(225, 225, 230), 2));
        dc.DrawLine(c.x - 4, c.y - 4, c.x + 4, c.y + 4);
        dc.DrawLine(c.x - 4, c.y + 4, c.x + 4, c.y - 4);
    }

    void OnLeftDown(wxMouseEvent& e) {
        if (InBadge(e.GetPosition())) {
            // Defer via the app so we don't destroy this window from inside
            // its own mouse handler (wx use-after-free -> GTK crash).
            wxTheApp->CallAfter(onRemove_);
        }
        e.Skip();
    }
};

// Per-model routing config. Resolved fresh at each StartCompletion so a model
// change during a tool-call loop applies on the next request.
struct ModelRoute {
    const char* url;
    const char* model;
    bool needsApiKey;
    Preferences::Provider provider;  // only valid when needsApiKey
    // Output ceiling, matching OpenCode's OUTPUT_TOKEN_MAX (32K). Leaving it
    // unset would let deepseek apply its 4096 server-side default, which clips
    // `write_file` arguments mid-JSON and triggers an unrecoverable "missing
    // 'path' argument" loop. 32K is plenty for any single response and also
    // bounds reasoning-token blowups that previously billed 384K.
    int maxTokens;
    // Total input+output token budget for the model. Used by the context
    // compactor to decide when to summarize the head of history. Set per
    // model from the published context window; conservative values are
    // fine — compaction triggers earlier rather than later.
    int contextWindow;
    // Runs through the user's Claude Code CLI instead of an HTTP endpoint;
    // url/needsApiKey/maxTokens don't apply.
    bool claudeCli = false;
};

// A model index names a model independently of the dropdown layout, and is
// what gets persisted: 0 is always the free tier (Kilo Gateway), 1.. map into
// the DeepSeek list (remoteModels_) — which is either the live GET /models
// result or the hardcoded fallback below, never both — and
// kClaudeIndexBase.. are the Claude entries. The dropdown shows Claude after
// DeepSeek, so a Claude entry's row depends on how many DeepSeek models are
// listed; ModelRowForIndex / ModelIndexForRow convert.
constexpr int kModelKiloFree = 0;
constexpr int kClaudeIndexBase = 100;

// Claude models, run through the user's own Claude Code CLI.
struct ClaudeModelEntry {
    const char* id;
    const char* label;
};
const ClaudeModelEntry kClaudeModels[] = {
    {"claude-opus-5-5", "Claude (Opus 5.5)"},
    {"claude-sonnet-5", "Claude (Sonnet 5)"},
};
constexpr int kClaudeModelCount =
    (int)(sizeof(kClaudeModels) / sizeof(kClaudeModels[0]));

// Hardcoded DeepSeek fallback, used only when GET /models is unavailable
// (no API key, network error, or unparseable response). Kept in a stable
// order (flash, then pro) so a persisted dropdown index resolves to the same
// model whether the live list or this fallback is showing.
const std::vector<std::string>& FallbackDeepseekModels() {
    static const std::vector<std::string> models = {
        "deepseek-flash", "deepseek-v4-pro"};
    return models;
}

// The DeepSeek ids the dropdown shows: the live list when the fetch
// succeeded, the fallback otherwise.
const std::vector<std::string>& DeepseekModels(
    const std::vector<std::string>& remoteModels) {
    return remoteModels.empty() ? FallbackDeepseekModels() : remoteModels;
}

// Dropdown row for a model index, or -1 when that model isn't listed (a
// DeepSeek index past the end of the current list).
int ModelRowForIndex(int idx, const std::vector<std::string>& remoteModels) {
    const int dsCount = (int)DeepseekModels(remoteModels).size();
    if (idx >= kClaudeIndexBase && idx < kClaudeIndexBase + kClaudeModelCount)
        return 1 + dsCount + (idx - kClaudeIndexBase);
    if (idx >= 0 && idx <= dsCount) return idx;
    return -1;
}

int ModelIndexForRow(int row, const std::vector<std::string>& remoteModels) {
    const int dsCount = (int)DeepseekModels(remoteModels).size();
    if (row <= dsCount) return row;
    return kClaudeIndexBase + (row - 1 - dsCount);
}

ModelRoute RouteForIndex(int idx, const std::vector<std::string>& remoteModels) {
    if (idx == kModelKiloFree) {
        // Kilo Gateway free tier: kilo-auto/free auto-routes to whichever
        // free model is currently available. OpenAI-compatible, no API key,
        // 256K context, anonymous rate limit 200 requests/hour/IP. Free
        // models are shared-pool and best-effort, so this is the no-key
        // default, not a replacement for a DeepSeek key.
        return {"https://api.kilo.ai/api/gateway/chat/completions",
                "kilo-auto/free", false, Preferences::Provider::DeepSeek,
                kOutputTokenMax, 256000};
    }

    if (idx >= kClaudeIndexBase && idx < kClaudeIndexBase + kClaudeModelCount) {
        // Claude Code manages its own context window and output limits.
        return {"", kClaudeModels[idx - kClaudeIndexBase].id, false,
                Preferences::Provider::DeepSeek, kOutputTokenMax, 1000000,
                /*claudeCli=*/true};
    }

    // DeepSeek models occupy indices >= 1. `remoteModels` is the single
    // source of truth for DeepSeek ids: the live list when the fetch
    // succeeded, the fallback above otherwise.
    const std::vector<std::string>& models = DeepseekModels(remoteModels);
    size_t i = (size_t)(idx - 1);
    if (i < models.size()) {
        return {"https://api.deepseek.com/chat/completions",
                models[i].c_str(), true, Preferences::Provider::DeepSeek,
                kOutputTokenMax, 1000000};
    }

    // Unknown/stale index — fall back to the no-key free provider.
    return {"https://api.kilo.ai/api/gateway/chat/completions",
            "kilo-auto/free", false, Preferences::Provider::DeepSeek,
            32000, 256000};
}

// "deepseek-v4.1-flash" -> "DeepSeek V4.1 Flash".
wxString RemoteModelLabel(const std::string& id) {
    std::string s = id;
    const std::string prefix = "deepseek-";
    if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
    bool cap = true;
    for (char& c : s) {
        if (c == '-') {
            c = ' ';
            cap = true;
        } else if (cap) {
            c = (char)std::toupper((unsigned char)c);
            cap = false;
        }
    }
    return wxString::FromUTF8("DeepSeek " + s);
}

// Route for an OpenAI-style completion (the first request of a turn or a
// tool-loop continuation). Switching the dropdown to Claude mid tool loop
// can't hand that loop to Claude Code, so it finishes on the model the turn
// started with.
ModelRoute CompletionRoute(int currentIdx, int turnIdx,
                           const std::vector<std::string>& remoteModels) {
    ModelRoute route = RouteForIndex(currentIdx, remoteModels);
    if (route.claudeCli) route = RouteForIndex(turnIdx, remoteModels);
    if (route.claudeCli) route = RouteForIndex(kModelKiloFree, remoteModels);
    return route;
}

// String field of a history/event object, or empty when missing or not a
// string. Imported sessions and CLI events are untrusted shapes; value()
// would throw on a type mismatch.
std::string StringField(const nlohmann::json& m, const char* key) {
    if (!m.is_object()) return std::string();
    auto it = m.find(key);
    if (it == m.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

bool IsClaudeModelId(const std::string& id) {
    return id.rfind("claude-", 0) == 0;
}

// Dropdown-style label for a model id recorded in history ("model" field).
wxString ModelLabelForId(const std::string& id) {
    if (id == "kilo-auto/free") return "Kilo Free";
    for (const auto& c : kClaudeModels) {
        if (id == c.id) return c.label;
    }
    if (IsClaudeModelId(id)) return wxString::FromUTF8("Claude (" + id + ")");
    if (id.rfind("deepseek-", 0) == 0) return RemoteModelLabel(id);
    return wxString::FromUTF8(id);
}

bool IsUserTurn(const nlohmann::json& m) {
    return m.is_object() && StringField(m, "role") == "user"
           && !m.value("isSummary", false);
}

// Replays user turns in order and says when one went to a different model
// than the turn before, from the "model" each user message records. Turns
// saved before gritcode recorded models have none, so the first tagged turn
// after them names its model instead of claiming a switch.
class ModelSwitchTracker {
public:
    // Notice to show before this user message, or empty. With announceFirst
    // the very first turn also names its model (used by the import viewer).
    wxString Next(const nlohmann::json& userMsg, bool announceFirst) {
        const std::string model = StringField(userMsg, "model");
        wxString notice;
        if (!model.empty() && model != last_) {
            if (!last_.empty())
                notice = "Switched to " + ModelLabelForId(model);
            else if (sawTurn_ || announceFirst)
                notice = "Model: " + ModelLabelForId(model);
        }
        if (!model.empty()) last_ = model;
        sawTurn_ = true;
        return notice;
    }

private:
    std::string last_;
    bool sawTurn_ = false;
};

// Italic one-line paragraph used for errors and status notices.
Block NoticeBlock(const wxString& msg) {
    Block b;
    b.type = BlockType::Paragraph;
    b.rawText = msg;
    b.visibleText = msg;
    InlineRun r; r.text = msg; r.italic = true;
    b.runs.push_back(r);
    return b;
}

std::string Truncated(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "\n[truncated]";
}

// Other models can't take Claude Code's tool calls as tool calls: those tools
// aren't in their request, and some APIs reject calls to undeclared tools. So
// each run of Claude assistant/tool messages is folded into one plain
// assistant message describing what Claude did.
nlohmann::json FlattenClaudeTurns(nlohmann::json messages) {
    nlohmann::json out = nlohmann::json::array();
    std::string merged;
    bool inRun = false;
    auto add = [&merged](const std::string& s) {
        if (s.empty()) return;
        if (!merged.empty()) merged += "\n\n";
        merged += s;
    };
    auto flush = [&]() {
        if (!inRun) return;
        out.push_back({{"role", "assistant"}, {"content", merged}});
        merged.clear();
        inRun = false;
    };
    for (auto& m : messages) {
        const std::string role = StringField(m, "role");
        const bool fromClaude = (role == "assistant" || role == "tool")
                                && IsClaudeModelId(StringField(m, "model"));
        if (!fromClaude) {
            flush();
            out.push_back(std::move(m));
            continue;
        }
        inRun = true;
        if (role == "tool") {
            add("[Tool result: " + Truncated(StringField(m, "content"), 1000) + "]");
            continue;
        }
        add(StringField(m, "content"));
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function")) continue;
                add("[Claude Code tool call: " + StringField(tc["function"], "name")
                    + " " + Truncated(StringField(tc["function"], "arguments"), 500)
                    + "]");
            }
        }
    }
    flush();
    return out;
}

// Plain-text transcript of h[from, to), used to hand a conversation to Claude
// Code when it didn't take part in it: the session started on another model,
// or other models answered since Claude's last turn. With skipClaude,
// Claude's own messages are left out (its session already has them). Keeps
// the most recent part when long.
std::string HandoverTranscript(const std::vector<nlohmann::json>& h,
                               size_t from, size_t to, bool skipClaude) {
    constexpr size_t kMaxChars = 120'000;
    std::string out;
    for (size_t i = from; i < to && i < h.size(); ++i) {
        const auto& m = h[i];
        if (!m.is_object() || m.value("compacted", false)) continue;
        const std::string role = StringField(m, "role");
        const std::string model = StringField(m, "model");
        if (role == "system") continue;
        if (skipClaude && IsClaudeModelId(model)) continue;
        if (m.value("isSummary", false)) {
            out += "--- summary of earlier conversation ---\n"
                   + StringField(m, "content") + "\n\n";
            continue;
        }
        if (role == "tool") {
            out += "[tool result " + StringField(m, "name") + "]\n"
                   + Truncated(StringField(m, "content"), 2000) + "\n\n";
            continue;
        }
        out += "--- " + role;
        if (role == "assistant" && !model.empty())
            out += " (" + ModelLabelForId(model).utf8_string() + ")";
        out += " ---\n";
        const std::string content = StringField(m, "content");
        if (!content.empty()) out += content + "\n";
        if (m.contains("images") && m["images"].is_array()) {
            for (const auto& img : m["images"]) {
                std::string path = ImageStore::PathFor(
                    StringField(img, "sha256"), StringField(img, "mime"));
                if (!path.empty()) out += "[attached image: " + path + "]\n";
            }
        }
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function")) continue;
                out += "[tool call " + StringField(tc["function"], "name") + " "
                       + Truncated(StringField(tc["function"], "arguments"), 1000)
                       + "]\n";
            }
        }
        out += "\n";
    }
    if (out.size() > kMaxChars) {
        out = "[... earlier transcript truncated ...]\n\n"
              + out.substr(out.size() - kMaxChars);
    }
    return out;
}

// Text of a Claude Code tool_result `content`: a string, or an array of
// content blocks (text, images).
std::string ClaudeToolResultText(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return content.is_null() ? std::string() : content.dump();
    std::string out;
    for (const auto& b : content) {
        const std::string type = StringField(b, "type");
        std::string part;
        if (type == "text") part = StringField(b, "text");
        else if (type == "image") part = "[image]";
        else if (b.is_string()) part = b.get<std::string>();
        if (part.empty()) continue;
        if (!out.empty()) out += "\n";
        out += part;
    }
    return out;
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

// ---- AGENTS.md project instructions (OpenCode-compatible) ----

// Directory holding gritcode's global AGENTS.md (next to gritcode.conf),
// mirroring OpenCode's ~/.config/<app>/AGENTS.md convention.
std::string GlobalAgentsDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (xdg[0] != '\0') return std::string(xdg) + "/gritcode";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.config/gritcode";
    }
    return std::string();
}

// Walk up from `cwd` to the nearest directory holding `.git` (a dir for a
// clone, a file for a worktree). Empty when not inside a repository.
std::string FindGitRoot(const std::string& cwd) {
    std::string cur = cwd;
    while (true) {
        if (wxDirExists(cur + "/.git") || wxFileExists(cur + "/.git"))
            return cur;
        const size_t pos = cur.find_last_of("/\\");
        if (pos == std::string::npos) break;
        const std::string parent = cur.substr(0, pos);
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }
    return std::string();
}

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Build the AGENTS.md instruction text for a session rooted at `cwd`: the
// global file first, then project files from `cwd` up to the git root
// (closest first), mirroring OpenCode's discovery order. Empty when none
// exist. Each file is labelled with its path so the model knows the source.
std::string LoadAgentsInstructions(const std::string& cwd) {
    std::vector<std::string> paths;

    const std::string global = GlobalAgentsDir();
    if (!global.empty()) paths.push_back(global + "/AGENTS.md");

    const std::string root = FindGitRoot(cwd);
    std::string cur = cwd;
    while (true) {
        paths.push_back(cur + "/AGENTS.md");
        if (!root.empty() && cur == root) break;
        if (root.empty()) break;  // not in a repo: only the session dir
        const size_t pos = cur.find_last_of("/\\");
        if (pos == std::string::npos) break;
        const std::string parent = cur.substr(0, pos);
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }

    std::string out;
    for (const auto& p : paths) {
        if (!wxFileExists(p)) continue;
        std::string content;
        if (!ReadTextFile(p, content)) continue;
        if (content.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += "Instructions from: " + p + "\n" + content;
    }
    return out;
}

// The static base of the system prompt (platform info + tool guidance).
// AGENTS.md instructions are appended to this by both SeedSystemPrompt and
// RefreshSystemPromptAgents so the same base is used for fresh and restored
// sessions alike.
std::string BaseSystemPrompt() {
    std::string platformInfo;
#ifdef _WIN32
    platformInfo = "You are running on Windows. The shell tool uses cmd /c - "
                   "use Windows commands (dir, type, findstr, del, etc.). "
                   "Path separators are backslashes.";
#elif defined(__APPLE__)
    platformInfo = "You are running on macOS. The shell tool uses bash. "
                   "Use Unix commands. Path separators are forward slashes.";
#else
    platformInfo = "You are running on Linux. The shell tool uses bash. "
                   "Use Unix commands. Path separators are forward slashes.";
#endif

    return
        "You are a helpful AI coding assistant with access to local file, "
        "shell, and web tools. Use them when the user's request requires "
        "reading files, running shell commands, or fetching web pages. "
        "Prefer concrete actions over speculation. Use markdown for "
        "formatting and fenced code blocks for code.\n\n"
        + platformInfo + "\n\n"
        "Play button: the ▶ button runs a single stored shell command "
        "directly from the project root - it does NOT invoke the AI. "
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
        "query - if it returns no relevant hits, stop and answer from "
        "what you have rather than firing speculative variants.";
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

#ifdef _WIN32
wxString GetCacertPath() {
    static wxString cached = []() -> wxString {
        wxString exeDir = wxStandardPaths::Get().GetResourcesDir();
        if (wxFileName::FileExists(exeDir + "/cacert.pem"))
            return exeDir + "/cacert.pem";
        if (wxFileName::FileExists(exeDir + "/../cacert.pem"))
            return exeDir + "/../cacert.pem";
        return exeDir + "/cacert.pem";
    }();
    return cached;
}
#endif

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
    fn.Normalize(wxPATH_NORM_ALL);
    if (!fn.FileExists()) {
        // Dev fallback: source-tree packaging directory.
        fn.Assign(wxString(GRITCODE_ASSETS_DIR) + "/../packaging/gritcode.svg");
        fn.Normalize(wxPATH_NORM_ALL);
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

struct ImageRef {
    std::string hash;
    std::string mime;
    std::string name;
};

bool HistoryHasImages(const nlohmann::json& history) {
    for (const auto& m : history) {
        if (!m.is_object()) continue;
        if (m.contains("images") && m["images"].is_array() && !m["images"].empty())
            return true;
    }
    return false;
}

std::vector<ImageRef> CollectImageRefs(const nlohmann::json& history) {
    std::vector<ImageRef> refs;
    std::set<std::string> seen;
    for (const auto& m : history) {
        if (!m.is_object() || !m.contains("images") || !m["images"].is_array())
            continue;
        for (const auto& img : m["images"]) {
            if (!img.is_object()) continue;
            std::string hash = img.value("sha256", std::string{});
            if (hash.empty() || seen.count(hash)) continue;
            seen.insert(hash);
            refs.push_back({hash, img.value("mime", "image/png"),
                            img.value("name", std::string{})});
        }
    }
    return refs;
}

std::string ReadAllStream(wxInputStream& in) {
    std::string out;
    char buf[8192];
    for (;;) {
        in.Read(buf, sizeof(buf));
        size_t n = in.LastRead();
        if (n == 0) break;
        out.append(buf, n);
    }
    return out;
}

bool ExportToZip(const std::string& path, const nlohmann::json& j,
                 const std::vector<ImageRef>& refs, std::string& err) {
    wxFFileOutputStream out(wxString::FromUTF8(path));
    if (!out.IsOk()) { err = "cannot write file"; return false; }
    wxZipOutputStream zip(out);
    if (!zip.IsOk()) { err = "cannot create archive"; return false; }

    std::string body = j.dump(2, ' ', false,
                              nlohmann::json::error_handler_t::replace);
    if (!zip.PutNextEntry("session.json")) { err = "archive write failed"; return false; }
    zip.Write(body.data(), body.size());

    for (const auto& r : refs) {
        std::string blobPath = ImageStore::PathFor(r.hash, r.mime);
        if (blobPath.empty()) continue;  // robustness: skip missing blobs
        std::ifstream in(blobPath, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string bytes = ss.str();
        std::string fname = wxFileName(wxString::FromUTF8(blobPath))
                                .GetFullName().ToStdString(wxConvUTF8);
        if (!zip.PutNextEntry(wxString::FromUTF8("images/" + fname)))
            continue;
        zip.Write(bytes.data(), bytes.size());
    }
    zip.Close();
    return true;
}

bool ImportFromZip(const std::string& path, nlohmann::json& outJ,
                   std::string& err) {
    wxFFileInputStream in(wxString::FromUTF8(path));
    if (!in.IsOk()) { err = "cannot open file"; return false; }
    wxZipInputStream zip(in);
    if (!zip.IsOk()) { err = "cannot read archive"; return false; }

    std::string sessionJson;
    std::map<std::string, std::string> blobs;

    wxZipEntry* entry = nullptr;
    while ((entry = zip.GetNextEntry()) != nullptr) {
        std::string name = entry->GetName().ToStdString(wxConvUTF8);
        if (entry->IsDir()) { delete entry; continue; }
        if (zip.OpenEntry(*entry)) {
            std::string bytes = ReadAllStream(zip);
            if (name == "session.json") sessionJson = std::move(bytes);
            else if (name.rfind("images/", 0) == 0) blobs[name] = std::move(bytes);
        }
        delete entry;
    }

    if (sessionJson.empty()) { err = "session.json missing in archive"; return false; }
    try { outJ = nlohmann::json::parse(sessionJson); }
    catch (...) { err = "invalid session.json in archive"; return false; }

    // Store extracted images and self-heal stale/mismatched hashes.
    if (outJ.contains("messages") && outJ["messages"].is_array()) {
        for (auto& m : outJ["messages"]) {
            if (!m.is_object() || !m.contains("images") || !m["images"].is_array())
                continue;
            for (auto& img : m["images"]) {
                if (!img.is_object()) continue;
                std::string hash = img.value("sha256", std::string{});
                std::string mime = img.value("mime", "image/png");
                std::string prefix = "images/" + hash + ".";
                std::string bytes;
                for (const auto& [k, v] : blobs) {
                    if (k.rfind(prefix, 0) == 0) { bytes = v; break; }
                }
                if (bytes.empty()) continue;  // robustness: blob missing
                std::string savedHash = ImageStore::Save(bytes, mime);
                if (!savedHash.empty() && savedHash != hash) {
                    img["sha256"] = savedHash;
                }
            }
        }
    }
    return true;
}

// Session export format. "version" 1 is plain JSON, 2 is a zip with image
// blobs. New per-message fields (e.g. "model") are additive: older gritcode
// ignores fields it doesn't know, so they don't bump the version. Bump past
// kMaxSessionVersion only for a change older readers would misread; they
// then refuse the file and ask the user to update.
constexpr int kMaxSessionVersion = 2;

bool ImportSessionFile(const std::string& path, nlohmann::json& outJ,
                       std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "Failed to open file."; return false; }
    char magic[4] = {0};
    f.read(magic, 4);
    f.close();
    bool isZip = (magic[0] == 'P' && magic[1] == 'K'
                  && magic[2] == 3 && magic[3] == 4);
    if (isZip) {
        if (!ImportFromZip(path, outJ, err)) return false;
    } else {
        std::ifstream fj(path);
        try { fj >> outJ; }
        catch (...) {
            err = "Couldn't read this file as a gritcode session.\n\n"
                  "If you're sure it's a valid session file, it may have been "
                  "created by a newer gritcode version - try updating to the "
                  "latest version.";
            return false;
        }
    }
    auto v = outJ.is_object() ? outJ.find("version") : outJ.end();
    if (outJ.is_object() && v != outJ.end() && v->is_number_integer()
        && v->get<int>() > kMaxSessionVersion) {
        err = "This session was exported by a newer version of gritcode.\n\n"
              "Update gritcode to the latest version to open it.";
        return false;
    }
    return true;
}

}  // namespace

// Padding between the window edge and the panes. macOS gets a little more
// room: Tahoe's rounder window corners crowd controls sitting 2px in.
#ifdef __WXOSX__
constexpr int kEdgePad = 4;
#else
constexpr int kEdgePad = 2;
#endif

// Default window width. macOS popups are wider than GTK's, and at 600 the
// toolbar row squeezes the Model dropdown down to nothing.
#ifdef __WXOSX__
constexpr int kDefaultWidth = 640;
#else
constexpr int kDefaultWidth = 600;
#endif

ChatFrame::ChatFrame()
    : wxFrame(nullptr, wxID_ANY, "gritcode",
              wxDefaultPosition, wxSize(kDefaultWidth, 850)) {
    PERF_SCOPE("ChatFrame::ctor");

    { PERF_SCOPE("LoadAppIcon"); SetIcons(LoadAppIcon()); }

    splitter_ = new wxSplitterWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    splitter_->SetMinimumPaneSize(200);

    // Inner splitter holds the main chat (left) and the editor (right). It is
    // nested inside the import/main splitter: the outer sash divides
    // import | chat, the inner sash divides chat | editor. No sash gravity is
    // set — SyncPanelSizing keeps the chat pane at a fixed width and lets the
    // editor absorb any transient resize while the outer splitter re-parents.
    innerSplitter_ = new wxSplitterWindow(splitter_, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE);
    innerSplitter_->SetMinimumPaneSize(150);
    editorPaneW_ = kEditorPaneWidth;

    // Main panel: left pane of the inner splitter.
    auto* panel = new wxPanel(innerSplitter_);
    mainPanel_ = panel;
    auto* outer = new wxBoxSizer(wxVERTICAL);

    canvas_ = new ChatCanvas(panel);
    canvas_->Bind(wxEVT_CANVAS_LINK, &ChatFrame::OnCanvasLink, this);
    outer->Add(canvas_, 1, wxEXPAND);

    // Toolbar row below the input:
    //   Session: [▾]   Model: [▾]   <stretch>   [⚙]
    const wxSize kIconSize(20, 20);
    const wxSize kBtnSize(FromDIP(32), -1);
    wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    wxBitmapBundle bbSettings = LoadThemedSvgIcon("settings.svg", kIconSize, accent);

    auto* toolbarRow = new wxBoxSizer(wxHORIZONTAL);
    toolbarRow_ = toolbarRow;

    wxBitmapBundle bbHamburger = LoadThemedSvgIcon("hamburger.svg", kIconSize, accent);
    hamburgerBtn_ = new wxBitmapButton(panel, ID_HAMBURGER, bbHamburger,
                                        wxDefaultPosition, kBtnSize,
                                        wxBORDER_NONE);
    hamburgerBtn_->SetToolTip(wxString::FromUTF8("Toggle session reference"));

    sessionLabel_ = new wxStaticText(panel, wxID_ANY, "Session:");
    sessionChoice_ = new wxChoice(panel, ID_SESSION);
    sessionChoice_->SetMinSize(FromDIP(wxSize(160, -1)));
    modelLabel_ = new wxStaticText(panel, wxID_ANY, "Model:");
    modelChoice_ = new wxChoice(panel, ID_MODEL);
    modelChoice_->SetMinSize(FromDIP(wxSize(150, -1)));
    currentModelIndex_ = Preferences::GetLastModelIndex();
    RebuildModelChoice();
    settingsBtn_ = new wxBitmapButton(panel, ID_SETTINGS, bbSettings,
                                      wxDefaultPosition, kBtnSize,
                                      wxBORDER_NONE);
    settingsBtn_->SetToolTip(wxString::FromUTF8("Settings…"));

    wxBitmapBundle bbExport = LoadThemedSvgIcon("export.svg", kIconSize, accent);
    exportBtn_ = new wxBitmapButton(panel, ID_EXPORT, bbExport,
                                     wxDefaultPosition, kBtnSize,
                                     wxBORDER_NONE);
    exportBtn_->SetToolTip(wxString::FromUTF8("Export session to file"));

    wxBitmapBundle bbEditor = LoadThemedSvgIcon("editor.svg", kIconSize, accent);
    editorBtn_ = new wxBitmapButton(panel, ID_EDITOR, bbEditor,
                                    wxDefaultPosition, kBtnSize,
                                    wxBORDER_NONE);
    editorBtn_->SetToolTip(wxString::FromUTF8("Toggle editor"));

    wxBitmapBundle bbPlay = LoadThemedSvgIcon("play.svg", kIconSize, accent);
    playBtn_ = new wxBitmapButton(panel, ID_PLAY, bbPlay,
                                  wxDefaultPosition, kBtnSize,
                                  wxBORDER_NONE);
    playBtn_->SetToolTip(wxString::FromUTF8("Build and run project"));

    toolbarRow->Add(hamburgerBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    toolbarRow->Add(sessionLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    // Session and model both get a small proportion so they share resize delta;
    // the stretch spacer absorbs most of it. Once the spacer collapses (narrow
    // window) both dropdowns shrink toward their MinSize.
    toolbarRow->Add(sessionChoice_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    toolbarRow->Add(playBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    toolbarRow->Add(modelLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    toolbarRow->Add(modelChoice_, 1, wxALIGN_CENTER_VERTICAL);
    toolbarRow->AddStretchSpacer(8);
    toolbarRow->Add(settingsBtn_, 0, wxALIGN_CENTER_VERTICAL);
    toolbarRow->Add(exportBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
#ifndef NDEBUG
    auto* debugBtn = debugBtn_ = new wxButton(panel, wxID_ANY, "Log",
                                  wxDefaultPosition, FromDIP(wxSize(48, -1)),
                                  wxBORDER_NONE);
    debugBtn->SetToolTip("Open debug log (request bodies + compaction)");
    debugBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenDebugWindow(); });
    toolbarRow->Add(debugBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
#endif
    toolbarRow->Add(editorBtn_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

    // Chip row — wraps to multiple lines if the queue gets long. Hidden
    // (via sizer Show) until the queue has at least one entry.
    chipRow_ = new wxPanel(panel);
    chipSizer_ = new wxBoxSizer(wxVERTICAL);
    chipRow_->SetSizer(chipSizer_);
    chipRow_->Hide();

    // Pending attached images: deletable thumbnails shown above the input.
    imageRow_ = new wxPanel(panel);
    imageSizer_ = new wxWrapSizer(wxHORIZONTAL);
    imageRow_->SetSizer(imageSizer_);
    imageRow_->Hide();

    auto* inputRow = new wxBoxSizer(wxHORIZONTAL);
    input_ = new wxTextCtrl(panel, ID_INPUT, "",
                            wxDefaultPosition, wxSize(-1, 60),
                            wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    // wxTextCtrl has its own file-drop handler (inserts the path as text);
    // override it so images dropped onto the input are attached.
    input_->SetDropTarget(new FrameFileDropTarget(this));
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
#ifdef __WXOSX__
    // The capsule Send button fills its frame; keep it off the window edge.
    inputRow->AddSpacer(FromDIP(2));
#endif
    outer->Add(chipRow_, 0, wxEXPAND | wxTOP, 4);
    outer->Add(imageRow_, 0, wxEXPAND | wxTOP, 4);
    outer->Add(inputRow, 0, wxEXPAND);
    outer->Add(toolbarRow, 0, wxEXPAND | wxTOP, 4);

    auto* root = new wxBoxSizer(wxVERTICAL);
    // Left/right padding only: the top/bottom padding lives on the frame
    // sizer around the whole splitter, so the sashes don't overhang the
    // padded content.
    root->Add(outer, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(kEdgePad));
    panel->SetSizer(root);
    // Runs before the panel's default size handler lays out, so toolbar
    // visibility is settled by the time the sizer positions the toolbar.
    panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        UpdateToolbarFit();
        e.Skip();
    });

    // Editor — right pane of the inner splitter, hidden until the editor
    // toggle. A project file tree on the left and an editable text area on
    // the right, laid out side by side in a box sizer (no nested splitter,
    // so the tree stays a fixed width).
    editorPanel_ = new wxPanel(innerSplitter_);
    auto* editorSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* treePane = new wxPanel(editorPanel_);
    auto* treeSizer = new wxBoxSizer(wxVERTICAL);
    fileTree_ = new wxTreeCtrl(treePane, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_NO_LINES);
    {
        wxImageList* il = new wxImageList(16, 16, true);
        imgFolder_ = il->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER,
                                                      wxSize(16, 16)));
        imgFile_ = il->Add(wxArtProvider::GetBitmap(wxART_NORMAL_FILE,
                                                    wxART_OTHER, wxSize(16, 16)));
        fileTree_->AssignImageList(il);
    }
    treeSizer->Add(fileTree_, 1, wxEXPAND);
    treePane->SetSizer(treeSizer);
    // Fixed, non-resizable tree width.
    treePane->SetMinSize(wxSize(kFileTreeWidth, -1));
    treePane->SetMaxSize(wxSize(kFileTreeWidth, -1));

    auto* editPane = new wxPanel(editorPanel_);
    auto* editSizer = new wxBoxSizer(wxVERTICAL);
    codeEdit_ = new wxTextCtrl(editPane, wxID_ANY, "",
                               wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_RICH2 | wxTE_PROCESS_TAB);
    {
        // Same size as code blocks in the chat.
        const int editorPt = kCodeFontPt;
#ifdef __WXOSX__
        wxFont mono = MacMonospaceFont(editorPt);
#else
        wxFont mono = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        mono.SetFamily(wxFONTFAMILY_TELETYPE);
        mono.SetPointSize(editorPt);
#endif
        codeEdit_->SetFont(mono);
    }
    codeEdit_->Bind(wxEVT_TEXT, &ChatFrame::OnEditorTextChanged, this);
    codeEdit_->Bind(wxEVT_CHAR, &ChatFrame::OnEditorChar, this);
    codeEdit_->Bind(wxEVT_CONTEXT_MENU, &ChatFrame::OnEditorContextMenu, this);
    editSizer->Add(codeEdit_, 1, wxEXPAND);
    editPane->SetSizer(editSizer);

    highlightTimer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, &ChatFrame::OnHighlightTimer, this, highlightTimer_->GetId());

    treeRefreshTimer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, &ChatFrame::OnTreeRefreshTimer, this,
         treeRefreshTimer_->GetId());

    // Left/right padding matches the other panes; the tree and editor sit
    // flush against each other with no sash between them.
    editorSizer->Add(treePane, 0, wxEXPAND | wxLEFT, FromDIP(kEdgePad));
    editorSizer->Add(editPane, 1, wxEXPAND | wxRIGHT, FromDIP(kEdgePad));
    editorPanel_->SetSizer(editorSizer);
    editorPanel_->Hide();

    // Import viewer — left pane of splitter, hidden until hamburger toggle.
    importPanel_ = new wxPanel(splitter_);
    auto* importSizer = new wxBoxSizer(wxVERTICAL);

    // Empty state: centered "Load Session…" button.
    importEmptyView_ = new wxPanel(importPanel_);
    auto* emptySizer = new wxBoxSizer(wxVERTICAL);
    emptySizer->AddStretchSpacer(1);
    auto* loadBtn = new wxButton(importEmptyView_, wxID_ANY,
        wxString::FromUTF8("Load Session\xE2\x80\xA6"));
    emptySizer->Add(loadBtn, 0, wxALIGN_CENTER);
    emptySizer->AddStretchSpacer(1);
    importEmptyView_->SetSizer(emptySizer);
    loadBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxCommandEvent dummy;
        OnImport(dummy);
    });

    importCanvas_ = new ChatCanvas(importPanel_);
    importCanvas_->SetBgColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    importCanvas_->Hide();

    refLabel_ = new wxStaticText(importPanel_, wxID_ANY,
        wxString::FromUTF8("Referenced Session: None"));
    auto refFont = refLabel_->GetFont();
    refFont.SetPointSize(refFont.GetPointSize() - 1);
    refLabel_->SetFont(refFont);
    refLabel_->SetForegroundColour(wxColour(140, 140, 140));

    auto* instructionLabel = new wxStaticText(importPanel_, wxID_ANY,
        wxString::FromUTF8("Click a prompt to copy it to the current session."));
    auto instrFont = instructionLabel->GetFont();
    instrFont.SetPointSize(instrFont.GetPointSize() - 1);
    instructionLabel->SetFont(instrFont);
    instructionLabel->SetForegroundColour(wxColour(140, 140, 140));

    auto* bottomBox = new wxBoxSizer(wxVERTICAL);

    auto* refRow = new wxBoxSizer(wxHORIZONTAL);
    refRow->Add(refLabel_, 0, wxALIGN_CENTER_VERTICAL);

    auto* changeBtn = new wxButton(importPanel_, wxID_ANY,
        wxString::FromUTF8("load another\xE2\x80\xA6"),
        wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    changeBtn_ = changeBtn;
    // Match tool accent colour from ChatCanvas palette. Set before the font:
    // macOS only applies a button's text colour when SetFont/SetLabel rebuild
    // its title, so the reverse order leaves the link in the default colour.
    changeBtn->SetForegroundColour(importCanvas_->GetPalette().toolAccent);
    auto cf = changeBtn->GetFont();
    cf.SetPointSize(cf.GetPointSize() - 1);
    changeBtn->SetFont(cf);
    changeBtn->Hide();
    changeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxCommandEvent dummy;
        OnImport(dummy);
    });
    refRow->Add(changeBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

    bottomBox->Add(refRow, 0, wxLEFT | wxRIGHT | wxTOP, 4);
    bottomBox->Add(instructionLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

    importSizer->Add(importEmptyView_, 1, wxEXPAND);
    importSizer->Add(importCanvas_, 1, wxEXPAND);
    importSizer->Add(bottomBox, 0, wxEXPAND);
    auto* importRoot = new wxBoxSizer(wxVERTICAL);
    importRoot->Add(importSizer, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(kEdgePad));
    importPanel_->SetSizer(importRoot);
    importPanel_->Hide();  // hidden until split

    // Click on import canvas copies prompt to main input.
    importCanvas_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        wxPoint logical = importCanvas_->CalcUnscrolledPosition(
            e.GetPosition());
        BlockPos pos = importCanvas_->HitTestPublic(logical.x, logical.y);
        if (!pos.IsValid()) { e.Skip(); return; }
        const auto& blocks = importCanvas_->Blocks();
        if (pos.block < 0 || pos.block >= (int)blocks.size()) { e.Skip(); return; }
        if (blocks[pos.block].type == BlockType::UserPrompt) {
            input_->SetValue(blocks[pos.block].rawText);
        }
        e.Skip();
    });

    splitter_->Initialize(innerSplitter_);   // Right pane only at start
    innerSplitter_->Initialize(mainPanel_);  // Editor hidden at start

    innerSplitter_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING,
                         &ChatFrame::OnInnerSashChanging, this);

    auto* frameSizer = new wxBoxSizer(wxHORIZONTAL);
    // Top/bottom padding wraps the whole splitter so its sashes are inset
    // from the window edges instead of running edge-to-edge.
    frameSizer->Add(splitter_, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(kEdgePad));
    SetSizer(frameSizer);

    // Open the most recent session if one exists; otherwise seed one for the
    // default cwd so the dropdown always has at least one entry.
    { PERF_SCOPE("SessionStore::Init"); store_.Init(); }
    // Open the FTS5 memory index. Failure (e.g. read-only home) is silent;
    // grit_history_search returns "Memory index unavailable" in that case.
    { PERF_SCOPE("MemoryDB::Open"); memory_.Open(MemoryDB::DefaultPath()); }
    bool restored = false;
    { PERF_SCOPE("RestoreLastSession");
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
        { PERF_SCOPE("PersistActive:Save"); store_.Save(activeCwd_, history_); }
        store_.SetLastActiveCwd(activeCwd_);
    } else {
        RefreshSystemPromptAgents();
    }
    } // RestoreLastSession
    ChdirToCwd(activeCwd_);
    RefreshSessionChoice();
    PopulateEditorTree();
    // Some watcher backends need a running event loop before they initialise,
    // so defer setup until after OnInit returns. Harmless where it isn't
    // needed — it just starts one loop iteration later.
    CallAfter([this]() {
        if (destroying_.load()) return;
        SetupFsWatcher();
    });

    // Large sessions show a centered placeholder first and render the canvas
    // after that placeholder has actually painted (the canvas invokes the
    // callback from its first OnPaint). Small sessions are restored
    // synchronously, as before.
    canvas_->SetLoadingPaintCallback([this] { RestoreSession(); });
    if (HistoryIsLarge()) {
        canvas_->SetLoading(true);
    } else {
        RestoreCanvasFromHistory();
    }

    Bind(wxEVT_BUTTON, &ChatFrame::OnSend, this, ID_SEND);
    Bind(wxEVT_BUTTON, &ChatFrame::OnContinueQueue, this, ID_QUEUE_CONTINUE);
    Bind(wxEVT_BUTTON, &ChatFrame::OnClearQueue, this, ID_QUEUE_CLEAR);
    input_->Bind(wxEVT_KEY_DOWN, &ChatFrame::OnInputKey, this);
    // CHAR_HOOK fires at the frame before any focused child sees the key, so
    // Escape cancels regardless of where focus is (input, canvas, dropdowns).
    Bind(wxEVT_CHAR_HOOK, &ChatFrame::OnCharHook, this);
    Bind(wxEVT_CLOSE_WINDOW, &ChatFrame::OnClose, this);
    Bind(wxEVT_BUTTON, &ChatFrame::OnSettings, this, ID_SETTINGS);
    Bind(wxEVT_BUTTON, &ChatFrame::OnExport, this, ID_EXPORT);
    Bind(wxEVT_BUTTON, &ChatFrame::OnHamburger, this, ID_HAMBURGER);
    Bind(wxEVT_BUTTON, &ChatFrame::OnEditorToggle, this, ID_EDITOR);
    fileTree_->Bind(wxEVT_TREE_ITEM_EXPANDING, &ChatFrame::OnEditorTreeExpanding, this);
    fileTree_->Bind(wxEVT_TREE_ITEM_COLLAPSED, &ChatFrame::OnEditorTreeCollapsed, this);
    fileTree_->Bind(wxEVT_TREE_SEL_CHANGED, &ChatFrame::OnEditorTreeSelect, this);
    fileTree_->Bind(wxEVT_CONTEXT_MENU, &ChatFrame::OnEditorTreeContextMenu, this);
    Bind(wxEVT_FSWATCHER, &ChatFrame::OnFsWatcherEvent, this);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeNewFile, this, ID_TREE_NEW_FILE);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeNewFolder, this, ID_TREE_NEW_FOLDER);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeRename, this, ID_TREE_RENAME);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeShowInFiles, this, ID_TREE_SHOW_IN_FILES);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeRefresh, this, ID_TREE_REFRESH);
    Bind(wxEVT_MENU, &ChatFrame::OnTreeToggleHidden, this, ID_TREE_TOGGLE_HIDDEN);
    Bind(wxEVT_MENU, &ChatFrame::OnEditorSave, this, ID_EDITOR_SAVE);
    Bind(wxEVT_MENU, &ChatFrame::OnEditorSaveAs, this, ID_EDITOR_SAVE_AS);
    Bind(wxEVT_MENU, &ChatFrame::OnEditorReload, this, ID_EDITOR_RELOAD);
    Bind(wxEVT_MENU, &ChatFrame::OnEditorCloseFile, this, ID_EDITOR_CLOSE);
    Bind(wxEVT_MENU, &ChatFrame::OnEditorShowInFiles, this, ID_EDITOR_SHOW_IN_FILES);
    Bind(wxEVT_BUTTON, &ChatFrame::OnPlay, this, ID_PLAY);
    Bind(wxEVT_TOOL_BATCH_DONE, &ChatFrame::OnToolBatchDone, this);
    sessionChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnSessionChoice, this);
    modelChoice_->Bind(wxEVT_CHOICE, &ChatFrame::OnModelChoice, this);
    modelChoice_->Bind(wxEVT_CONTEXT_MENU, &ChatFrame::OnModelContextMenu, this);
    Bind(wxEVT_MENU,
         [this](wxCommandEvent&) { FetchRemoteModelsAsync(); },
         ID_MODEL_REFRESH);
    Bind(wxEVT_SYS_COLOUR_CHANGED,
         [this](wxSysColourChangedEvent& e) { ApplyTheme(); e.Skip(); });
    // The watcher needs a running event loop, so start it once one is up.
    CallAfter([this] { StartThemeWatcher(); });

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
            for (const auto& q : pendingQueue_) queue.push_back(q.text);
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
                EnqueueMessage(text);
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
        CallAfter([this]() {
            request_.Cancel();
            claudeProc_.Cancel();
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
            if (!SwitchToCwd(cwd))
                return {{"ok", false}, {"reason", "cancelled"}};
            return {{"ok", true}};
        });
    };
    cb.setModel = [this, guiSync](int idx) -> nlohmann::json {
        return guiSync([this, idx]() -> nlohmann::json {
            // `idx` is a dropdown row, as a user would pick it.
            if (idx < 0 || idx >= (int)modelChoice_->GetCount())
                return {{"ok", false}, {"reason", "out of range"}};
            currentModelIndex_ = ModelIndexForRow(idx, remoteModels_);
            modelChoice_->SetSelection(idx);
            Preferences::SetLastModelIndex(currentModelIndex_);
            return {{"ok", true}, {"modelIndex", currentModelIndex_}};
        });
    };
    cb.getPreferences = [guiSync]() -> nlohmann::json {
        return guiSync([]() -> nlohmann::json {
            return {
                {"modelIndex", Preferences::GetLastModelIndex()},
                {"hasDeepseekKey",
                 Preferences::HasApiKey(Preferences::Provider::DeepSeek)},
                {"enableGritHistory", Preferences::GetEnableGritHistory()},
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
                RefreshSystemPromptAgents();
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
    cb.exportSession = [this, guiSync](const std::string& path)
        -> nlohmann::json {
        return guiSync([this, path]() -> nlohmann::json {
            nlohmann::json j;
            j["version"] = HistoryHasImages(history_) ? 2 : 1;
            j["messages"] = history_;
            std::string err;
            bool ok = false;
            if (HistoryHasImages(history_)) {
                ok = ExportToZip(path, j, CollectImageRefs(history_), err);
            } else {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                ok = (bool)f;
                if (ok) f << j.dump(2, ' ', false,
                                    nlohmann::json::error_handler_t::replace);
                else err = "cannot write file";
            }
            if (!ok) return {{"ok", false}, {"error", err}};
            return {{"ok", true}, {"messageCount", (int)history_.size()}};
        });
    };
    cb.importSession = [this, guiSync](const std::string& path)
        -> nlohmann::json {
        return guiSync([this, path]() -> nlohmann::json {
            nlohmann::json j;
            std::string err;
            if (!ImportSessionFile(path, j, err))
                return {{"ok", false}, {"error", err}};
            if (!j.contains("messages") || !j["messages"].is_array())
                return {{"ok", false}, {"error", "no messages array"}};

            importedMessages_.clear();
            int promptCount = 0;
            for (const auto& m : j["messages"]) {
                if (m.is_object()) {
                    importedMessages_.push_back(m);
                    if (m.value("role", std::string{}) == "user") ++promptCount;
                }
            }

            // Store filename for display.
            auto slash = path.rfind('/');
            importedFileName_ = wxString::FromUTF8(
                slash != std::string::npos ? path.substr(slash + 1) : path);

            // Trigger the import dialog on the GUI thread.
            CallAfter([this]() { ShowImportDialog(); });

            return {{"ok", true}, {"promptCount", promptCount}};
        });
    };
    cb.play = [this, guiSync]() -> nlohmann::json {
        return guiSync([this]() -> nlohmann::json {
            if (streaming_) return {{"started", false}, {"reason", "streaming"}};
            const bool configured = RunConfigStore::Get(activeCwd_).has_value();
            wxCommandEvent ev(wxEVT_BUTTON, ID_PLAY);
            OnPlay(ev);
            return {{"started", true}, {"configured", configured}};
        });
    };
    mcp_.Start(std::move(cb));

    SetDropTarget(new FrameFileDropTarget(this));

    input_->SetFocus();
    SetMinClientSize(wxSize(kMainMinClientW, 400));

#ifdef __WXOSX__
    // Tahoe look: native styling only, the layout above stays shared.
    ApplyMacStyle({this, canvas_, sendBtn_, importPanel_});
#endif

    // Discover current DeepSeek model ids (e.g. a newly shipped flash) and
    // merge them into the dropdown. No-op when no API key is configured.
    FetchRemoteModelsAsync();
}

void ChatFrame::RestoreSession() {
    PERF_SCOPE("RestoreSession");
    RestoreCanvasFromHistory();
    canvas_->SetLoading(false);
    canvas_->Refresh();
}

void ChatFrame::AddDroppedFiles(const wxArrayString& files) {
    for (const auto& f : files) {
        std::string path = f.ToStdString(wxConvUTF8);
        std::string mime = MimeForImageFile(path);
        if (mime.empty()) continue;

        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string bytes = ss.str();
        if (bytes.empty() || bytes.size() > 32u * 1024u * 1024u) continue;

        std::string hash = ImageStore::Save(bytes, mime);
        if (hash.empty()) continue;

        PendingImage pi;
        pi.hash = hash;
        pi.mime = mime;
        pi.name = wxFileName(path).GetFullName().ToStdString(wxConvUTF8);
        pi.path = wxString::FromUTF8(ImageStore::PathFor(hash, mime));
        pendingImages_.push_back(std::move(pi));
    }
    RebuildImageRow();
}

void ChatFrame::RebuildImageRow() {
    imageSizer_->Clear(true);  // destroy old thumbnails and clear sizer items

    for (size_t i = 0; i < pendingImages_.size(); ++i) {
        wxImage img;
        if (!img.LoadFile(pendingImages_[i].path)) continue;
        auto* item = new ThumbnailItem(
            imageRow_, SquareThumbnail(img, 56),
            [this, hash = pendingImages_[i].hash] { RemovePendingImageByHash(hash); });
        imageSizer_->Add(item, 0, wxALL, 2);
    }

    bool any = !pendingImages_.empty();
    imageRow_->Show(any);
    imageRow_->GetContainingSizer()->Layout();
    if (auto* fs = GetSizer()) fs->Layout();
}

void ChatFrame::RemovePendingImageByHash(const std::string& hash) {
    auto it = std::find_if(pendingImages_.begin(), pendingImages_.end(),
                           [&](const PendingImage& p) { return p.hash == hash; });
    if (it == pendingImages_.end()) return;
    pendingImages_.erase(it);
    RebuildImageRow();
}

bool ChatFrame::TryPasteImage() {
    if (!wxTheClipboard->Open()) return false;

    wxBitmapDataObject bmpObj;
    bool hasImage = wxTheClipboard->GetData(bmpObj);
    wxTheClipboard->Close();

    if (!hasImage || !bmpObj.GetBitmap().IsOk()) return false;

    wxImage img = bmpObj.GetBitmap().ConvertToImage();
    if (!img.IsOk()) return false;

    wxString tmp = wxFileName::CreateTempFileName("gritimg");
    if (tmp.empty()) return true;  // consumed the paste even if we can't save

    if (!img.SaveFile(tmp, wxBITMAP_TYPE_PNG)) {
        wxRemoveFile(tmp);
        return true;
    }

    std::ifstream in(tmp.ToStdString(wxConvUTF8), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();
    wxRemoveFile(tmp);

    std::string bytes = ss.str();
    if (bytes.empty()) return true;

    std::string hash = ImageStore::Save(bytes, "image/png");
    if (hash.empty()) return true;

    PendingImage pi;
    pi.hash = hash;
    pi.mime = "image/png";
    pi.name = "pasted.png";
    pi.path = wxString::FromUTF8(ImageStore::PathFor(hash, "image/png"));
    pendingImages_.push_back(std::move(pi));
    RebuildImageRow();
    return true;
}

bool ChatFrame::HistoryIsLarge() const {
    if (history_.size() > 200) return true;
    size_t chars = 0;
    for (const auto& m : history_) {
        if (!m.is_object()) continue;
        if (m.contains("content") && m["content"].is_string())
            chars += m["content"].get<std::string>().size();
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto& tc : m["tool_calls"]) {
                if (tc.is_object() && tc.contains("function")
                    && tc["function"].is_object()
                    && tc["function"].contains("arguments")
                    && tc["function"]["arguments"].is_string())
                    chars += tc["function"]["arguments"].get<std::string>().size();
            }
        }
    }
    return chars > 250000;
}

void ChatFrame::RestoreCanvasMaybeDeferred() {
    canvas_->Clear();
    if (HistoryIsLarge()) {
        // The canvas fires onLoadingPainted_ (set once in the constructor)
        // from its first paint of the placeholder, which runs RestoreSession.
        // No direct CallAfter here — that would run RestoreSession twice.
        canvas_->SetLoading(true);
    } else {
        RestoreCanvasFromHistory();
    }
}

ChatFrame::~ChatFrame() {
    // Signal MCP's guiSync callers to bail out of their future.get() polls
    // before we stop the server — otherwise mcp_.Stop()'s join would deadlock
    // on a thread waiting for a CallAfter that can no longer fire.
    destroying_.store(true);
    mcp_.Stop();
    request_.Cancel();
    // Kills a running Claude Code turn and joins its worker, so no callback
    // can land on a half-destroyed frame.
    claudeProc_ = ClaudeAgentProcess();
    delete fileWatcher_;  // stop filesystem watches (own their thread)
    fileWatcher_ = nullptr;
    delete themeWatcher_;
    themeWatcher_ = nullptr;
    themeTimer_.Stop();
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
    if (persistWorker_.joinable()) persistWorker_.join();
    if (remoteModelsWorker_.joinable()) remoteModelsWorker_.join();
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
        case BlockType::Image:      return "image";
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
        if (m.contains("model")) e["model"] = m["model"];
        out.push_back(std::move(e));
    }
    return out;
}

void ChatFrame::OnClose(wxCloseEvent& evt) {
    // currentToolToken_ is non-null exactly while the tool-dispatch worker is
    // running. Closing during a tool batch must veto + cancel just like
    // closing during an in-flight HTTP request — otherwise the worker would
    // outlive the frame and post wxQueueEvent to a dangling `this`.
    if (request_.IsActive() || currentToolToken_ || claudeProc_.IsActive()) {
        quitRequested_ = true;
        Hide();
        RequestCancel();
        // OnStreamDone / OnToolBatchDone / OnClaudeDone check quitRequested_
        // and re-fire Close() once their phase finishes.
        evt.Veto();
    } else {
        if (!MaybeSaveEditor()) {
            evt.Veto();
            return;
        }
        PersistActive();
        evt.Skip();
    }
}

void ChatFrame::OnInputKey(wxKeyEvent& e) {
    // Ctrl+V with an image on the clipboard attaches it instead of pasting
    // text; with text we fall through to the control's normal paste.
    if (e.ControlDown() && e.GetKeyCode() == 'V') {
        if (TryPasteImage()) return;
        e.Skip();
        return;
    }
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
    int key = e.GetKeyCode();
    if (e.GetModifiers() == wxMOD_CONTROL && (key == 'S' || key == 's')) {
        SaveEditorFile();
        return;
    }
    e.Skip();
}

void ChatFrame::RequestCancel() {
    // Idempotent on a finished request — Cancel just sets an atomic.
    request_.Cancel();
    // A Claude turn gets SIGINT, so Claude Code ends it cleanly.
    claudeProc_.Cancel();
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
    // Note: Play-button worker (currentPlayToken_) is intentionally NOT
    // cancelled here. Escape cancels AI-initiated tool calls, not user-
    // initiated Play runs. If the user started a dev server via Play,
    // they want it to keep running.
}

void ChatFrame::StartThemeWatcher() {
    themeTimer_.SetOwner(&themeEvents_);
    themeEvents_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        // A switch may replace directories under the watch; re-arm it.
        if (themeWatcher_) {
            themeWatcher_->RemoveAll();
            wxString dir = omarchy::WatchDir();
            if (!dir.empty()) themeWatcher_->Add(wxFileName::DirName(dir));
        }
        ApplyTheme();
    });
    wxString dir = omarchy::WatchDir();
    if (dir.empty()) return;  // not an Omarchy system
    themeWatcher_ = new wxFileSystemWatcher();
    themeWatcher_->SetOwner(&themeEvents_);
    themeEvents_.Bind(wxEVT_FSWATCHER, [this](wxFileSystemWatcherEvent&) {
        themeTimer_.StartOnce(400);
    });
    themeWatcher_->Add(wxFileName::DirName(dir));
}

void ChatFrame::ApplyTheme() {
    omarchy::Reload();
    canvas_->ApplyTheme();
    importCanvas_->SetBgColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    importCanvas_->ApplyTheme();
    changeBtn_->SetForegroundColour(importCanvas_->GetPalette().toolAccent);
    ReloadToolbarIcons();
    if (!pendingQueue_.empty()) RebuildChips();
    // Re-colour the open file with the new syntax palette.
    if (codeEdit_ && !editorFilePath_.empty() && highlightTimer_)
        highlightTimer_->StartOnce(1);
    Refresh();
}

void ChatFrame::ReloadToolbarIcons() {
    const wxSize kIconSize(20, 20);
    wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    settingsBtn_->SetBitmap(LoadThemedSvgIcon("settings.svg", kIconSize, accent));
    playBtn_->SetBitmap(LoadThemedSvgIcon("play.svg", kIconSize, accent));
    exportBtn_->SetBitmap(LoadThemedSvgIcon("export.svg", kIconSize, accent));
    hamburgerBtn_->SetBitmap(LoadThemedSvgIcon("hamburger.svg", kIconSize, accent));
    editorBtn_->SetBitmap(LoadThemedSvgIcon("editor.svg", kIconSize, accent));
}

void ChatFrame::RefreshSessionChoice() {
    PERF_SCOPE("RefreshSessionChoice");
    sessionChoice_->Clear();
    sessionCwds_.clear();
    sessionChoice_->Append(wxString::FromUTF8("New Session\xE2\x80\xA6"));
    int activeRow = -1;
    for (const auto& e : store_.List()) {
        sessionChoice_->Append(DisplayPath(e.cwd));
        sessionCwds_.push_back(e.cwd);
        if (e.cwd == activeCwd_) activeRow = (int)sessionCwds_.size();
    }
    if (activeRow < 1 && sessionCwds_.size() >= 1) activeRow = 1;
    if (activeRow >= 1) sessionChoice_->SetSelection(activeRow);
    if (activeRow >= 1) {
        sessionChoice_->SetToolTip(
            wxString::FromUTF8(sessionCwds_[activeRow - 1]));
    }
}

void ChatFrame::OnSessionChoice(wxCommandEvent& evt) {
    int sel = evt.GetSelection();
    if (sel == 0) {
        // "New Session…" sentinel — snap back.
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
    if (!SwitchToCwd(target)) {
        // Cancelled (unsaved editor changes): snap back to the active session.
        for (int i = 0; i < (int)sessionCwds_.size(); ++i) {
            if (sessionCwds_[i] == activeCwd_) {
                sessionChoice_->SetSelection(i + 1);
                break;
            }
        }
    }
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

    if (!MaybeSaveEditor()) return;  // cancelled: keep current session + editor
    ClearEditorState();

    PersistActive();
    activeCwd_ = chosen;
    std::vector<nlohmann::json> hist;
    if (store_.Load(activeCwd_, hist)) {
        // Existing session for this folder — restore it.
        history_ = std::move(hist);
        RefreshSystemPromptAgents();
    } else {
        // Brand new folder: seed a fresh system prompt.
        history_.clear();
        SeedSystemPrompt();
        store_.Save(activeCwd_, history_);
    }
    historyCompactBaseCount_ = 0;  // fresh compaction gate for the new session
    store_.SetLastActiveCwd(activeCwd_);
    ChdirToCwd(activeCwd_);  // keep the process cwd in sync with activeCwd_
    RestoreCanvasMaybeDeferred();
    RefreshSessionChoice();
    PopulateEditorTree();
    SetupFsWatcher();
}

bool ChatFrame::SwitchToCwd(const std::string& cwd) {
    PERF_SCOPE("SwitchToCwd");
    if (cwd == activeCwd_) return true;
    if (!MaybeSaveEditor()) return false;  // cancelled: keep current session
    ClearEditorState();

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
    } else {
        RefreshSystemPromptAgents();
    }
    historyCompactBaseCount_ = 0;  // fresh compaction gate for the new session
    store_.SetLastActiveCwd(activeCwd_);
    ChdirToCwd(activeCwd_);
    RestoreCanvasMaybeDeferred();
    RefreshSessionChoice();
    PopulateEditorTree();
    SetupFsWatcher();
    return true;
}

void ChatFrame::PersistActive() {
    PERF_SCOPE("PersistActive");
    if (activeCwd_.empty()) return;

    // Snapshot everything the worker needs up front. The worker must not
    // read history_/activeCwd_ while the GUI thread keeps mutating them.
    std::string cwd = activeCwd_;
    std::vector<nlohmann::json> msgs = history_;
    std::string ts = SessionStore::NowIso();

    // The index update is tiny and touches entries_/sessions.json - keep it on
    // the GUI thread so SessionStore stays single-threaded. The heavy parts
    // (session-file JSON dump + write, and the FTS5 reindex) run on the worker.
    store_.UpdateIndex(cwd, ts);

    // Serialize with any previous persist so we don't reuse the std::thread
    // slot while it's still joinable. Normally it finished long ago, so this
    // join is instant; it only blocks if the user switches sessions rapidly.
    if (persistWorker_.joinable()) persistWorker_.join();
    persistWorker_ = std::thread([this, cwd, msgs = std::move(msgs), ts]() {
        store_.WriteSessionFile(cwd, msgs, ts);
        if (memory_.IsOpen()) {
            memory_.RebuildSession(SessionStore::IdForCwd(cwd), cwd, msgs, ts);
        }
    });
}

void ChatFrame::SeedSystemPrompt() {
    std::string content = BaseSystemPrompt();
    std::string agents = LoadAgentsInstructions(activeCwd_);
    if (!agents.empty()) content += "\n\n" + agents;
    history_.push_back({{"role", "system"}, {"content", std::move(content)}});
}

void ChatFrame::RefreshSystemPromptAgents() {
    // Rebuild history_[0]'s content from the static base + current AGENTS.md.
    // This re-injects project instructions into an EXISTING session whose
    // system message was seeded before AGENTS.md existed (or changed).
    if (history_.empty()) return;
    auto& m = history_[0];
    if (!m.is_object() || m.value("role", std::string{}) != "system") return;
    std::string agents = LoadAgentsInstructions(activeCwd_);
    lastAgentsContent_ = agents;
    std::string content = BaseSystemPrompt();
    if (!agents.empty()) content += "\n\n" + agents;
    m["content"] = std::move(content);
}

void ChatFrame::RestoreCanvasFromHistory() {
    PERF_SCOPE("RestoreCanvasFromHistory");
    canvas_->BeginBatch();
    ModelSwitchTracker modelTracker;
    // Walk the message list and re-emit blocks. Tool call/result pairs are
    // stitched back together: an assistant message's tool_calls are matched
    // against the immediately-following "tool" messages by tool_call_id.
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto& m = history_[i];
        // Compacted messages are still part of the durable transcript.
        // They are hidden from the MODEL view (BuildModelView), but MUST be
        // rendered here so the user sees the full session, not just the
        // post-compaction working set.
        std::string role = m.value("role", std::string{});

        if (role == "user") {
            if (!m.contains("content") || !m["content"].is_string()) continue;
            if (IsUserTurn(m)) {
                wxString notice = modelTracker.Next(m, /*announceFirst=*/false);
                if (!notice.IsEmpty()) canvas_->AddBlock(NoticeBlock(notice));
            }
            wxString text = wxString::FromUTF8(m["content"].get<std::string>());
            Block ub;
            ub.type = BlockType::UserPrompt;
            ub.rawText = text;
            InlineRun r; r.text = text;
            ub.runs.push_back(r);
            ub.visibleText = text;
            canvas_->AddBlock(std::move(ub));

            if (m.contains("images") && m["images"].is_array()) {
                for (const auto& img : m["images"]) {
                    if (!img.is_object()) continue;
                    EmitImageBlock(img.value("sha256", std::string{}),
                                   img.value("mime", "image/png"),
                                   img.value("name", std::string{}));
                }
            }
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
    canvas_->EndBatch();
}

void ChatFrame::OnModelChoice(wxCommandEvent& evt) {
    int sel = evt.GetSelection();
    if (sel < 0) return;
    currentModelIndex_ = ModelIndexForRow(sel, remoteModels_);
    Preferences::SetLastModelIndex(currentModelIndex_);
}

void ChatFrame::RebuildModelChoice() {
    if (!modelChoice_) return;
    modelChoice_->Clear();
    modelChoice_->Append("Kilo Free");
    // DeepSeek models come from one source only: the live list when the
    // fetch succeeded, the hardcoded fallback otherwise. They are never
    // concatenated, so a renamed model can't show up twice.
    for (const auto& id : DeepseekModels(remoteModels_))
        modelChoice_->Append(RemoteModelLabel(id));
    for (const auto& c : kClaudeModels) modelChoice_->Append(c.label);

    // currentModelIndex_ is the *desired* selection and may name a dynamic
    // entry that hasn't arrived yet (or just disappeared). Clamp only for
    // display so the dropdown never renders unselected; routing falls back
    // to Kilo Free for an out-of-range index.
    int sel = ModelRowForIndex(currentModelIndex_, remoteModels_);
    if (sel < 0 || sel >= (int)modelChoice_->GetCount()) sel = 0;
    modelChoice_->SetSelection(sel);
}

void ChatFrame::FetchRemoteModelsAsync() {
    // Serialize with any in-flight fetch (startup and settings-close can both
    // fire). The request is short (5 s connect / 5 s idle), so joining here
    // only blocks if a fetch is genuinely still running.
    if (remoteModelsWorker_.joinable()) remoteModelsWorker_.join();

    wxString key = Preferences::GetApiKey(Preferences::Provider::DeepSeek);
    if (key.IsEmpty()) {
        // No key: nothing to fetch. The hardcoded fallback list shows.
        return;
    }

    std::string url = "https://api.deepseek.com/models";
    std::string bearer = "Bearer " + std::string(key.utf8_str());
    ChatFrame* self = this;
    WebCancelToken* token = &remoteModelsCancel_;
    token->cancelled.store(false);
    remoteModelsWorker_ = std::thread([self, url, bearer, token]() {
        WebRequestSpec spec;
        spec.url = url;
        spec.method = "GET";
        spec.connectTimeoutSeconds = 5;
        spec.idleTimeoutSeconds = 5;
        spec.headers.push_back({"Authorization", bearer});
        spec.headers.push_back({"Accept", "application/json"});
        WebResponse resp = RequestSync(std::move(spec), token);

        std::vector<std::string> models;
        bool ok = false;
        if (resp.ok) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                for (const auto& m : j.value("data", nlohmann::json::array())) {
                    if (!m.is_object()) continue;
                    std::string id = m.value("id", std::string{});
                    // Only remote DeepSeek chat models: skip vision models
                    // (they're used internally by the image tool, not offered
                    // as a chat model). This list REPLACES the hardcoded
                    // DeepSeek fallback, so there is no dedup here.
                    if (id.rfind("deepseek-", 0) == 0
                        && id.find("vision") == std::string::npos) {
                        models.push_back(std::move(id));
                    }
                }
                ok = true;
            } catch (const std::exception&) {
                // Parse failure: ok stays false, fall through.
            }
        }

        self->CallAfter([self, models = std::move(models), ok]() mutable {
            if (self->destroying_.load()) return;
            // Replace remoteModels_ only on a successful fetch. On failure
            // (HTTP error or unparseable body) leave it untouched so the last
            // successfully-fetched list remains; if it's empty, the hardcoded
            // fallback shows.
            if (ok) self->OnRemoteModelsFetched(std::move(models));
        });
    });
}

void ChatFrame::OnRemoteModelsFetched(std::vector<std::string> models) {
    remoteModels_ = std::move(models);
    RebuildModelChoice();
}

void ChatFrame::OnModelContextMenu(wxContextMenuEvent&) {
    wxMenu menu;
    wxMenuItem* refresh = menu.Append(ID_MODEL_REFRESH,
                                      wxString::FromUTF8("Refresh model list…"));
    if (Preferences::GetApiKey(Preferences::Provider::DeepSeek).IsEmpty())
        refresh->Enable(false);
    PopupMenu(&menu);
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
        userBlock.runs.push_back({userMsg, false, false, false, {}});
        canvas_->AddBlock(std::move(userBlock));

        // Run the command on a background thread. Use a dedicated playWorker_
        // so a long-running dev server doesn't block the tool dispatch worker
        // — otherwise the model can't execute tool calls while the server runs.
        auto token = std::make_shared<ToolCancelToken>();
        // If a previous Play run is still alive (e.g. a dev server), cancel
        // it so join() returns quickly instead of blocking the GUI forever.
        if (auto old = currentPlayToken_) {
            old->cancelled.store(true);
#ifndef _WIN32
            int pgid = old->activePgid.load();
            if (pgid > 0) ::kill(-pgid, SIGTERM);
#endif
        }
        if (playWorker_.joinable()) playWorker_.join();
        currentPlayToken_ = token;
        playWorker_ = std::thread([this, cmd = cfg->command, token, cwd = activeCwd_]() {
            // Wrap the stored command with a cd to the project directory so
            // relative paths work regardless of the process's current cwd.
            // This is the key fix for the Play button: the model stores
            // commands like "cmake --build build && ./build/gritcode" which
            // only work from the project root. The wrapping cd ensures the
            // shell is in the right place before executing.
#ifdef _WIN32
            // cmd.exe understands neither single quotes nor a bare `cd` across
            // drives, so use `cd /d "..."`.
            std::string wrapped = "cd /d \"" + cwd + "\" && " + cmd;
#else
            // POSIX-safe single-quote escaping for the cwd path.
            std::string qcwd;
            qcwd += '\'';
            for (char ch : cwd) {
                if (ch == '\'') qcwd += "'\\''";
                else qcwd += ch;
            }
            qcwd += '\'';
            std::string wrapped = "cd " + qcwd + " && " + cmd;
#endif
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
            "3. Test it with bash - debug until it succeeds.\n"
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
            "\n\nFor compiled projects, just build and run - no browser needed."
            "[/hidden]";

        // The canvas shows only the visible prompt; the model also gets the
        // hidden instructions. StartTurn routes it to whichever model is
        // selected, Claude included.
        StartTurn(visible, {}, hidden);
    }
}
void ChatFrame::OnSettings(wxCommandEvent&) {
    SettingsDialog dlg(this);
    dlg.ShowModal();
    // The API key may have been added/rotated/removed — refresh the model
    // catalog against the new credential (no-op if there is still no key).
    FetchRemoteModelsAsync();
}

void ChatFrame::OnCanvasLink(wxCommandEvent& e) {
    if (e.GetString() == "gritcode://settings") {
        wxCommandEvent ev;
        OnSettings(ev);
    }
}

void ChatFrame::OnHamburger(wxCommandEvent&) {
    // The chat pane keeps its current width; only the window grows/shrinks.
    int centerW = mainPanel_->GetSize().x;
    if (splitter_->IsSplit()) {
        splitter_->Unsplit(importPanel_);
        importPanel_->Hide();
    } else {
        importPanel_->Show();
        splitter_->SplitVertically(importPanel_, innerSplitter_, kImportPaneWidth);
    }
    SyncPanelSizing(centerW);
}

void ChatFrame::OnEditorToggle(wxCommandEvent&) {
    int centerW = mainPanel_->GetSize().x;
    if (innerSplitter_->IsSplit()) {
        // Remember the editor's current width (it may have changed via window
        // resize) so toggling it back open restores the same width.
        editorPaneW_ = editorPanel_->GetSize().x;
        innerSplitter_->Unsplit(editorPanel_);
        editorPanel_->Hide();
        SyncPanelSizing(centerW);
    } else {
        editorPanel_->Show();
        // The agent (or an external tool) may have created/deleted files since
        // the tree was last populated — re-read disk before the pane appears.
        ReloadTreeKeepExpanded();
        CheckEditorFileChangedOnDisk();
        // Grow the window first so the inner splitter can be split with the
        // chat pane at its exact current width. Splitting first would force a
        // clamped transient sash, and with default gravity the chat pane
        // would then keep that clamped width instead of its real one.
        int importW = splitter_->IsSplit()
                    ? kImportPaneWidth + splitter_->GetSashSize() : 0;
        int editorW = editorPaneW_ + innerSplitter_->GetSashSize();
        SetClientSize(wxSize(centerW + importW + editorW, GetClientSize().y));
        Layout();
        splitter_->UpdateSize();
        innerSplitter_->UpdateSize();
        innerSplitter_->SplitVertically(mainPanel_, editorPanel_, centerW);
        SyncPanelSizing(centerW);
    }
    UpdateWindowTitle();
}

void ChatFrame::SyncPanelSizing(int centerW) {
    // Import pane width includes the splitter sash, and the editor counts its
    // (possibly user-adjusted) width plus the inner sash. The target window
    // width is the fixed chat width plus whatever panes are visible, so
    // toggling never resizes the chat pane.
    int importW = splitter_->IsSplit()
                ? kImportPaneWidth + splitter_->GetSashSize() : 0;
    int editorW = innerSplitter_->IsSplit()
                ? editorPaneW_ + innerSplitter_->GetSashSize() : 0;
    // Minimum pane widths use each splitter's own minimum pane size so the
    // editor can shrink (default gravity makes the right pane absorb resizes).
    int importMinW = splitter_->IsSplit()
                ? splitter_->GetMinimumPaneSize() + splitter_->GetSashSize() : 0;
    int editorMinW = innerSplitter_->IsSplit()
                ? innerSplitter_->GetMinimumPaneSize() + innerSplitter_->GetSashSize() : 0;

    SetMinClientSize(wxSize(kMainMinClientW + importMinW + editorMinW, 400));
    SetClientSize(wxSize(centerW + importW + editorW, GetClientSize().y));
    Layout();
    splitter_->UpdateSize();
    innerSplitter_->UpdateSize();
}

void ChatFrame::UpdateToolbarFit() {
    if (!toolbarRow_) return;
    int w = mainPanel_->GetClientSize().x;
    auto fit = [&](wxWindow* win, int minW) {
        if (win) toolbarRow_->Show(win, w >= minW);
    };
    fit(sessionLabel_, kToolbarLabelsW);
    fit(modelLabel_, kToolbarLabelsW);
    fit(settingsBtn_, kToolbarSettingsW);
    fit(debugBtn_, kToolbarSettingsW);
    fit(exportBtn_, kToolbarExportW);
    fit(modelChoice_, kToolbarModelW);
    // Without the model dropdown the session dropdown takes the freed room:
    // the stretch spacer stops stretching, and the dropdown may shrink further.
    // Next to the model dropdown it keeps a readable width.
    bool modelShown = w >= kToolbarModelW;
    sessionChoice_->SetMinSize(FromDIP(wxSize(modelShown ? 160 : 100, -1)));
    for (auto* item : toolbarRow_->GetChildren())
        if (item->IsSpacer()) item->SetProportion(modelShown ? 8 : 0);
}

void ChatFrame::OnInnerSashChanging(wxSplitterEvent& e) {
    // Fires only while the user drags the chat|editor sash (programmatic sash
    // moves never send SASH_POS_CHANGING), so it is safe to record the editor
    // width chosen so a later toggle reopens it at that width.
    editorPaneW_ = innerSplitter_->GetClientSize().x
                 - e.GetSashPosition() - innerSplitter_->GetSashSize();
    e.Skip();
}

void ChatFrame::PopulateEditorTree() {
    if (!fileTree_) return;
    fileTree_->DeleteAllItems();
    if (activeCwd_.empty()) return;
    wxString root = wxString::FromUTF8(activeCwd_);
    auto* rootData = new FileTreeItemData(root, true);
    wxTreeItemId rootItem = fileTree_->AddRoot(
        wxFileName(root).GetFullName(), imgFolder_, -1, rootData);
    PopulateTreeDir(rootItem, root);
    // Root is hidden (TR_HIDE_ROOT); its children are the top-level entries.
}

void ChatFrame::PopulateTreeDir(wxTreeItemId parent, const wxString& path) {
    if (!fileTree_) return;
    fileTree_->DeleteChildren(parent);

    wxDir dir;
    if (!dir.Open(path)) return;

    struct Entry {
        wxString name;
        bool isDir;
    };
    std::vector<Entry> entries;
    wxString name;
    // wxDir excludes dot-entries (hidden files AND folders) unless
    // wxDIR_HIDDEN is passed — so enable it when the toggle is on.
    int flags = wxDIR_DIRS | wxDIR_FILES;
    if (showHidden_) flags |= wxDIR_HIDDEN;
    bool cont = dir.GetFirst(&name, wxEmptyString, flags);
    while (cont && entries.size() < 500) {
        // Skip dotfiles unless "Show Hidden Files" is ticked.
        if (!name.empty() && (showHidden_ || name[0] != wxT('.'))) {
            wxString full = path + wxFILE_SEP_PATH + name;
            entries.push_back({name, wxDirExists(full)});
        }
        cont = dir.GetNext(&name);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;  // dirs first
        return a.name.Lower() < b.name.Lower();
    });

    for (const auto& e : entries) {
        wxString full = path + wxFILE_SEP_PATH + e.name;
        auto* data = new FileTreeItemData(full, e.isDir);
        int img = e.isDir ? imgFolder_ : imgFile_;
        wxTreeItemId item = fileTree_->AppendItem(parent, e.name, img, -1, data);
        if (e.isDir) fileTree_->SetItemHasChildren(item, true);
    }
}

void ChatFrame::ReloadTreeKeepExpanded() {
    if (!fileTree_) return;

    // Snapshot the expanded directories and current selection, rebuild from
    // disk, then restore both (where the paths still exist).
    std::vector<wxString> expanded;
    CollectExpandedPaths(fileTree_->GetRootItem(), expanded);
    wxString selPath = GetSelectedTreePath();

    PopulateEditorTree();

    std::set<wxString> populated;
    if (!activeCwd_.empty()) populated.insert(wxString::FromUTF8(activeCwd_));
    for (const auto& p : expanded) ExpandPathTo(p, populated);

    if (!selPath.empty()) {
        wxTreeItemId it = FindTreeItemByPath(fileTree_->GetRootItem(), selPath);
        if (it.IsOk()) {
            // Selecting a file here must not behave like a user click (open
            // it / prompt to save). Suppress the SEL_CHANGED handler.
            treeSelectionRestoring_ = true;
            fileTree_->SelectItem(it);
            treeSelectionRestoring_ = false;
        }
    }
}

void ChatFrame::CollectExpandedPaths(wxTreeItemId parent,
                                     std::vector<wxString>& out) {
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = fileTree_->GetFirstChild(parent, cookie);
         c.IsOk(); c = fileTree_->GetNextChild(parent, cookie)) {
        if (fileTree_->IsExpanded(c)) {
            auto* d = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(c));
            if (d) out.push_back(d->path);
            CollectExpandedPaths(c, out);
        }
    }
}

wxString ChatFrame::GetSelectedTreePath() const {
    if (!fileTree_) return wxString();
    wxTreeItemId sel = fileTree_->GetSelection();
    auto* d = sel.IsOk()
        ? dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(sel)) : nullptr;
    return d ? d->path : wxString();
}

void ChatFrame::ExpandPathTo(const wxString& path, std::set<wxString>& populated) {
    if (!fileTree_ || activeCwd_.empty() || path.empty()) return;
    wxString root = wxString::FromUTF8(activeCwd_);
    if (path == root || !path.StartsWith(root)) return;

    wxTreeItemId cur = fileTree_->GetRootItem();
    if (!cur.IsOk()) return;

    wxString rel = path.Mid(root.size());
    wxStringTokenizer tok(rel, wxFILE_SEP_PATH);
    while (tok.HasMoreTokens()) {
        wxString seg = tok.GetNextToken();
        if (seg.empty()) continue;

        // Load + expand `cur` once, before searching for the next component.
        // `populated` keeps a second ExpandPathTo (for a sibling branch) from
        // re-running PopulateTreeDir and collapsing a sibling we just expanded.
        auto* d = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(cur));
        if (d && d->isDir && !populated.count(d->path)) {
            PopulateTreeDir(cur, d->path);
            populated.insert(d->path);
        }
        // The hidden root is already "expanded" (its children are always
        // visible); expanding it explicitly is harmless but pointless.
        if (d && d->isDir && cur != fileTree_->GetRootItem())
            fileTree_->Expand(cur);

        wxTreeItemIdValue cookie;
        wxTreeItemId next;
        for (wxTreeItemId c = fileTree_->GetFirstChild(cur, cookie);
             c.IsOk(); c = fileTree_->GetNextChild(cur, cookie)) {
            if (fileTree_->GetItemText(c) == seg) { next = c; break; }
        }
        if (!next.IsOk()) return;  // path vanished on disk
        cur = next;
    }

    // `cur` is the final directory (expanded paths are directories): load and
    // expand it too.
    auto* d = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(cur));
    if (d && d->isDir && !populated.count(d->path)) {
        PopulateTreeDir(cur, d->path);
        populated.insert(d->path);
    }
    if (d && d->isDir) fileTree_->Expand(cur);
}

void ChatFrame::SetupFsWatcher() {
    delete fileWatcher_;
    fileWatcher_ = new wxFileSystemWatcher();
    fileWatcher_->SetOwner(this);  // events are queued on `this`
    RescanFsWatcher();
}

// wxDir classifies a symlink to a directory as a directory, and the watcher
// canonicalises paths before watching — so a symlinked dir (venv/lib64 -> lib)
// would re-add an already-watched path and trip a backend assertion. Detect
// symlinks through wxFileName so this stays backend-agnostic.
static bool IsSymlink(const wxString& path) {
    return wxFileName(path).Exists(wxFILE_EXISTS_SYMLINK);
}

void ChatFrame::CollectExpandedDirs(wxTreeItemId parent, int depth,
                                    std::vector<wxString>& out) {
    constexpr int kMaxWatchDepth = 4;
    if (depth > kMaxWatchDepth) return;
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = fileTree_->GetFirstChild(parent, cookie);
         c.IsOk(); c = fileTree_->GetNextChild(parent, cookie)) {
        if (!fileTree_->IsExpanded(c)) continue;
        auto* d = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(c));
        if (!d || !d->isDir || IsSymlink(d->path)) continue;
        out.push_back(d->path);
        CollectExpandedDirs(c, depth + 1, out);
    }
}

void ChatFrame::RescanFsWatcher() {
    if (!fileWatcher_) return;
    fileWatcher_->RemoveAll();
    if (activeCwd_.empty()) return;

    // Watch only what's visible in the file tree: the session root plus every
    // expanded directory, capped at 4 levels deep. Deeper expansions need a
    // manual Refresh. This keeps the watch count proportional to what's open,
    // not the size of the project.
    std::vector<wxString> dirs;
    dirs.push_back(wxString::FromUTF8(activeCwd_));
    if (fileTree_ && fileTree_->GetRootItem().IsOk())
        CollectExpandedDirs(fileTree_->GetRootItem(), 1, dirs);

    for (const auto& dir : dirs) {
        if (!wxDirExists(dir)) continue;
        fileWatcher_->Add(wxFileName(dir));
    }
}

void ChatFrame::OnFsWatcherEvent(wxFileSystemWatcherEvent&) {
    // Coalesce bursts (an agent writing several files at once) into one reload.
    if (treeRefreshTimer_) treeRefreshTimer_->Start(300, true);
}

void ChatFrame::OnTreeRefreshTimer(wxTimerEvent&) {
    // Modal dialogs (save prompt, "changed on disk", rename, …) run a nested
    // event loop that still delivers watcher/timer events, and they disable
    // this frame. Defer rather than rebuild the tree (DeleteAllItems)
    // underneath a handler that is still mid-flight; retry once the dialog
    // closes and IsEnabled() flips back.
    if (!IsEnabled()) {
        if (treeRefreshTimer_) treeRefreshTimer_->Start(500, true);
        return;
    }

    ReloadTreeKeepExpanded();
    // Directories created since the last scan aren't watched yet.
    RescanFsWatcher();
    CheckEditorFileChangedOnDisk();

    // AGENTS.md may have been added or edited since the session loaded —
    // re-inject it into the system prompt only when its content changed.
    std::string agents = LoadAgentsInstructions(activeCwd_);
    if (agents != lastAgentsContent_) RefreshSystemPromptAgents();
}

void ChatFrame::OnTreeRefresh(wxCommandEvent&) {
    ReloadTreeKeepExpanded();
}

void ChatFrame::OnTreeToggleHidden(wxCommandEvent&) {
    showHidden_ = !showHidden_;
    ReloadTreeKeepExpanded();
}

void ChatFrame::OnEditorTreeExpanding(wxTreeEvent& e) {
    auto* data = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(e.GetItem()));
    if (data && data->isDir) {
        PopulateTreeDir(e.GetItem(), data->path);
        RescanFsWatcher();  // start watching the newly expanded dir
    }
    e.Skip();
}

void ChatFrame::OnEditorTreeCollapsed(wxTreeEvent& e) {
    RescanFsWatcher();  // stop watching the just-collapsed subtree
    e.Skip();
}

void ChatFrame::OnEditorTreeSelect(wxTreeEvent& e) {
    if (treeSelectionRestoring_) { e.Skip(); return; }

    // Copy out of the item data up front. MaybeSaveEditor() shows a modal
    // dialog whose nested event loop can trigger a background tree reload
    // (DeleteAllItems), which would free `data`; keep only value copies alive.
    wxString path;
    bool isDir = true;
    if (auto* data =
            dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(e.GetItem()))) {
        path = data->path;
        isDir = data->isDir;
    }

    if (!isDir && !path.empty() && path != editorFilePath_) {
        // Switching to a different file: protect unsaved changes first.
        if (!MaybeSaveEditor()) {
            // Cancel: put the selection back on the open file (if it is
            // still visible in the tree), otherwise clear it.
            wxTreeItemId prev = FindTreeItemByPath(fileTree_->GetRootItem(),
                                                   editorFilePath_);
            if (prev.IsOk()) fileTree_->SelectItem(prev);
            else fileTree_->UnselectAll();
            return;
        }
        LoadFileIntoEditor(path);
    }
    e.Skip();
}

wxTreeItemId ChatFrame::FindTreeItemByPath(wxTreeItemId parent,
                                           const wxString& path) {
    if (!parent.IsOk()) return wxTreeItemId();
    wxTreeItemIdValue cookie;
    for (wxTreeItemId c = fileTree_->GetFirstChild(parent, cookie);
         c.IsOk(); c = fileTree_->GetNextChild(parent, cookie)) {
        auto* d = dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(c));
        if (d && d->path == path) return c;
        if (fileTree_->ItemHasChildren(c)) {
            wxTreeItemId found = FindTreeItemByPath(c, path);
            if (found.IsOk()) return found;
        }
    }
    return wxTreeItemId();
}

void ChatFrame::OnEditorTreeContextMenu(wxContextMenuEvent& e) {
    // Right-click on a tree item or empty area. Use the mouse position so we
    // can find the item under the cursor (keyboard-triggered events carry no
    // position, in which case fall back to the current pointer location).
    wxPoint pt = e.GetPosition();
    if (pt == wxDefaultPosition) {
        pt = fileTree_->ScreenToClient(wxGetMousePosition());
    } else {
        pt = fileTree_->ScreenToClient(pt);
    }
    int flags = 0;
    wxTreeItemId item = fileTree_->HitTest(pt, flags);
    ShowTreeContextMenu(item);
}

void ChatFrame::ShowTreeContextMenu(wxTreeItemId item) {
    auto* data = item.IsOk()
        ? dynamic_cast<FileTreeItemData*>(fileTree_->GetItemData(item)) : nullptr;

    treeCtxItem_ = item;
    if (data) {
        treeCtxPath_ = data->path;
        treeCtxIsDir_ = data->isDir;
    } else {
        // Right-click on empty space targets the project root.
        treeCtxPath_ = wxString::FromUTF8(activeCwd_);
        treeCtxIsDir_ = true;
    }

    wxMenu menu;
    menu.Append(ID_TREE_NEW_FILE, "New File");
    menu.Append(ID_TREE_NEW_FOLDER, "New Folder");
    menu.AppendSeparator();
    wxMenuItem* renameItem = menu.Append(ID_TREE_RENAME, "Rename");
    wxMenuItem* showItem = menu.Append(ID_TREE_SHOW_IN_FILES, "Show in Files");
    menu.AppendSeparator();
    menu.AppendCheckItem(ID_TREE_TOGGLE_HIDDEN, "Show Hidden Files")
        ->Check(showHidden_);
    menu.AppendSeparator();
    menu.Append(ID_TREE_REFRESH, "Refresh");
    if (!item.IsOk()) {
        renameItem->Enable(false);
        showItem->Enable(false);
    }
    fileTree_->PopupMenu(&menu);
}

void ChatFrame::TreeCtxTarget(wxString& dir, wxTreeItemId& parentItem) {
    if (treeCtxItem_.IsOk() && treeCtxIsDir_) {
        dir = treeCtxPath_;
        parentItem = treeCtxItem_;
    } else if (treeCtxItem_.IsOk()) {
        dir = wxFileName(treeCtxPath_).GetPath();
        parentItem = fileTree_->GetItemParent(treeCtxItem_);
    } else {
        dir = wxString::FromUTF8(activeCwd_);
        parentItem = wxTreeItemId();
    }
}

void ChatFrame::OnTreeNewFile(wxCommandEvent&) {
    wxString dir;
    wxTreeItemId parentItem;
    TreeCtxTarget(dir, parentItem);

    wxString name = wxGetTextFromUser("File name:", "New File", "", this);
    if (name.empty()) return;
    if (name.Find(wxFILE_SEP_PATH) != wxNOT_FOUND) {
        wxMessageBox("The file name cannot contain path separators.",
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    wxString path = dir + wxFILE_SEP_PATH + name;
    if (wxFileExists(path) || wxDirExists(path)) {
        wxMessageBox("A file or directory with that name already exists:\n" + path,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    wxFile f(path, wxFile::write);
    if (!f.IsOpened()) {
        wxMessageBox("Could not create file:\n" + path,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }
    f.Close();

    if (parentItem.IsOk()) {
        PopulateTreeDir(parentItem, dir);
        fileTree_->Expand(parentItem);
        // Select the new file; OnEditorTreeSelect opens it in the editor
        // (and guards any unsaved changes in the previously open file).
        wxTreeItemIdValue cookie;
        for (wxTreeItemId c = fileTree_->GetFirstChild(parentItem, cookie);
             c.IsOk(); c = fileTree_->GetNextChild(parentItem, cookie)) {
            if (fileTree_->GetItemText(c) == name) {
                fileTree_->SelectItem(c);
                break;
            }
        }
    } else {
        PopulateEditorTree();
    }
}

void ChatFrame::OnTreeNewFolder(wxCommandEvent&) {
    wxString dir;
    wxTreeItemId parentItem;
    TreeCtxTarget(dir, parentItem);

    wxString name = wxGetTextFromUser("Folder name:", "New Folder", "", this);
    if (name.empty()) return;
    if (name.Find(wxFILE_SEP_PATH) != wxNOT_FOUND) {
        wxMessageBox("The folder name cannot contain path separators.",
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    wxString path = dir + wxFILE_SEP_PATH + name;
    if (wxFileExists(path) || wxDirExists(path)) {
        wxMessageBox("A file or directory with that name already exists:\n" + path,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    if (!wxMkdir(path)) {
        wxMessageBox("Could not create folder:\n" + path,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    if (parentItem.IsOk()) {
        PopulateTreeDir(parentItem, dir);
        fileTree_->Expand(parentItem);
    } else {
        PopulateEditorTree();
    }
}

void ChatFrame::OnTreeRename(wxCommandEvent&) {
    if (!treeCtxItem_.IsOk()) return;

    wxString oldPath = treeCtxPath_;
    wxString oldName = wxFileName(oldPath).GetFullName();
    wxString newName = wxGetTextFromUser("New name:", "Rename", oldName, this);
    if (newName.empty() || newName == oldName) return;
    if (newName.Find(wxFILE_SEP_PATH) != wxNOT_FOUND) {
        wxMessageBox("The name cannot contain path separators.",
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    wxString newPath = wxFileName(oldPath).GetPath() + wxFILE_SEP_PATH + newName;
    if (wxFileExists(newPath) || wxDirExists(newPath)) {
        wxMessageBox("A file or directory with that name already exists:\n" + newPath,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    if (!wxRenameFile(oldPath, newPath)) {
        wxMessageBox("Could not rename:\n" + oldPath,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return;
    }

    // Keep the open file's path in sync if it was just renamed.
    if (!treeCtxIsDir_ && editorFilePath_ == oldPath) {
        editorFilePath_ = newPath;
        UpdateWindowTitle();
    }

    wxTreeItemId parent = fileTree_->GetItemParent(treeCtxItem_);
    if (parent.IsOk()) {
        wxString parentPath = wxFileName(oldPath).GetPath();
        PopulateTreeDir(parent, parentPath);
    } else {
        PopulateEditorTree();
    }
}

void ChatFrame::OnTreeShowInFiles(wxCommandEvent&) {
    ShowFileInManager(treeCtxPath_);
}

void ChatFrame::ShowFileInManager(const wxString& path) {
    const bool isDir = wxDirExists(path);
#ifdef __WXMSW__
    wxString cmd = isDir
        ? "explorer.exe \"" + path + "\""
        : "explorer.exe /select,\"" + path + "\"";
    wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
#elif defined(__WXOSX__)
    wxString cmd = isDir ? "open \"" + path + "\"" : "open -R \"" + path + "\"";
    wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
#else
    // Prefer the freedesktop FileManager1 interface so the file gets selected
    // in the file manager; fall back to opening the containing directory.
    wxString uri = wxFileName::FileNameToURL(wxFileName(path));
    wxString cmd =
        "dbus-send --session --dest=org.freedesktop.FileManager1 "
        "--type=method_call /org/freedesktop/FileManager1 "
        "org.freedesktop.FileManager1.ShowItems "
        "array:string:\"" + uri + "\" string:\"\"";
    if (wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE) == 0) {
        wxString target = isDir ? path : wxFileName(path).GetPath();
        if (!target.empty()) wxLaunchDefaultApplication(target);
    }
#endif
}

void ChatFrame::LoadFileIntoEditor(const wxString& path) {
    // Default to editable; binary/too-large placeholders switch to read-only
    // below. Reset here so a failed open can't leave the editor read-only.
    codeEdit_->SetEditable(true);
    suppressReloadPrompt_ = false;  // new file: re-arm the change prompt

    wxFile f(path, wxFile::read);
    if (!f.IsOpened()) return;

    wxFileOffset len = f.Length();
    const wxFileOffset kMaxBytes = 2 * 1024 * 1024;
    if (len > kMaxBytes) {
        // Keep the path so the title bar and close/reload still know the
        // file, but make the placeholder read-only (there is nothing to save).
        // Set the text first: ChangeValue is ignored once the control is
        // read-only on wxGTK.
        codeEdit_->ChangeValue(wxString::Format(
            wxString::FromUTF8("File is %lld bytes — too large to open here."),
            (long long)len));
        codeEdit_->SetEditable(false);
        editorFilePath_ = path;
        editorDirty_ = false;
        UpdateWindowTitle();
        return;
    }

    std::vector<char> buf((size_t)len + 1);
    size_t n = f.Read(buf.data(), (size_t)len);
    buf[n] = 0;
    if (memchr(buf.data(), 0, n)) {
        // Binary file: keep the path but don't let the user edit the
        // placeholder (saving it would corrupt the real file). Set the text
        // first: ChangeValue is ignored once the control is read-only.
        codeEdit_->ChangeValue(wxString::FromUTF8("[ Binary file — not shown ]"));
        codeEdit_->SetEditable(false);
        editorFilePath_ = path;
        editorDirty_ = false;
        UpdateWindowTitle();
        return;
    }

    codeEdit_->ChangeValue(wxString::FromUTF8(buf.data(), n));
    editorFilePath_ = path;
    editorDirty_ = false;
    UpdateWindowTitle();
    syntax::Highlight(codeEdit_, path, std::string_view(buf.data(), n));
    RecordEditorFileStamp();
}

void ChatFrame::RecordEditorFileStamp() {
    lastFileMtime_ = wxDateTime();
    lastFileSize_ = 0;
    if (editorFilePath_.empty()) return;
    wxFileName fn(editorFilePath_);
    if (!fn.Exists()) return;
    lastFileMtime_ = fn.GetModificationTime();
    lastFileSize_ = fn.GetSize();
}

void ChatFrame::CheckEditorFileChangedOnDisk() {
    if (editorFilePath_.empty()) return;
    // Read-only placeholders (binary / too-large) aren't editable and carry
    // no meaningful stamp, so there's nothing to reload into them.
    if (!codeEdit_->IsEditable()) return;

    wxFileName fn(editorFilePath_);
    if (!fn.Exists()) return;  // deleted on disk: keep the buffer for Save As

    wxDateTime mtime = fn.GetModificationTime();
    wxULongLong size = fn.GetSize();

    bool changed = mtime.IsValid() && lastFileMtime_.IsValid() &&
                   (mtime != lastFileMtime_ || size != lastFileSize_);
    if (!changed) return;

    if (editorDirty_) {
        // Ask once per conflict. The flag is set before ShowModal so the
        // dialog's nested event loop (which keeps delivering watcher/timer
        // events) can't re-enter here and stack another prompt on top.
        if (suppressReloadPrompt_) return;
        suppressReloadPrompt_ = true;

        wxMessageDialog dlg(this,
            "\"" + editorFilePath_ + "\" has changed on disk.\n\n"
            "Reload and discard your unsaved changes?",
            "File Changed on Disk", wxYES_NO | wxNO_DEFAULT);
        dlg.SetYesNoLabels("Reload", "Keep My Changes");
        if (dlg.ShowModal() != wxID_YES) {
            // User keeps their edits: adopt the new stamp and stay suppressed
            // until they save/reload/switch file, so later writes to the same
            // file don't nag again.
            RecordEditorFileStamp();
            return;
        }
    }

    // Clean (no unsaved edits): reload silently to reflect the disk version.
    LoadFileIntoEditor(editorFilePath_);
}

bool ChatFrame::WriteEditorFile(const wxString& path) {
    // GetValue() returns a temporary wxString, and utf8_str() is a NON-OWNING
    // view into it in every build (CreateNonOwned over the string's internal
    // buffer in UTF-8 builds, and over m_convertedToChar in wchar builds).
    // Storing it in a variable dangles the instant the temporary is destroyed,
    // so the old code wrote freed heap memory to disk (the allocator reuses
    // the block and the first bytes come out as garbage — e.g. a tcache
    // pointer). Copy into an owned std::string first — the same pattern
    // OnHighlightTimer already uses for exactly this reason — and write with
    // truncation so a shorter save can't leave the tail of the previous
    // contents behind.
    std::string utf8Path = path.ToStdString(wxConvUTF8);
    std::string utf8 = codeEdit_->GetValue().ToStdString(wxConvUTF8);
    std::ofstream f(utf8Path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8.data(), (std::streamsize)utf8.size());
    if (!f) return false;
    return true;
}

bool ChatFrame::SaveEditorFile() {
    // Read-only placeholder (binary/too-large file): nothing to save.
    if (!codeEdit_->IsEditable()) return false;
    if (editorFilePath_.empty()) return SaveEditorFileAs();
    if (!WriteEditorFile(editorFilePath_)) {
        wxMessageBox("Could not write file:\n" + editorFilePath_,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return false;
    }
    editorDirty_ = false;
    UpdateWindowTitle();
    RecordEditorFileStamp();
    suppressReloadPrompt_ = false;  // resolved: re-arm the change prompt
    return true;
}

bool ChatFrame::SaveEditorFileAs() {
    if (!codeEdit_->IsEditable()) return false;  // nothing meaningful to save
    wxString dir, name;
    if (!editorFilePath_.empty()) {
        wxFileName fn(editorFilePath_);
        dir = fn.GetPath();
        name = fn.GetFullName();
    } else {
        dir = activeCwd_.empty() ? wxString() : wxString::FromUTF8(activeCwd_);
        name = "untitled";
    }
    wxFileDialog dlg(this, "Save File As", dir, name,
                     "All Files (*)|*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return false;

    wxString path = dlg.GetPath();
    if (!WriteEditorFile(path)) {
        wxMessageBox("Could not write file:\n" + path,
                     "gritcode", wxOK | wxICON_ERROR, this);
        return false;
    }
    editorFilePath_ = path;
    editorDirty_ = false;
    UpdateWindowTitle();
    RecordEditorFileStamp();
    suppressReloadPrompt_ = false;  // resolved: re-arm the change prompt
    return true;
}

void ChatFrame::ReloadEditorFile() {
    if (editorFilePath_.empty()) return;
    if (!MaybeSaveEditor()) return;
    LoadFileIntoEditor(editorFilePath_);
}

void ChatFrame::CloseEditorFile() {
    if (!MaybeSaveEditor()) return;
    ClearEditorState();
    // Deselect the tree item so clicking the same file again reloads it
    // (otherwise it is already selected and won't fire SEL_CHANGED).
    if (fileTree_) fileTree_->Unselect();
}

void ChatFrame::ClearEditorState() {
    if (highlightTimer_) highlightTimer_->Stop();
    codeEdit_->SetEditable(true);
    codeEdit_->ChangeValue("");
    syntax::ClearStyles(codeEdit_);
    editorFilePath_.clear();
    editorDirty_ = false;
    suppressReloadPrompt_ = false;
    lastFileMtime_ = wxDateTime();
    lastFileSize_ = 0;
    UpdateWindowTitle();
}

bool ChatFrame::MaybeSaveEditor() {
    if (!editorDirty_) return true;

    wxString name = editorFilePath_.empty()
                  ? wxString("Untitled") : editorFilePath_;
    wxMessageDialog dlg(this, "Save changes to \"" + name + "\"?",
                        "Unsaved Changes", wxYES_NO | wxCANCEL | wxYES_DEFAULT);
    dlg.SetYesNoLabels("Save", "Don't Save");
    switch (dlg.ShowModal()) {
        case wxID_YES: return SaveEditorFile();
        case wxID_NO:  return true;
        default:       return false;  // Cancel
    }
}

void ChatFrame::OnEditorTextChanged(wxCommandEvent& e) {
    if (!editorDirty_) {
        editorDirty_ = true;
        UpdateWindowTitle();
    }
    if (highlightTimer_) highlightTimer_->Start(300, true);
    e.Skip();
}

void ChatFrame::OnHighlightTimer(wxTimerEvent&) {
    if (!codeEdit_ || !codeEdit_->IsEditable() || editorFilePath_.empty()) return;
    // GetValue() returns a temporary wxString and utf8_str() is non-owning, so
    // capturing it directly would dangle. Copy into an owned std::string first.
    std::string text = codeEdit_->GetValue().ToStdString(wxConvUTF8);
    syntax::Highlight(codeEdit_, editorFilePath_, text);
}

void ChatFrame::OnEditorChar(wxKeyEvent& e) {
    if (!codeEdit_ || !codeEdit_->IsEditable()) {
        e.Skip();
        return;
    }
    int key = e.GetKeyCode();

    // Tab inserts spaces (never a literal tab, and never focus navigation —
    // the editor has wxTE_PROCESS_TAB so it receives the key).
    if (key == WXK_TAB) {
        long from, to;
        codeEdit_->GetSelection(&from, &to);
        codeEdit_->Replace(from, to, "    ");
        codeEdit_->SetInsertionPoint(from + (long)editor_indent::kIndentWidth);
        return;
    }

    // Enter keeps the current line's indentation on the new line.
    if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
        long from, to;
        codeEdit_->GetSelection(&from, &to);
        wxString value = codeEdit_->GetValue();
        wxString indent = editor_indent::LineLeadingIndent(value, from);
        wxString insert = "\n" + indent;
        codeEdit_->Replace(from, to, insert);
        codeEdit_->SetInsertionPoint(from + 1 + (long)indent.length());
        return;
    }

    // Backspace deletes a whole indent level when the cursor is inside the
    // line's leading spaces (8 -> 4 -> 0, with a partial indent snapping to
    // the previous multiple of the indent width).
    if (key == WXK_BACK) {
        long from, to;
        codeEdit_->GetSelection(&from, &to);
        if (from != to) {  // selection: default single-character delete
            e.Skip();
            return;
        }
        long deleteCount =
            editor_indent::BackspaceIndentCount(codeEdit_->GetValue(), from);
        if (deleteCount == 0) {
            e.Skip();
            return;
        }
        codeEdit_->Remove(from - deleteCount, from);
        return;
    }

    e.Skip();
}

void ChatFrame::OnEditorContextMenu(wxContextMenuEvent& e) {
    bool hasFile = !editorFilePath_.empty();
    bool editable = codeEdit_->IsEditable();
    wxMenu menu;
    wxMenuItem* save = menu.Append(ID_EDITOR_SAVE, "Save\tCtrl+S");
    wxMenuItem* saveAs = menu.Append(ID_EDITOR_SAVE_AS, wxString::FromUTF8("Save As…"));
    menu.AppendSeparator();
    wxMenuItem* reload = menu.Append(ID_EDITOR_RELOAD, "Reload from Disk");
    wxMenuItem* close = menu.Append(ID_EDITOR_CLOSE, "Close File");
    menu.AppendSeparator();
    wxMenuItem* show = menu.Append(ID_EDITOR_SHOW_IN_FILES, "Show in Files");

    save->Enable(hasFile && editable);
    saveAs->Enable(editable);
    reload->Enable(hasFile);
    close->Enable(hasFile);
    show->Enable(hasFile);

    // PopupMenu() is modal; the actions run from the frame-level menu
    // handlers bound in the constructor (same pattern as the file tree).
    codeEdit_->PopupMenu(&menu);
}

void ChatFrame::OnEditorSave(wxCommandEvent&) { SaveEditorFile(); }
void ChatFrame::OnEditorSaveAs(wxCommandEvent&) { SaveEditorFileAs(); }
void ChatFrame::OnEditorReload(wxCommandEvent&) { ReloadEditorFile(); }
void ChatFrame::OnEditorCloseFile(wxCommandEvent&) { CloseEditorFile(); }
void ChatFrame::OnEditorShowInFiles(wxCommandEvent&) {
    if (!editorFilePath_.empty()) ShowFileInManager(editorFilePath_);
}

void ChatFrame::UpdateWindowTitle() {
    wxString title = "gritcode";
    if (innerSplitter_ && innerSplitter_->IsSplit() && !editorFilePath_.empty()) {
        title = editorFilePath_ + wxString::FromUTF8(" — gritcode");
        if (editorDirty_) title = "* " + title;
    }
    SetTitle(title);
}

void ChatFrame::OnExport(wxCommandEvent&) {
    wxFileDialog dlg(this, "Export Session", "", "gritcode-session",
                     "Gritcode Session (*.gritsession)|*.gritsession",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    std::string path = dlg.GetPath().ToStdString(wxConvUTF8);
    if (path.find(".gritsession") == std::string::npos)
        path += ".gritsession";

    nlohmann::json j;
    j["version"] = HistoryHasImages(history_) ? 2 : 1;
    j["exportedAt"] = []() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
        return std::string(buf);
    }();
    j["messages"] = history_;

    std::string err;
    bool ok = false;
    if (HistoryHasImages(history_)) {
        ok = ExportToZip(path, j, CollectImageRefs(history_), err);
    } else {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        ok = (bool)f;
        if (ok) {
            f << j.dump(2, ' ', false,
                        nlohmann::json::error_handler_t::replace);
        } else {
            err = "Failed to write file.";
        }
    }

    if (!ok) {
        wxMessageBox(wxString::FromUTF8(err), "Export",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    wxMessageBox(FormatU8("Session exported ({} messages).", history_.size()),
                 "Export", wxOK | wxICON_INFORMATION, this);
}

void ChatFrame::OnImport(wxCommandEvent&) {
    wxFileDialog dlg(this, "Import Session", "", "",
                     "Gritcode Session (*.gritsession)|*.gritsession",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    std::string path = dlg.GetPath().ToStdString(wxConvUTF8);

    nlohmann::json j;
    std::string err;
    if (!ImportSessionFile(path, j, err)) {
        wxMessageBox(wxString::FromUTF8(err), "Import",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    if (!j.contains("messages") || !j["messages"].is_array()) {
        wxMessageBox("No messages found in file.", "Import",
                     wxOK | wxICON_ERROR, this);
        return;
    }

    importedMessages_.clear();
    for (const auto& m : j["messages"]) {
        if (m.is_object()) importedMessages_.push_back(m);
    }

    // Store filename for display.
    wxString fullPath = wxString::FromUTF8(path);
    importedFileName_ = fullPath.AfterLast('/');
    if (importedFileName_.empty()) importedFileName_ = fullPath;

    ShowImportDialog();
}

void ChatFrame::ShowImportDialog() {
    if (importedMessages_.empty()) return;

    importCanvas_->Clear();

    // Update header with file name.
    wxString displayName = importedFileName_.empty()
        ? wxString::FromUTF8("Imported Session")
        : importedFileName_;

    // Render imported messages into the side canvas.
    importCanvas_->Clear();
    MdStream importStream([this](Block b) {
        importCanvas_->AddBlock(std::move(b));
    });
    // Say which model the session started on and where it changed. Files
    // exported before gritcode recorded models carry no "model" and show no
    // notices.
    ModelSwitchTracker modelTracker;

    for (const auto& m : importedMessages_) {
        std::string role = m.value("role", std::string{});
        if (role == "system") continue;

        if (role == "user") {
            if (IsUserTurn(m)) {
                wxString notice = modelTracker.Next(m, /*announceFirst=*/true);
                if (!notice.IsEmpty()) importCanvas_->AddBlock(NoticeBlock(notice));
            }
            std::string content = m.value("content", std::string{});
            Block ub;
            ub.type = BlockType::UserPrompt;
            ub.rawText = wxString::FromUTF8(content);
            InlineRun r;
            r.text = ub.rawText;
            ub.runs.push_back(r);
            ub.visibleText = ub.rawText;
            importCanvas_->AddBlock(std::move(ub));

            if (m.contains("images") && m["images"].is_array()) {
                for (const auto& img : m["images"]) {
                    if (!img.is_object()) continue;
                    importCanvas_->AddBlock(MakeImageBlock(
                        img.value("sha256", std::string{}),
                        img.value("mime", "image/png"),
                        img.value("name", std::string{})));
                }
            }
        } else if (role == "assistant") {
            if (m.contains("content") && m["content"].is_string() && !m["content"].get<std::string>().empty()) {
                std::string content = m["content"].get<std::string>();
                importStream.Feed(wxString::FromUTF8(content));
            }
            if (m.contains("reasoning_content") && m["reasoning_content"].is_string()) {
                std::string rc = m["reasoning_content"].get<std::string>();
                Block tb;
                tb.type = BlockType::Thinking;
                tb.rawText = wxString::FromUTF8(rc);
                tb.visibleText = tb.rawText;
                tb.toolExpanded = false;
                importCanvas_->AddBlock(std::move(tb));
            }
            importStream.Flush();
        } else if (role == "tool") {
            std::string name = m.value("name", std::string{});
            std::string result = m.value("content", std::string{});
            // Find matching tool_call from preceding assistant message.
            std::string displayArgs;
            if (&m > &importedMessages_.front()) {
                const auto* prev = &m - 1;
                while (prev >= &importedMessages_.front()) {
                    if (prev->value("role", std::string{}) == "assistant" &&
                        prev->contains("tool_calls") && (*prev)["tool_calls"].is_array()) {
                        for (const auto& tc : (*prev)["tool_calls"]) {
                            if (tc.value("id", std::string{}) == m.value("tool_call_id", std::string{})) {
                                if (tc.contains("function") && tc["function"].contains("arguments"))
                                    displayArgs = tc["function"]["arguments"].get<std::string>();
                                break;
                            }
                        }
                        break;
                    }
                    --prev;
                }
            }
            Block tb;
            tb.type = BlockType::ToolCall;
            tb.toolName = wxString::FromUTF8(name);
            tb.toolArgs = wxString::FromUTF8(displayArgs);
            tb.toolResult = wxString::FromUTF8(result);
            tb.visibleText = tb.toolName + "(" + tb.toolArgs + ")\n\n" + tb.toolResult;
            tb.toolExpanded = false;
            importCanvas_->AddBlock(std::move(tb));
        }
    }

    // Swap empty view → canvas, update reference label.
    importEmptyView_->Hide();
    importCanvas_->Show();

    // Update reference label.
    refLabel_->SetLabel(wxString::FromUTF8("Referenced Session: ") + displayName
                        + wxString::FromUTF8(" \xe2\x80\x94 "));
    changeBtn_->Show();
    importPanel_->Layout();

    // Split to show the import panel on the left.
    if (!splitter_->IsSplit()) {
        int centerW = mainPanel_->GetSize().x;
        importPanel_->Show();
        splitter_->SplitVertically(importPanel_, innerSplitter_, kImportPaneWidth);
        SyncPanelSizing(centerW);
    }
}

void ChatFrame::OnSend(wxCommandEvent&) {
    wxString text = input_->GetValue();
    text.Trim().Trim(false);
    if (text.IsEmpty() && pendingImages_.empty()) return;

    // Agent busy: append to the queue (the Send button is "Add" while busy).
    // Reaching kMaxQueue_ disables the button — defensive check for a
    // synthesized event from MCP / Enter key.
    if (streaming_) {
        if (pendingQueue_.size() >= kMaxQueue_) return;
        EnqueueMessage(text);
        input_->Clear();
        return;
    }

    input_->Clear();
    std::vector<PendingImage> images = std::move(pendingImages_);
    pendingImages_.clear();
    RebuildImageRow();
    StartTurn(text, std::move(images));
}

void ChatFrame::StartTurn(const wxString& userText,
                        std::vector<PendingImage> images,
                        const std::string& hiddenInstructions) {
    overflowRetried_ = false;
    turnModelIndex_ = currentModelIndex_;
    const ModelRoute route = RouteForIndex(currentModelIndex_, remoteModels_);
    const std::string modelId = route.model;
    RenderModelSwitchNotice(modelId);
    // Render the user prompt block (skipped for image-only messages).
    if (!userText.IsEmpty()) {
        Block ub;
        ub.type = BlockType::UserPrompt;
        ub.rawText = userText;
        InlineRun r;
        r.text = userText;
        ub.runs.push_back(r);
        ub.visibleText = userText;
        canvas_->AddBlock(std::move(ub));
    }
    for (const auto& pi : images) {
        EmitImageBlock(pi.hash, pi.mime, pi.name);
    }

    // "model" records where the turn was sent, so a restored or exported
    // session can show where the model changed.
    nlohmann::json userMsg = {
        {"role", "user"},
        {"content", userText.ToStdString(wxConvUTF8) + hiddenInstructions},
        {"model", modelId},
    };
    if (!images.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& pi : images) {
            arr.push_back({{"sha256", pi.hash},
                           {"mime", pi.mime},
                           {"name", pi.name}});
        }
        userMsg["images"] = std::move(arr);
    }
    history_.push_back(std::move(userMsg));

    streaming_ = true;
    canvas_->SetThinking(true);
    UpdateQueueUI();  // Send becomes "Add", chip row stays visible if non-empty.
    toolIter_ = 0;

    StartCompletion();
}

void ChatFrame::EnqueueMessage(const wxString& text) {
    QueuedMessage q;
    q.text = text.ToStdString(wxConvUTF8);
    q.images = std::move(pendingImages_);
    pendingImages_.clear();
    RebuildImageRow();
    pendingQueue_.push_back(std::move(q));
    UpdateQueueUI();
}

void ChatFrame::LogDebug(const std::string& line) {
#ifndef NDEBUG
    debugBuffer_ += line;
    debugBuffer_ += '\n';
    constexpr size_t kMaxBuffer = 1'500'000;
    if (debugBuffer_.size() > kMaxBuffer) {
        debugBuffer_.erase(0, debugBuffer_.size() - kMaxBuffer / 2);
    }
    if (debugWindow_) debugWindow_->Append(wxString::FromUTF8(line));
#else
    (void)line;
#endif
}

void ChatFrame::OpenDebugWindow() {
#ifndef NDEBUG
    if (!debugWindow_) {
        debugWindow_ = new DebugWindow(this);
        debugWindow_->SetText(wxString::FromUTF8(debugBuffer_));
    }
    debugWindow_->Show();
    debugWindow_->Raise();
#endif
}

nlohmann::json ChatFrame::BuildModelView() const {
    nlohmann::json view = nlohmann::json::array();

    // System prompt is always the head.
    if (!history_.empty() && history_[0].is_object()
        && history_[0].value("role", std::string{}) == "system") {
        view.push_back(history_[0]);
    }

    // Summary checkpoints become the new head after compaction.
    for (const auto& m : history_) {
        if (!m.is_object() || m.value("compacted", false)) continue;
        if (m.value("isSummary", false)) view.push_back(m);
    }

    // Non-hidden, non-summary, non-system tail.
    nlohmann::json tail = nlohmann::json::array();
    for (const auto& m : history_) {
        if (!m.is_object() || m.value("compacted", false)) continue;
        if (m.value("isSummary", false)) continue;
        if (m.value("role", std::string{}) == "system") continue;
        tail.push_back(m);
    }

    PruneToolOutputs(tail);

    for (auto& m : tail) view.push_back(std::move(m));
    return view;
}

void ChatFrame::PruneToolOutputs(nlohmann::json& tail) const {
    if (!tail.is_array() || tail.empty()) return;
    int n = (int)tail.size();

    // Walk newest->oldest: assign each message a turn index (0 = newest)
    // and the cumulative tool-output token count below it.
    std::vector<int> turnOf(n, 0);
    std::vector<int> toolTokensBelow(n, 0);
    int curTurn = 0;
    int toolTokens = 0;
    for (int i = n - 1; i >= 0; --i) {
        turnOf[i] = curTurn;
        const auto& m = tail[i];
        if (m.value("role", std::string{}) == "user") ++curTurn;
        toolTokensBelow[i] = toolTokens;
        if (m.value("role", std::string{}) == "tool")
            toolTokens += EstimateMessageTokens(m);
    }

    // First pass: how much would be freed by truncating stale outputs.
    int freedChars = 0;
    for (int i = 0; i < n; ++i) {
        const auto& m = tail[i];
        if (!m.is_object() || m.value("role", std::string{}) != "tool")
            continue;
        if (!m.contains("content") || !m["content"].is_string()) continue;
        bool fresh = turnOf[i] < kPruneFreshTurns
                     || toolTokensBelow[i] < kPruneProtectTokens;
        if (fresh) continue;
        const auto& c = m["content"].get_ref<const std::string&>();
        if ((int)c.size() > kToolOutputMaxChars)
            freedChars += (int)c.size() - kToolOutputMaxChars;
    }

    // Only prune when it actually matters.
    if (freedChars / 4 < kPruneMinFreedTokens) return;

    for (int i = 0; i < n; ++i) {
        auto& m = tail[i];
        if (!m.is_object() || m.value("role", std::string{}) != "tool")
            continue;
        if (!m.contains("content") || !m["content"].is_string()) continue;
        bool fresh = turnOf[i] < kPruneFreshTurns
                     || toolTokensBelow[i] < kPruneProtectTokens;
        if (fresh) continue;
        std::string c = m["content"].get<std::string>();
        if ((int)c.size() <= kToolOutputMaxChars) continue;
        m["content"] = c.substr(0, kToolOutputMaxChars)
                       + "\n[output truncated by compaction]";
    }
}

Block ChatFrame::MakeImageBlock(const std::string& hash, const std::string& mime,
                               const std::string& name) {
    Block b;
    b.type = BlockType::Image;
    b.imagePath = wxString::FromUTF8(ImageStore::PathFor(hash, mime));
    b.imageName = wxString::FromUTF8(name);
    return b;
}

void ChatFrame::EmitImageBlock(const std::string& hash, const std::string& mime,
                               const std::string& name) {
    canvas_->AddBlock(MakeImageBlock(hash, mime, name));
}

void ChatFrame::StartCompletion() {
    // A Claude turn never takes the HTTP path: Claude Code runs its own agent
    // loop and manages (and compacts) its own context, so gritcode's
    // compaction doesn't apply either.
    const ModelRoute turnRoute = RouteForIndex(turnModelIndex_, remoteModels_);
    if (turnRoute.claudeCli) {
        claudeModel_ = turnRoute.model;
        claudeRetriedFresh_ = false;
        StartClaudeTurn();
        return;
    }
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
    // Re-sync the process cwd to the active session right before every model
    // turn. The bash/list_directory/read_file/etc. tools fork in the process
    // cwd, so this must match activeCwd_ or the agent ends up operating in the
    // previous session's folder. Cheap (one syscall) and covers any call site
    // that forgets to chdir after changing activeCwd_.
    ChdirToCwd(activeCwd_);

    // Reset per-completion stream state.
    activeAssistantText_.clear();
    activeReasoning_.clear();
    activeToolCalls_.clear();
    thinkingEmitted_ = false;
    liveThinkingIdx_ = -1;
    finishReason_.clear();
    sseBuf_.clear();
    mdStream_ = std::make_unique<MdStream>([this](Block b) {
        canvas_->AddBlock(std::move(b));
    });

    ModelRoute route =
        CompletionRoute(currentModelIndex_, turnModelIndex_, remoteModels_);
    requestModel_ = route.model;

    // Build an outbound copy of history with the active cwd appended to the
    // system prompt. Done per-request rather than baked into stored history
    // so the cwd stays current if the user switches sessions mid-conversation
    // and doesn't accumulate across turns.
    nlohmann::json messages = BuildModelView();
    if (!messages.empty() && messages[0].is_object()
        && messages[0].value("role", std::string{}) == "system") {
        std::string base = messages[0].value("content", std::string{});
        std::string extra;
        if (!activeCwd_.empty())
            extra += "\n\nWorking directory: " + activeCwd_;
        // When the user disabled Grit History tools, explicitly countermand
        // the baked "use grit_history_search" system-prompt guidance so the
        // agent neither attempts the (now-absent) tools nor leans on local
        // cross-project history. Re-enabling makes the tools reappear in the
        // next request's tools array, which is the agent's source of truth
        // for what it may call.
        if (!Preferences::GetEnableGritHistory()) {
            extra +=
                "\n\nGrit History tools (grit_history_search, "
                "grit_history_fetch) are DISABLED for this session by the "
                "user. Do not call them - they will return an error. Do not "
                "reference or rely on prior conversation history from other "
                "projects or sessions.";
        }
        if (!extra.empty())
            messages[0]["content"] = base + extra;
    }

    // Claude Code turns become plain assistant text for this model, and the
    // gritcode-only bookkeeping fields stay off the wire.
    messages = FlattenClaudeTurns(std::move(messages));
    for (auto& m : messages) {
        if (!m.is_object()) continue;
        m.erase("model");
        m.erase("claudeSessionId");
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

    // DeepSeek requires the reasoning of earlier assistant turns whenever a
    // request carries `tools` (every chat request here does): "the
    // reasoning_content must be fully passed back to the API in all
    // subsequent requests", including turns without a tool call. So for
    // DeepSeek we send it back verbatim, and turns that have none still get
    // the (empty) field, which the API also insists on. The repeated history
    // is served from DeepSeek's prefix cache, and compaction already counts
    // reasoning when sizing the window. Other routes get it erased, as before.
    for (auto& m : messages) {
        if (m.is_object() && m.value("role", std::string{}) == "assistant") {
            if (route.provider == Preferences::Provider::DeepSeek
                && route.needsApiKey) {
                if (!m.contains("reasoning_content")
                    || !m["reasoning_content"].is_string())
                    m["reasoning_content"] = "";
            } else {
                m.erase("reasoning_content");
            }
        }
    }

    // B2: bound any single retained tool-call argument (e.g. a huge write_file
    // `content`) so one message can't dominate the tail. The call already ran
    // (its result follows it in the history), so the model only needs the
    // shape of the arguments to stay coherent, not the full payload. The
    // durable history_ is untouched — this only affects the outbound request.
    for (auto& m : messages) {
        if (!m.is_object() || m.value("role", std::string{}) != "assistant"
            || !m.contains("tool_calls") || !m["tool_calls"].is_array()) {
            continue;
        }
        for (auto& tc : m["tool_calls"]) {
            if (!tc.is_object() || !tc.contains("function")
                || !tc["function"].is_object()
                || !tc["function"].contains("arguments")
                || !tc["function"]["arguments"].is_string()) {
                continue;
            }
            const std::string& args =
                tc["function"]["arguments"].get_ref<const std::string&>();
            if ((int)args.size() > kToolCallArgsMaxChars) {
                std::string t = args.substr(0, kToolCallArgsMaxChars);
                t += "\n...[arguments truncated]";
                tc["function"]["arguments"] = std::move(t);
            }
        }
    }

    // Convert stored image attachments into an ask_vision hint for the
    // (text-only) chat model. The bytes live in the sidecar store; the model
    // can't see them inline, so we point it at the blob path and ask it to
    // use ask_vision. The `images` field is stripped from the wire message.
    for (auto& m : messages) {
        if (!m.is_object() || m.value("role", std::string{}) != "user"
            || !m.contains("images") || !m["images"].is_array()) {
            continue;
        }
        std::string note =
            "\n\n[The user attached image(s). They are stored at these paths "
            "and must be analyzed with the ask_vision tool:";
        for (const auto& img : m["images"]) {
            if (!img.is_object()) continue;
            std::string hash = img.value("sha256", std::string{});
            std::string mime = img.value("mime", "image/png");
            std::string name = img.value("name", std::string{});
            std::string path = ImageStore::PathFor(hash, mime);
            if (path.empty()) continue;
            note += "\n  - " + path;
            if (!name.empty()) note += " (" + name + ")";
        }
        note += "\nUse ask_vision on each path to see them.]";

        std::string content = m.value("content", std::string{});
        m["content"] = content + note;
        m.erase("images");
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

    // Estimate the prompt size and clamp max_tokens so prompt + completion
    // fits the model context. The API reserves max_tokens against the window
    // even when the completion doesn't use it. Use chars/3 (real ratio is
    // ~3.3); the overflow trigger + provider-400 recovery are the backstops.
    int promptTokens = EstimatePromptTokens(messages);
    int maxTokens = route.maxTokens;
    if (route.provider == Preferences::Provider::DeepSeek && route.needsApiKey
        && Preferences::GetReasoningEffort() == "max") {
        maxTokens = kOutputTokenMaxDeepSeekMax;
    }
    int avail = route.contextWindow - promptTokens - 8000;
    if (avail < 8000) avail = 8000;
    if (maxTokens > avail) maxTokens = avail;
    requestMaxTokens_ = maxTokens;

    nlohmann::json req;
    req["model"] = route.model;
    req["stream"] = true;
    req["max_tokens"] = maxTokens;
    req["messages"] = std::move(messages);
    req["tools"] = GetToolDefinitions(Preferences::GetEnableGritHistory());
    // DeepSeek thinking effort ("high" is the API default; "max" is opt-in
    // from Settings).
    if (route.provider == Preferences::Provider::DeepSeek && route.needsApiKey) {
        req["reasoning_effort"] =
            std::string(Preferences::GetReasoningEffort().utf8_string());
    }

    // error_handler_t::replace silently swaps invalid UTF-8 bytes in any
    // history string (bash output, file contents, model glitches, user paste)
    // for U+FFFD instead of throwing type_error.316.
    std::string body = req.dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace);

#ifndef NDEBUG
    {
        std::string preview = body;
        constexpr size_t kMaxLogBody = 400'000;
        if (preview.size() > kMaxLogBody) {
            preview.resize(kMaxLogBody);
            preview += "\n...[truncated]";
        }
        LogDebug("=== REQUEST model=" + std::string(route.model)
                 + " max_tokens=" + std::to_string(maxTokens)
                 + " body_bytes=" + std::to_string(body.size()) + " ===");
        LogDebug(preview);
    }
#endif

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
                if (choice.contains("finish_reason")
                    && choice["finish_reason"].is_string())
                    finishReason_ = choice["finish_reason"].get<std::string>();
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
                        UpdateLiveThinking();
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
        // Layer 3: context-overflow recovery. Force one compaction and
        // retry; if it overflows again, fall through to the error path.
        if (resp.status == 400 && !overflowRetried_) {
            std::string errBody =
                ExtractErrorBody().ToStdString(wxConvUTF8);
            bool isOverflow =
                errBody.find("context") != std::string::npos
                && (errBody.find("token") != std::string::npos
                    || errBody.find("maximum") != std::string::npos);
            if (isOverflow && ForceCompactForOverflow()) {
                overflowRetried_ = true;
                return;
            }
        }

        // No-key free model that stopped responding: the transport failed
        // (timeout, reset, closed mid-stream, unreachable, …). Show a friendly
        // explanation + a link to Settings instead of a raw curl error, so it
        // doesn't read as a gritcode failure.
        ModelRoute route =
            CompletionRoute(currentModelIndex_, turnModelIndex_, remoteModels_);
        if (!route.needsApiKey && resp.networkError) {
            HandleCompletion(wxString(), /*freeModelStall=*/true);
            return;
        }

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

void ChatFrame::HandleCompletion(const wxString& errorIfFailed,
                                 bool freeModelStall) {
    // Pure-reasoning response (model returned reasoning_content but no content
    // or tool_calls): emit the thinking block here so the user sees what the
    // model produced. No-op if a prior content/tool delta already emitted it.
    EmitPendingThinking();

    if (!errorIfFailed.IsEmpty() || freeModelStall) {
        if (freeModelStall) RenderFreeModelStallNotice();
        else RenderErrorBlock(errorIfFailed);
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

    // The reply stopped at max_tokens. With tool calls the truncated
    // arguments already produce a retry error for the model; for a plain
    // reply, tell the user instead of ending silently.
    if (finishReason_ == "length" && activeToolCalls_.empty()) {
        RenderErrorBlock(FormatU8(
            "The reply was cut off: it reached the output limit ({} tokens). "
            "Say \"continue\" to let the model pick up where it stopped.",
            requestMaxTokens_));
    }

    if (activeToolCalls_.empty()) {
        // Plain assistant message — record and finish.
        if (!activeAssistantText_.empty() || !activeReasoning_.empty()) {
            nlohmann::json msg = {{"role", "assistant"},
                                  {"content", activeAssistantText_},
                                  {"model", requestModel_}};
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
    nlohmann::json assistantMsg = {{"role", "assistant"},
                                   {"model", requestModel_}};
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

    // OnToolBatchDone only runs after the previous worker exits, so by the
    // time we dispatch a new batch the prior thread is finished — join() is
    // a non-blocking handoff that lets us reuse the std::thread slot without
    // detaching (detach makes destructor cleanup impossible).
    if (toolWorker_.joinable()) toolWorker_.join();
    bool enableGrit = Preferences::GetEnableGritHistory();
    toolWorker_ = std::thread([this, jobs, token, cwd = activeCwd_,
                               enableGrit]() {
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
                                 &memory_, cwd, enableGrit);
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
    // A cancelled/failed round may never reach EmitPendingThinking; don't
    // leave its Thinking block live with stale text.
    if (liveThinkingIdx_ >= 0) {
        canvas_->UpdateThinkingBlock(liveThinkingIdx_,
                                     wxString::FromUTF8(activeReasoning_), false);
        liveThinkingIdx_ = -1;
    }
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
    canvas_->AddBlock(NoticeBlock(msg));
}

void ChatFrame::RenderFreeModelStallNotice() {
    wxString text =
        "The free model stopped responding. Free models can lose their "
        "connection from time to time. For a reliable experience, set up "
        "DeepSeek, or just retry. ";
    wxString linkText = "Open Settings";

    Block b;
    b.type = BlockType::Paragraph;
    b.rawText = text + linkText;
    b.visibleText = text + linkText;

    InlineRun r1;
    r1.text = text;
    r1.italic = true;
    b.runs.push_back(r1);

    InlineRun r2;
    r2.text = linkText;
    r2.bold = true;
    r2.link = "gritcode://settings";
    b.runs.push_back(r2);

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
    if (liveThinkingIdx_ >= 0) {
        // The block is already on the canvas: give it the complete reasoning
        // and end live mode, after which it behaves like any Thinking block.
        canvas_->UpdateThinkingBlock(liveThinkingIdx_,
                                     wxString::FromUTF8(activeReasoning_), false);
        liveThinkingIdx_ = -1;
        return;
    }
    if (activeReasoning_.empty()) return;
    RenderThinkingBlock(wxString::FromUTF8(activeReasoning_));
}

void ChatFrame::UpdateLiveThinking() {
    if (thinkingEmitted_ || activeReasoning_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (liveThinkingIdx_ < 0) {
        Block b;
        b.type = BlockType::Thinking;
        b.rawText = wxString::FromUTF8(activeReasoning_);
        b.visibleText = b.rawText;
        b.toolExpanded = false;  // collapsed, like every Thinking block
        b.thinkingLive = true;
        canvas_->AddBlock(std::move(b));
        liveThinkingIdx_ = (int)canvas_->Blocks().size() - 1;
        liveThinkingLastUpdate_ = now;
        liveThinkingLayoutCost_ = {};
        return;
    }
    // A collapsed block only draws its header, so its text can wait until the
    // round finalizes it. When expanded, refresh it in chunks, not per token.
    const auto& blocks = canvas_->Blocks();
    if (liveThinkingIdx_ >= (int)blocks.size()
        || !blocks[liveThinkingIdx_].toolExpanded)
        return;
    // Each refresh re-wraps the whole reasoning, which gets slower as it
    // grows, so the interval grows with it: at least 500 ms, and 20x the last
    // re-layout, keeping layout under ~5% of the UI thread on long runs.
    const auto interval = std::max<std::chrono::steady_clock::duration>(
        std::chrono::milliseconds(500), liveThinkingLayoutCost_ * 20);
    if (now - liveThinkingLastUpdate_ < interval) return;
    canvas_->UpdateThinkingBlock(liveThinkingIdx_,
                                 wxString::FromUTF8(activeReasoning_), true);
    liveThinkingLastUpdate_ = std::chrono::steady_clock::now();
    liveThinkingLayoutCost_ = liveThinkingLastUpdate_ - now;
}

void ChatFrame::DispatchNextQueued() {
    if (pendingQueue_.empty()) return;
    QueuedMessage next = std::move(pendingQueue_.front());
    pendingQueue_.erase(pendingQueue_.begin());
    StartTurn(wxString::FromUTF8(next.text), std::move(next.images));
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
        const QueuedMessage& q = pendingQueue_[i];
        wxString full = wxString::FromUTF8(q.text);
        wxString label = full;
        label.Replace("\n", " ");
        label.Replace("\r", " ");
        label.Replace("\t", " ");
        while (label.Replace("  ", " ")) {}
        label.Trim().Trim(false);
        if (!q.images.empty()) {
            if (label.IsEmpty()) {
                label = FormatU8("[img {}]", q.images.size());
            } else {
                label = FormatU8("[img {}] ", q.images.size()) + label;
            }
        }
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
        std::string entry = q.text;
        close->Bind(wxEVT_LEFT_DOWN, [this, entry](wxMouseEvent&) {
            CallAfter([this, entry]() {
                auto it = std::find_if(
                    pendingQueue_.begin(), pendingQueue_.end(),
                    [&](const QueuedMessage& q) { return q.text == entry; });
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
    int growth = histSize - historyCompactBaseCount_;
    if (growth < 5 && historyCompactBaseCount_ > 0) return false;

    // Select the token-budget tail: the most recent ~kTailBudgetTokens of
    // conversation stays verbatim; everything before it is the head we
    // summarize. This replaces the old "keep 2 user turns" rule (which was
    // unbounded) with OpenCode's bounded preserveRecentBudget.
    int splitIdx = SelectTailSplit();
    // Nothing older than the protected tail to summarize.
    if (splitIdx <= 1) return false;

    // Overflow trigger (OpenCode's isOverflow): compact only when the full
    // rendered view would exceed the context window minus a reserve. This is
    // ~contextWindow (≈980K for DeepSeek) instead of the old 150K head budget,
    // so compaction fires ~7x less often.
    nlohmann::json view = BuildModelView();
    int viewTokens = EstimatePromptTokens(view);
    ModelRoute route =
        CompletionRoute(currentModelIndex_, turnModelIndex_, remoteModels_);
    int usable = route.contextWindow - kBufferTokens;
    if (viewTokens < usable) return false;

    RunSummaryThenSend(splitIdx);
    return true;
}

int ChatFrame::SelectTailSplit() const {
    int histSize = (int)history_.size();
    int tailTokens = 0;
    int splitIdx = histSize;
    for (int i = histSize - 1; i >= 0; --i) {
        const auto& m = history_[i];
        if (!m.is_object()) continue;
        if (m.value("compacted", false)) continue;
        // A summary checkpoint is a head boundary: everything older than it
        // is already represented by that summary, so it must not leak into
        // the verbatim tail.
        if (m.value("isSummary", false)) break;
        if (m.value("role", std::string{}) == "system") break;
        int t = EstimateMessageTokens(m);
        if (tailTokens > 0 && tailTokens + t > kTailBudgetTokens) break;
        tailTokens += t;
        splitIdx = i;
    }
    return splitIdx;
}

bool ChatFrame::ForceCompactForOverflow() {
    // Keep only the most recent user turn verbatim; summarize everything
    // before it. This is more aggressive than the normal 2-turn keep.
    int splitIdx = -1;
    for (int i = (int)history_.size() - 1; i >= 0; --i) {
        if (!history_[i].is_object()) continue;
        if (history_[i].value("compacted", false)) continue;
        if (history_[i].value("role", std::string{}) != "user") continue;
        if (history_[i].value("isSummary", false)) continue;
        splitIdx = i;
        break;
    }
    if (splitIdx <= 1) return false;
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
        "📦 Compacting context - summarizing {} older messages before "
        "continuing. This usually takes a few seconds…",
        compactionHeadCount_));

    // Render the head as plain text. We feed the summary model a flat
    // transcript rather than the raw tool_calls / tool_result structure —
    // (a) it works identically for any wire protocol, and (b) the summary
    // model doesn't need machine-readable tool shape, just what happened.
    //
    // A previous summary checkpoint (isSummary) is carried forward as a
    // dedicated `previousSummary` string rather than dumped inline — inline
    // dumps sit at the FRONT of the head and are the first thing the size cap
    // below truncates away, silently breaking the summary chain. Keeping it
    // separate mirrors OpenCode's buildPrompt({ previousSummary, context }).
    std::string headText;
    headText.reserve(16384);
    std::string previousSummary;
    for (int i = 0; i < splitIdx; ++i) {
        const auto& m = history_[i];
        if (!m.is_object()) continue;
        if (m.value("compacted", false)) continue;  // already summarized
        if (m.value("isSummary", false)) {
            if (m.contains("content") && m["content"].is_string())
                previousSummary = m["content"].get_ref<const std::string&>();
            continue;
        }
        std::string role = m.value("role", std::string{});
        if (role == "system") continue;  // omit our own seed prompt
        headText += "--- " + role + " ---\n";
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
    ModelRoute route =
        CompletionRoute(currentModelIndex_, turnModelIndex_, remoteModels_);
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
        "If a previous summary is provided, UPDATE and EXTEND it with the new "
        "content rather than summarizing from scratch; preserve details already "
        "in the previous summary. Write a compact past-tense narrative. Do not "
        "invent details, do not add a sign-off, do not ask questions. Output "
        "only the summary.";

    std::string summaryUser;
    if (previousSummary.empty()) {
        summaryUser =
            "Summarize this conversation so a fresh session can continue the "
            "work without re-reading it:\n\n" + headText;
    } else {
        summaryUser =
            "Previous summary:\n" + previousSummary +
            "\n\nSummarize the following NEW conversation content since that "
            "summary, and merge it with the previous summary into one updated "
            "summary:\n\n" + headText;
    }

    nlohmann::json req;
    req["model"] = route.model;
    req["stream"] = true;
    req["max_tokens"] = kSummaryMaxTokens;
    if (route.needsApiKey && route.provider == Preferences::Provider::DeepSeek) {
        // Summarization is a mechanical task; keep the budget for the output
        // rather than letting the reasoning model think the tokens away.
        req["thinking"] = {{"type", "disabled"}};
    }
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

#ifndef NDEBUG
    {
        std::string preview = body;
        constexpr size_t kMaxLogBody = 200'000;
        if (preview.size() > kMaxLogBody) {
            preview.resize(kMaxLogBody);
            preview += "\n...[truncated]";
        }
        LogDebug("=== SUMMARY REQUEST body_bytes=" + std::to_string(body.size()) + " ===");
        LogDebug(preview);
    }
#endif

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
  try {
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
    // Trim so whitespace-only/empty summaries count as failures and fall
    // back to the "context dropped" path instead of an empty checkpoint.
    std::string summary = summaryText_;
    while (!summary.empty() && (summary.back() == '\n' || summary.back() == '\r'
                                || summary.back() == ' ' || summary.back() == '\t'))
        summary.pop_back();
    size_t lead = 0;
    while (lead < summary.size() && (summary[lead] == '\n' || summary[lead] == '\r'
                                     || summary[lead] == ' ' || summary[lead] == '\t'))
        ++lead;
    summary = summary.substr(lead);
    ApplyCompaction(!summary.empty(), summary, std::string{});
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[OnSummaryStreamDone] exception: %s\n", e.what());
    throw;
  }
}

void ChatFrame::ApplyCompaction(bool success, const std::string& summary,
                                const std::string& error) {
  try {
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
            "[Prior conversation summary - the earlier turns have been "
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
            "⚠️ Compaction summary failed{} - dropping {} older messages "
            "without a summary so the next request can fit.",
            error.empty() ? std::string{} : (" (" + error + ")"),
            origHeadCount));
    }

    // Mark the head as compacted (hidden from the model view, NEVER deleted
    // from the durable session). Append the summary checkpoint as the new
    // head of the model view. The session file keeps every message.
    std::string ts = SessionStore::NowIso();
    for (int i = 0; i < splitIdx; ++i) {
        if (!history_[i].is_object()) continue;
        history_[i]["compacted"] = true;
        history_[i]["compactedAt"] = ts;
    }

    nlohmann::json summaryMsg = {
        {"role", "assistant"},
        {"content", std::move(summaryBody)},
        {"isSummary", true},
    };

#ifndef NDEBUG
    // Log before moving summaryMsg into history_ — a moved-from json is null
    // and reading its content would throw type_error.
    LogDebug("=== COMPACTION split=" + std::to_string(splitIdx)
             + " hidden=" + std::to_string(origHeadCount)
             + " success=" + (success ? "true" : "false") + " ===");
    LogDebug(summaryMsg["content"].get<std::string>());
#endif

    history_.push_back(std::move(summaryMsg));

    historyCompactBaseCount_ = (int)history_.size();
    PersistActive();

    compacting_ = false;
    DoSendActualRequest();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[ApplyCompaction] exception: %s\n", e.what());
    throw;
  }
}

// ---------------------------------------------------------------------------
// Claude turns
//
// A Claude turn runs the user's own Claude Code CLI:
//   claude -p --input-format stream-json --output-format stream-json ...
// with the user message (text + images) as one JSON line on stdin. Claude
// Code runs its own agent loop and tools; gritcode renders its events:
//   - stream_event text/thinking deltas stream into the canvas,
//   - "assistant" events carry the finished blocks (text, tool_use), which
//     are recorded in history_ as OpenAI-style assistant messages,
//   - "user" events carry tool_results, rendered as tool blocks and recorded
//     as role=tool messages,
//   - "result" ends the turn (is_error for failures).
// Every recorded message is tagged with "model"; assistant messages also get
// "claudeSessionId", and the next Claude turn resumes that session with
// --resume. Whatever other models said in between is handed over as a
// transcript in front of the user's message.
// ---------------------------------------------------------------------------

namespace {

// Largest image sent inline (base64 grows it by a third; the API takes up to
// 5 MB per image). Bigger ones are passed by path for Claude's Read tool.
constexpr size_t kClaudeInlineImageMaxBytes = 3'750'000;
// Cap on a tool result kept in gritcode's copy of the history. Claude Code
// keeps the full output in its own session; ours is for display, search and
// handing context to other models.
constexpr size_t kClaudeToolResultMaxChars = 30'000;

}  // namespace

void ChatFrame::RenderModelSwitchNotice(const std::string& modelId) {
    ModelSwitchTracker tracker;
    for (const auto& m : history_) {
        if (IsUserTurn(m)) tracker.Next(m, false);
    }
    wxString notice = tracker.Next(nlohmann::json{{"model", modelId}}, false);
    if (!notice.IsEmpty()) RenderErrorBlock(notice);
}

void ChatFrame::StartClaudeTurn(bool fresh) {
    claudeTurnHistoryStart_ = history_.size();

    const std::string exe = FindClaudeExecutable();
    if (exe.empty()) {
        wxString text = "Claude Code isn't installed. Install it, sign in by "
                        "running claude in a terminal, then try again. ";
        wxString linkText = "Install Claude Code";
        Block b = NoticeBlock(text);
        InlineRun link;
        link.text = linkText;
        link.bold = true;
        link.link = "https://code.claude.com/docs/en/setup";
        b.runs.push_back(link);
        b.rawText += linkText;
        b.visibleText += linkText;
        canvas_->AddBlock(std::move(b));
        if (!history_.empty() && IsUserTurn(history_.back())) history_.pop_back();
        FinalizeTurn(true);
        return;
    }

    // Resume the latest Claude Code session in this gritcode session. What
    // other models said after it (or the whole conversation, when there is
    // no Claude session yet) goes in front of the message as a transcript.
    const size_t userIdx = history_.size() - 1;
    std::string resumeId;
    size_t since = 0;
    if (!fresh) {
        for (size_t i = userIdx; i-- > 0;) {
            std::string sid = StringField(history_[i], "claudeSessionId");
            if (!sid.empty()) {
                resumeId = sid;
                since = i + 1;
                break;
            }
        }
    }
    std::string text;
    {
        std::string t = HandoverTranscript(history_, since, userIdx,
                                           /*skipClaude=*/!resumeId.empty());
        if (!t.empty()) {
            text = resumeId.empty()
                ? "[This conversation started in gritcode with another model. "
                  "Transcript so far, for context:]\n\n"
                : "[Since your last reply, this gritcode conversation "
                  "continued with another model. What happened since, for "
                  "context:]\n\n";
            text += t + "[End of transcript. The user's new message follows.]\n\n";
        }
    }

    const auto& userMsg = history_[userIdx];
    text += StringField(userMsg, "content");
    nlohmann::json content = nlohmann::json::array();
    if (userMsg.contains("images") && userMsg["images"].is_array()) {
        for (const auto& img : userMsg["images"]) {
            const std::string hash = StringField(img, "sha256");
            std::string mime = StringField(img, "mime");
            if (mime.empty()) mime = "image/png";
            std::string bytes = ImageStore::Load(hash, mime);
            if (bytes.empty()) continue;
            if (bytes.size() > kClaudeInlineImageMaxBytes) {
                text += "\n\n[Attached image too large to send inline: "
                        + ImageStore::PathFor(hash, mime)
                        + " - view it with the Read tool.]";
                continue;
            }
            content.push_back({
                {"type", "image"},
                {"source", {{"type", "base64"},
                            {"media_type", mime},
                            {"data", wxBase64Encode(bytes.data(), bytes.size())
                                         .ToStdString()}}},
            });
        }
    }
    if (!text.empty()) content.push_back({{"type", "text"}, {"text", text}});
    nlohmann::json line = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", std::move(content)}}},
    };

    // Extra context and config go through files: that keeps a large
    // AGENTS.md clear of argv limits and Windows command-line quoting.
    claudeTempFiles_.clear();
    auto writeTemp = [this](const std::string& content) -> std::string {
        wxString tmp = wxFileName::CreateTempFileName("gritclaude");
        if (tmp.empty()) return std::string();
        std::ofstream f(tmp.ToStdString(wxConvUTF8), std::ios::binary | std::ios::trunc);
        f << content;
        f.close();
        if (!f) {
            wxRemoveFile(tmp);
            return std::string();
        }
        claudeTempFiles_.push_back(tmp.ToStdString(wxConvUTF8));
        return claudeTempFiles_.back();
    };

    // Project instructions: Claude Code reads CLAUDE.md itself but not
    // gritcode's AGENTS.md.
    std::string append =
        "You are running inside gritcode, a desktop coding app. The user reads "
        "your replies, rendered as markdown, and your tool calls in gritcode's "
        "chat window.\n\n"
        "Play button: gritcode's \u25B6 button runs one stored shell command "
        "for this project directly, without the AI. Manage it with the "
        "run_project tool (mcp__gritcode__run_project): 'get' shows it, "
        "'detect' suggests one, 'set' stores it (test it with Bash first, and "
        "pass cwd = the project root), 'forget' removes it.";
    const std::string agents = LoadAgentsInstructions(activeCwd_);
    if (!agents.empty()) append += "\n\n" + agents;
    const std::string promptFile = writeTemp(append);

    // gritcode's own stdio MCP server, in its run_project-only mode, so the
    // Play button can be configured from a Claude turn too. It writes the
    // same run config store the Play button reads.
    const nlohmann::json mcpConfig = {
        {"mcpServers", {
            {"gritcode", {
                {"type", "stdio"},
                {"command", wxStandardPaths::Get().GetExecutablePath().utf8_string()},
                {"args", {"--mcp-stdio", "--run-project", activeCwd_}},
            }},
        }},
    };
    const std::string mcpFile = writeTemp(mcpConfig.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace));

    ClaudeRunSpec spec;
    spec.executable = exe;
    spec.cwd = activeCwd_;
    spec.args = {
        "-p",
        "--input-format", "stream-json",
        "--output-format", "stream-json",
        "--verbose",
        "--include-partial-messages",
        "--model", claudeModel_,
        // gritcode's own agent runs its tools without confirmation, and a
        // headless run has nobody to answer a prompt: Claude gets the same.
        "--permission-mode", "bypassPermissions",
    };
    const wxString effort = Preferences::GetClaudeEffort();
    if (!effort.IsEmpty()) {
        spec.args.push_back("--effort");
        spec.args.push_back(effort.utf8_string());
    }
    if (!resumeId.empty()) {
        spec.args.push_back("--resume");
        spec.args.push_back(resumeId);
    }
    if (!promptFile.empty()) {
        spec.args.push_back("--append-system-prompt-file");
        spec.args.push_back(promptFile);
    }
    if (!mcpFile.empty()) {
        spec.args.push_back("--mcp-config");
        spec.args.push_back(mcpFile);
    }
    spec.stdinPayload = line.dump(-1, ' ', false,
                                  nlohmann::json::error_handler_t::replace)
                        + "\n";

    // Reset per-run stream state.
    claudeBuf_.clear();
    claudeSessionId_.clear();
    claudeResumed_ = !resumeId.empty();
    claudeResultSeen_ = false;
    claudeIsError_ = false;
    claudeErrorText_.clear();
    claudeMsgId_.clear();
    claudeStreamMsgId_.clear();
    claudePendingMsg_ = nullptr;
    claudeStreamedMsgIds_.clear();
    claudeToolUses_.clear();
    activeAssistantText_.clear();
    activeReasoning_.clear();
    thinkingEmitted_ = false;
    liveThinkingIdx_ = -1;
    mdStream_ = std::make_unique<MdStream>([this](Block b) {
        canvas_->AddBlock(std::move(b));
    });

    LogDebug("=== CLAUDE model=" + claudeModel_
             + (resumeId.empty() ? std::string(" (new session)")
                                 : " resume=" + resumeId)
             + " stdin_bytes=" + std::to_string(spec.stdinPayload.size())
             + " ===");

    ChdirToCwd(activeCwd_);
    claudeProc_ = ClaudeAgentProcess(
        this, std::move(spec),
        [this](std::string_view chunk) { OnClaudeData(chunk); },
        [this](ClaudeProcessResult res) { OnClaudeDone(std::move(res)); });
}

void ChatFrame::OnClaudeData(std::string_view chunk) {
    claudeBuf_.append(chunk.data(), chunk.size());
    size_t start = 0;
    size_t nl;
    while ((nl = claudeBuf_.find('\n', start)) != std::string::npos) {
        std::string_view line(claudeBuf_.data() + start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() != '{') continue;
        try {
            HandleClaudeEvent(nlohmann::json::parse(line));
        } catch (const std::exception& e) {
            LogDebug(std::string("claude: skipped event: ") + e.what());
        }
    }
    claudeBuf_.erase(0, start);
}

void ChatFrame::HandleClaudeEvent(const nlohmann::json& ev) {
    if (!ev.is_object()) return;
    const std::string type = StringField(ev, "type");

    if (type == "system") {
        if (StringField(ev, "subtype") == "init") {
            claudeSessionId_ = StringField(ev, "session_id");
            LogDebug("claude: session " + claudeSessionId_ + " model "
                     + StringField(ev, "model"));
        }
        return;
    }
    if (type == "result") {
        claudeResultSeen_ = true;
        auto isErr = ev.find("is_error");
        claudeIsError_ = isErr != ev.end() && isErr->is_boolean() && isErr->get<bool>();
        if (claudeSessionId_.empty()) claudeSessionId_ = StringField(ev, "session_id");
        if (claudeIsError_) {
            std::string msg;
            auto errs = ev.find("errors");
            if (errs != ev.end() && errs->is_array()) {
                for (const auto& e : *errs) {
                    if (!e.is_string()) continue;
                    if (!msg.empty()) msg += "\n";
                    msg += e.get<std::string>();
                }
            }
            if (msg.empty()) msg = StringField(ev, "result");
            if (msg.empty()) msg = StringField(ev, "subtype");
            claudeErrorText_ = msg;
        }
        return;
    }

    // Subagent traffic (parent_tool_use_id set) stays inside the Agent tool's
    // own result; only the main conversation is rendered and recorded.
    auto parent = ev.find("parent_tool_use_id");
    if (parent != ev.end() && !parent->is_null()) return;

    if (type == "stream_event") {
        auto eIt = ev.find("event");
        if (eIt == ev.end() || !eIt->is_object()) return;
        const auto& e = *eIt;
        const std::string et = StringField(e, "type");
        if (et == "message_start") {
            // A new API round: settle the previous round's thinking.
            EmitPendingThinking();
            activeReasoning_.clear();
            thinkingEmitted_ = false;
            liveThinkingIdx_ = -1;
            auto msg = e.find("message");
            claudeStreamMsgId_ = msg != e.end() ? StringField(*msg, "id") : std::string();
        } else if (et == "content_block_start") {
            auto block = e.find("content_block");
            if (block != e.end() && StringField(*block, "type") == "tool_use")
                EmitPendingThinking();
        } else if (et == "content_block_delta") {
            auto dIt = e.find("delta");
            if (dIt == e.end() || !dIt->is_object()) return;
            const std::string dt = StringField(*dIt, "type");
            if (dt == "text_delta") {
                const std::string t = StringField(*dIt, "text");
                if (t.empty()) return;
                EmitPendingThinking();
                claudeStreamedMsgIds_.insert(claudeStreamMsgId_);
                activeAssistantText_ += t;
                if (mdStream_) mdStream_->Feed(wxString::FromUTF8(t));
            } else if (dt == "thinking_delta") {
                activeReasoning_ += StringField(*dIt, "thinking");
                UpdateLiveThinking();
            }
        } else if (et == "content_block_stop") {
            // Close the markdown block so a following tool call or text block
            // starts on its own.
            if (mdStream_) mdStream_->Flush();
        }
        return;
    }

    if (type == "assistant") {
        auto msgIt = ev.find("message");
        if (msgIt == ev.end() || !msgIt->is_object()) return;
        const auto& msg = *msgIt;
        // Claude Code reports API failures (unsupported model, auth, limits)
        // as a synthetic assistant message. The result event carries the same
        // error and FailClaudeTurn shows it, so this isn't a reply to keep.
        auto apiErr = ev.find("is_api_error_message");
        if ((apiErr != ev.end() && apiErr->is_boolean() && apiErr->get<bool>())
            || StringField(msg, "model") == "<synthetic>")
            return;
        const std::string id = StringField(msg, "id");
        // Claude Code sends one "assistant" event per finished content block;
        // blocks of the same API message share its id.
        if (id != claudeMsgId_ || !claudePendingMsg_.is_object()) {
            FlushClaudePendingMessage();
            claudeMsgId_ = id;
            claudePendingMsg_ = {{"role", "assistant"},
                                 {"content", ""},
                                 {"model", claudeModel_}};
            if (!claudeSessionId_.empty())
                claudePendingMsg_["claudeSessionId"] = claudeSessionId_;
        }
        auto blocks = msg.find("content");
        if (blocks == msg.end() || !blocks->is_array()) return;
        for (const auto& b : *blocks) {
            const std::string bt = StringField(b, "type");
            if (bt == "text") {
                const std::string t = StringField(b, "text");
                if (t.empty()) continue;
                std::string c = StringField(claudePendingMsg_, "content");
                if (!c.empty()) c += "\n\n";
                claudePendingMsg_["content"] = c + t;
                // Not streamed as deltas (an older CLI, say): render it whole.
                if (!claudeStreamedMsgIds_.count(id) && mdStream_) {
                    EmitPendingThinking();
                    mdStream_->Feed(wxString::FromUTF8(t));
                    mdStream_->Flush();
                }
            } else if (bt == "thinking") {
                const std::string t = StringField(b, "thinking");
                if (t.empty()) continue;
                std::string r = StringField(claudePendingMsg_, "reasoning_content");
                if (!r.empty()) r += "\n\n";
                claudePendingMsg_["reasoning_content"] = r + t;
                if (activeReasoning_.empty() && !thinkingEmitted_) activeReasoning_ = t;
            } else if (bt == "tool_use") {
                const std::string toolId = StringField(b, "id");
                const std::string name = StringField(b, "name");
                auto input = b.find("input");
                std::string args = input != b.end()
                    ? input->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
                    : std::string("{}");
                claudeToolUses_[toolId] = {name, args};
                if (!claudePendingMsg_.contains("tool_calls"))
                    claudePendingMsg_["tool_calls"] = nlohmann::json::array();
                claudePendingMsg_["tool_calls"].push_back({
                    {"id", toolId},
                    {"type", "function"},
                    {"function", {{"name", name}, {"arguments", args}}},
                });
            }
        }
        return;
    }

    if (type == "user") {
        auto msgIt = ev.find("message");
        if (msgIt == ev.end() || !msgIt->is_object()) return;
        auto blocks = msgIt->find("content");
        if (blocks == msgIt->end() || !blocks->is_array()) return;
        for (const auto& b : *blocks) {
            if (StringField(b, "type") != "tool_result") continue;
            // The calls this result answers go into history_ first.
            FlushClaudePendingMessage();
            if (mdStream_) mdStream_->Flush();
            EmitPendingThinking();

            const std::string toolId = StringField(b, "tool_use_id");
            auto rc = b.find("content");
            std::string result = rc != b.end() ? ClaudeToolResultText(*rc) : std::string();
            if (result.size() > kClaudeToolResultMaxChars) {
                result.resize(kClaudeToolResultMaxChars);
                result += "\n[output truncated]";
            }
            auto use = claudeToolUses_.find(toolId);
            const std::string name = use != claudeToolUses_.end() ? use->second.first
                                                                  : std::string("tool");
            const std::string args = use != claudeToolUses_.end() ? use->second.second
                                                                  : std::string();
            RenderToolBlock(name, args, result);
            history_.push_back({
                {"role", "tool"},
                {"tool_call_id", toolId},
                {"name", name},
                {"content", result},
                {"model", claudeModel_},
            });
        }
    }
}

void ChatFrame::FlushClaudePendingMessage() {
    if (!claudePendingMsg_.is_object()) return;
    nlohmann::json msg = std::move(claudePendingMsg_);
    claudePendingMsg_ = nullptr;
    const bool hasTools = msg.contains("tool_calls") && !msg["tool_calls"].empty();
    const bool hasText = !StringField(msg, "content").empty();
    if (!hasText && !hasTools && StringField(msg, "reasoning_content").empty())
        return;
    // OpenAI shape: content is null when a message only calls tools.
    if (!hasText && hasTools) msg["content"] = nullptr;
    history_.push_back(std::move(msg));
}

void ChatFrame::OnClaudeDone(ClaudeProcessResult res) {
    if (!claudeBuf_.empty()) OnClaudeData("\n");  // a final unterminated line
    for (const auto& f : claudeTempFiles_) wxRemoveFile(wxString::FromUTF8(f));
    claudeTempFiles_.clear();
    if (quitRequested_) {
        Close();
        return;
    }

    if (mdStream_) mdStream_->Flush();
    mdStream_.reset();
    EmitPendingThinking();
    FlushClaudePendingMessage();

    LogDebug("=== CLAUDE done exit=" + std::to_string(res.exitCode)
             + (res.cancelled ? " cancelled" : "")
             + (claudeResultSeen_ ? (claudeIsError_ ? " result=error" : " result=ok")
                                  : " no-result")
             + " ===");
    if (!res.stderrTail.empty()) LogDebug("claude stderr: " + res.stderrTail);

    if (!res.started) {
        FailClaudeTurn("couldn't start claude (" + res.error + ")");
        return;
    }
    if (res.cancelled) {
        // SIGINT let Claude Code record the partial turn; what we already
        // recorded stays too, so the next turn resumes consistently.
        RenderErrorBlock("Cancelled.");
        FinalizeTurn(true);
        return;
    }

    std::string detail = claudeErrorText_;
    if (detail.empty() && !claudeResultSeen_) {
        detail = res.stderrTail;
        while (!detail.empty() && std::isspace((unsigned char)detail.back()))
            detail.pop_back();
    }

    // Claude Code prunes old session transcripts; a gritcode session can
    // outlive the Claude session it points at. Start a new one and hand over
    // the whole conversation instead.
    if (claudeResumed_ && !claudeRetriedFresh_
        && detail.find("No conversation found") != std::string::npos) {
        claudeRetriedFresh_ = true;
        LogDebug("claude: session gone, retrying without --resume");
        StartClaudeTurn(/*fresh=*/true);
        return;
    }

    if (claudeResultSeen_ && !claudeIsError_) {
        FinalizeTurn();
        return;
    }
    if (detail.empty())
        detail = "Claude Code exited with status " + std::to_string(res.exitCode) + ".";
    FailClaudeTurn(detail);
}

void ChatFrame::FailClaudeTurn(const std::string& detail) {
    std::string shown = detail;
    if (shown.size() > 1500) shown = shown.substr(0, 1500) + "…";
    std::string lower = detail;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    const bool authProblem = lower.find("login") != std::string::npos
                             || lower.find("log in") != std::string::npos
                             || lower.find("not logged") != std::string::npos
                             || lower.find("authenticat") != std::string::npos
                             || lower.find("api key") != std::string::npos;
    wxString msg;
    if (authProblem) {
        msg = "Claude Code isn't signed in. Run claude in a terminal and sign "
              "in, then try again.\n\n" + wxString::FromUTF8(shown);
    } else {
        msg = "Claude Code error: " + wxString::FromUTF8(shown);
    }
    RenderErrorBlock(msg);

    // Nothing from this turn was recorded: drop the unanswered user message,
    // as the HTTP error path does, so it isn't sent again with the next one.
    if (history_.size() == claudeTurnHistoryStart_ && !history_.empty()
        && IsUserTurn(history_.back()))
        history_.pop_back();
    FinalizeTurn(true);
}
