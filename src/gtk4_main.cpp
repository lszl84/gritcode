// GTK4 WIP frontend for Gritcode
// Incremental migration path from custom GL UI to native GTK rendering.

#include <gtk/gtk.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include "session.h"
#include "curl_http.h"
#include "keychain.h"
#include "tool_exec.h"
#include <nlohmann/json.hpp>
#include <cmark.h>
#include <thread>

#ifdef GRIT_ENABLE_MCP
#include "mcp_server.h"
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

struct MainTask {
    std::function<void()> fn;
};

static gboolean run_main_task(gpointer data) {
    auto* t = static_cast<MainTask*>(data);
    t->fn();
    delete t;
    return G_SOURCE_REMOVE;
}

static void post_main(std::function<void()> fn) {
    g_idle_add_full(G_PRIORITY_DEFAULT, run_main_task, new MainTask{std::move(fn)}, nullptr);
}

struct UiState {
    GtkWidget* window = nullptr;
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* textView = nullptr;
    GtkWidget* input = nullptr;
    GtkWidget* provider = nullptr;  // GtkDropDown
    GtkWidget* model = nullptr;     // GtkDropDown
    GtkWidget* workspace = nullptr; // GtkDropDown
    GtkWidget* status = nullptr;
    GtkWidget* apiKeyBtn = nullptr;
    GtkWidget* chooserBtn = nullptr;
    GtkWidget* sendBtn = nullptr;
    GtkWidget* cancelBtn = nullptr;
    GtkWidget* statusExpander = nullptr;
    GtkWidget* details = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* statusRow = nullptr;
    GtkWidget* inputPlaceholder = nullptr;

#ifdef GRIT_ENABLE_MCP
    MCPServer mcp;
#endif

    GtkStringList* providerList = nullptr;
    GtkStringList* modelList = nullptr;
    GtkStringList* workspaceList = nullptr;

    GtkTextTag* tagUser = nullptr;
    GtkTextTag* tagAssistant = nullptr;
    GtkTextTag* tagMeta = nullptr;
    GtkTextTag* tagError = nullptr;
    GtkTextTag* tagThinking = nullptr;
    GtkTextTag* tagCode = nullptr;
    GtkTextTag* tagInlineCode = nullptr;
    GtkTextTag* tagHeading = nullptr;    // legacy single-level fallback
    GtkTextTag* tagBullet = nullptr;
    GtkTextTag* tagStrong = nullptr;
    GtkTextTag* tagEmph = nullptr;
    GtkTextTag* tagLink = nullptr;
    GtkTextTag* tagQuote = nullptr;
    GtkTextTag* tagH1 = nullptr;
    GtkTextTag* tagH2 = nullptr;
    GtkTextTag* tagH3 = nullptr;
    GtkTextTag* tagH4 = nullptr;

    SessionManager session;
    net::CurlHttpClient http;
    std::vector<std::string> workspaceCwds;
    bool mutatingWorkspace = false;
    bool streaming = false;
    bool alive = true;

    // Tool loop. toolRound caps iterations so a confused model can't hang
    // us forever. requestGen is bumped on cancel so stale finalizers from
    // an in-flight request can detect they've been replaced and drop
    // their work instead of appending to a new conversation.
    int toolRound = 0;
    std::atomic<uint64_t> requestGen{0};

    // Currently-open streaming thinking section, if any. Non-null means
    // the next `thinking=true` chunk should append to the already-opened
    // block instead of starting a new one.
    GtkTextTag* activeThinkingTag = nullptr;  // per-section invisible tag

    // Scroll anchoring — see scroll_to_end and on_vadj_* handlers.
    GtkWidget* scrolledWindow = nullptr;
    GtkAdjustment* vadj = nullptr;
    bool stickToBottom = true;      // Auto-follow new content until user scrolls up.
    bool internalScroll = false;    // Guard: ignore value-changed from our own snap.
};

static std::string current_cwd() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return ".";
}

static void set_status(UiState* s, const std::string& t) {
    gtk_label_set_text(GTK_LABEL(s->status), t.c_str());
}

static void set_busy(UiState* s, bool busy) {
    if (!s->spinner) return;
    gtk_widget_set_visible(s->spinner, busy);
    if (busy) gtk_spinner_start(GTK_SPINNER(s->spinner));
    else      gtk_spinner_stop(GTK_SPINNER(s->spinner));
}

static void set_details(UiState* s, const std::string& t) {
    if (!s->details) return;
    gtk_label_set_text(GTK_LABEL(s->details), t.c_str());
}

static const char* dropdown_selected(GtkWidget* dd, GtkStringList* list) {
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!list) return nullptr;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
    if (idx >= n) return nullptr;
    return gtk_string_list_get_string(list, idx);
}

static int list_index_of(GtkStringList* list, const std::string& value) {
    if (!list) return -1;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
    for (guint i = 0; i < n; ++i) {
        const char* s = gtk_string_list_get_string(list, i);
        if (s && value == s) return (int)i;
    }
    return -1;
}

static bool adj_at_bottom(GtkAdjustment* a) {
    if (!a) return true;
    double upper = gtk_adjustment_get_upper(a);
    double page  = gtk_adjustment_get_page_size(a);
    double val   = gtk_adjustment_get_value(a);
    return (upper - page - val) < 2.0;
}

static void snap_to_bottom(UiState* s) {
    if (!s || !s->vadj) return;
    double upper = gtk_adjustment_get_upper(s->vadj);
    double page  = gtk_adjustment_get_page_size(s->vadj);
    s->internalScroll = true;
    gtk_adjustment_set_value(s->vadj, upper - page);
    s->internalScroll = false;
}

// Park the insert cursor at the start of the buffer so GtkTextView does not
// auto-scroll to follow the cursor on allocation/reflow.
static void park_cursor(UiState* s) {
    if (!s || !s->buffer) return;
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(s->buffer, &start);
    gtk_text_buffer_place_cursor(s->buffer, &start);
}

static void scroll_to_end(UiState* s) {
    if (!s) return;
    park_cursor(s);
    if (!s->stickToBottom) return;
    snap_to_bottom(s);
}

// Emitted when the USER scrolls (or we scroll ourselves — guarded).
// Update stickToBottom to match whether the viewport currently rests at bottom.
static void on_vadj_value_changed(GtkAdjustment* adj, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (s->internalScroll) return;
    s->stickToBottom = adj_at_bottom(adj);
}

// Emitted when the adjustment's bounds change (content growth OR resize).
// Re-pin to bottom only if the user was already there.
static void on_vadj_changed(GtkAdjustment* adj, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (!s->stickToBottom) return;
    double upper = gtk_adjustment_get_upper(adj);
    double page  = gtk_adjustment_get_page_size(adj);
    s->internalScroll = true;
    gtk_adjustment_set_value(adj, upper - page);
    s->internalScroll = false;
}

static void append_block(UiState* s, const char* role, const char* text, GtkTextTag* bodyTag) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);

    std::string hdr = role;
    hdr += "\n";
    gtk_text_buffer_insert_with_tags(s->buffer, &end, hdr.c_str(), -1, s->tagMeta, nullptr);

    gtk_text_buffer_insert_with_tags(s->buffer, &end, text, -1, bodyTag, nullptr);
    gtk_text_buffer_insert(s->buffer, &end, "\n\n", -1);

    scroll_to_end(s);
}

static void append_assistant_header(UiState* s) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert_with_tags(s->buffer, &end, "Assistant\n", -1, s->tagMeta, nullptr);
}

// Close any currently-open thinking section. After this, further
// thinking chunks will start a fresh collapsible block.
static void close_thinking_section(UiState* s) {
    s->activeThinkingTag = nullptr;
}

static void on_thinking_expander_toggled(GObject* obj, GParamSpec*, gpointer data) {
    auto* tag = static_cast<GtkTextTag*>(data);
    gboolean expanded = gtk_expander_get_expanded(GTK_EXPANDER(obj));
    g_object_set(tag, "invisible", expanded ? FALSE : TRUE, nullptr);
}

// Open a fresh collapsible thinking section. Inserts an inline
// GtkExpander as a child widget at the current buffer end; subsequent
// thinking chunks go into a per-section invisible-by-default tag. The
// user clicks the expander to reveal.
static void open_thinking_section(UiState* s) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);

    // Per-section tag: independent `invisible` property so each block
    // can be toggled on its own. Anonymous tag (no name) to avoid
    // collisions across blocks.
    auto* tag = gtk_text_tag_new(nullptr);
    g_object_set(tag, "invisible", TRUE,
                      "foreground", "#7EC7FF",
                      "style", PANGO_STYLE_ITALIC,
                      "family", "monospace",
                      nullptr);
    gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(s->buffer), tag);
    g_object_unref(tag);  // table holds the only strong ref we need

    auto* anchor = gtk_text_buffer_create_child_anchor(s->buffer, &end);

    auto* expander = gtk_expander_new("▸ Thinking");
    gtk_expander_set_expanded(GTK_EXPANDER(expander), FALSE);
    g_signal_connect(expander, "notify::expanded",
                     G_CALLBACK(on_thinking_expander_toggled), tag);

    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(s->textView), expander, anchor);

    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert(s->buffer, &end, "\n", 1);

    s->activeThinkingTag = tag;
}

static void append_stream_chunk(UiState* s, const std::string& text, bool thinking) {
    if (thinking && !s->activeThinkingTag) open_thinking_section(s);
    if (!thinking && s->activeThinkingTag) close_thinking_section(s);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    if (thinking) {
        // Tag text with both the base tagThinking (for style when visible)
        // and the per-section invisible tag (for toggleable visibility).
        gtk_text_buffer_insert_with_tags(s->buffer, &end, text.c_str(), -1,
                                         s->tagThinking, s->activeThinkingTag, nullptr);
    } else {
        gtk_text_buffer_insert_with_tags(s->buffer, &end, text.c_str(), -1,
                                         s->tagAssistant, nullptr);
    }
    scroll_to_end(s);
}

static void append_stream_footer(UiState* s) {
    close_thinking_section(s);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert(s->buffer, &end, "\n\n", -1);
    scroll_to_end(s);
}

static void clear_transcript(UiState* s) {
    gtk_text_buffer_set_text(s->buffer, "", -1);
}

// Render a markdown document into the text buffer at the end iterator.
// Walks the cmark tree with an iterator and maintains a stack of active
// inline tags so TEXT/CODE/SOFTBREAK leaves pick up the right styling
// from surrounding STRONG/EMPH/LINK/HEADING/BLOCK_QUOTE nodes.
static void render_markdown(UiState* s, GtkTextIter* end, const std::string& text) {
    cmark_node* doc = cmark_parse_document(text.c_str(), text.size(), CMARK_OPT_DEFAULT);
    if (!doc) {
        gtk_text_buffer_insert_with_tags(s->buffer, end, text.c_str(), -1, s->tagAssistant, nullptr);
        return;
    }

    struct ListFrame { bool ordered; int nextIndex; };
    std::vector<ListFrame> lists;
    std::vector<GtkTextTag*> inlineStack;    // STRONG/EMPH/LINK
    GtkTextTag* const blockTag = s->tagAssistant;  // base tag for paragraph-level text
    int quoteDepth = 0;
    int headingLevel = 0;

    auto active_tag = [&]() -> GtkTextTag* {
        if (!inlineStack.empty()) return inlineStack.back();
        if (headingLevel == 1) return s->tagH1;
        if (headingLevel == 2) return s->tagH2;
        if (headingLevel == 3) return s->tagH3;
        if (headingLevel >= 4) return s->tagH4;
        if (quoteDepth > 0) return s->tagQuote;
        return blockTag;
    };

    auto insert = [&](const char* txt, int len = -1) {
        if (!txt || !*txt) return;
        gtk_text_buffer_insert_with_tags(s->buffer, end, txt, len, active_tag(), nullptr);
    };

    cmark_iter* iter = cmark_iter_new(doc);
    cmark_event_type ev;
    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node* node = cmark_iter_get_node(iter);
        cmark_node_type t = cmark_node_get_type(node);
        bool enter = (ev == CMARK_EVENT_ENTER);

        switch (t) {
        case CMARK_NODE_DOCUMENT: break;

        case CMARK_NODE_PARAGRAPH:
            if (!enter) gtk_text_buffer_insert(s->buffer, end, "\n\n", 2);
            break;

        case CMARK_NODE_HEADING:
            if (enter) headingLevel = cmark_node_get_heading_level(node);
            else {
                gtk_text_buffer_insert(s->buffer, end, "\n\n", 2);
                headingLevel = 0;
            }
            break;

        case CMARK_NODE_BLOCK_QUOTE:
            if (enter) ++quoteDepth;
            else       --quoteDepth;
            break;

        case CMARK_NODE_LIST:
            if (enter) {
                bool ordered = (cmark_node_get_list_type(node) == CMARK_ORDERED_LIST);
                int start = ordered ? cmark_node_get_list_start(node) : 0;
                lists.push_back({ordered, start ? start : 1});
            } else {
                if (!lists.empty()) lists.pop_back();
            }
            break;

        case CMARK_NODE_ITEM:
            if (enter && !lists.empty()) {
                auto& lf = lists.back();
                std::string indent((lists.size() - 1) * 2, ' ');
                if (lf.ordered) {
                    std::string marker = indent + std::to_string(lf.nextIndex++) + ". ";
                    gtk_text_buffer_insert_with_tags(s->buffer, end, marker.c_str(), -1, s->tagBullet, nullptr);
                } else {
                    std::string marker = indent + "• ";
                    gtk_text_buffer_insert_with_tags(s->buffer, end, marker.c_str(), -1, s->tagBullet, nullptr);
                }
            }
            break;

        case CMARK_NODE_CODE_BLOCK:
            if (enter) {
                const char* lit = cmark_node_get_literal(node);
                if (lit) {
                    std::string body = lit;
                    while (!body.empty() && body.back() == '\n') body.pop_back();
                    gtk_text_buffer_insert_with_tags(s->buffer, end, body.c_str(), -1, s->tagCode, nullptr);
                }
                gtk_text_buffer_insert(s->buffer, end, "\n\n", 2);
            }
            break;

        case CMARK_NODE_THEMATIC_BREAK:
            if (enter) gtk_text_buffer_insert_with_tags(s->buffer, end, "──────────\n\n", -1, s->tagMeta, nullptr);
            break;

        case CMARK_NODE_TEXT: {
            const char* lit = cmark_node_get_literal(node);
            if (lit) insert(lit);
            break;
        }

        case CMARK_NODE_SOFTBREAK:
        case CMARK_NODE_LINEBREAK:
            if (enter) insert("\n");
            break;

        case CMARK_NODE_CODE: {
            if (!enter) break;
            const char* lit = cmark_node_get_literal(node);
            if (lit) gtk_text_buffer_insert_with_tags(s->buffer, end, lit, -1, s->tagInlineCode, nullptr);
            break;
        }

        case CMARK_NODE_EMPH:
            if (enter) inlineStack.push_back(s->tagEmph);
            else if (!inlineStack.empty()) inlineStack.pop_back();
            break;

        case CMARK_NODE_STRONG:
            if (enter) inlineStack.push_back(s->tagStrong);
            else if (!inlineStack.empty()) inlineStack.pop_back();
            break;

        case CMARK_NODE_LINK:
            if (enter) inlineStack.push_back(s->tagLink);
            else if (!inlineStack.empty()) {
                inlineStack.pop_back();
                // Show the URL inline after the link text if it differs,
                // so the user can see where a link points.
                const char* url = cmark_node_get_url(node);
                if (url && *url) {
                    std::string suffix = " (";
                    suffix += url;
                    suffix += ")";
                    gtk_text_buffer_insert_with_tags(s->buffer, end, suffix.c_str(), -1, s->tagMeta, nullptr);
                }
            }
            break;

        case CMARK_NODE_IMAGE:
            if (enter) inlineStack.push_back(s->tagEmph);
            else if (!inlineStack.empty()) inlineStack.pop_back();
            break;

        case CMARK_NODE_HTML_BLOCK:
        case CMARK_NODE_HTML_INLINE:
            // Skip raw HTML — too noisy in a text transcript.
            break;

        default:
            break;
        }
    }

    cmark_iter_free(iter);
    cmark_node_free(doc);
}

static void append_assistant_markdown(UiState* s, const std::string& text) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert_with_tags(s->buffer, &end, "Assistant\n", -1, s->tagMeta, nullptr);

    render_markdown(s, &end, text);

    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
    scroll_to_end(s);
}

static void append_tool_result(UiState* s, const ChatMessage& m) {
    std::string header = m.toolCallId.empty() ? "Tool Result" : ("Tool Result [" + m.toolCallId + "]");
    append_block(s, header.c_str(), "", s->tagThinking);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    std::string body = m.content;
    const size_t MAX = 16000;
    if (body.size() > MAX) {
        body.resize(MAX);
        body += "\n\n[truncated]";
    }
    gtk_text_buffer_insert_with_tags(s->buffer, &end, body.c_str(), -1, s->tagCode, nullptr);
    gtk_text_buffer_insert(s->buffer, &end, "\n\n", 2);
    scroll_to_end(s);
}

static void append_from_message(UiState* s, const ChatMessage& m) {
    if (m.role == "user") append_block(s, "You", m.content.c_str(), s->tagUser);
    else if (m.role == "assistant") append_assistant_markdown(s, m.content);
    else if (m.role == "tool") append_tool_result(s, m);
    else append_block(s, m.role.c_str(), m.content.c_str(), s->tagAssistant);
}

static void sync_provider_model_from_session(UiState* s) {
    int p = list_index_of(s->providerList, s->session.Provider());
    if (p >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(s->provider), (guint)p);
    int m = list_index_of(s->modelList, s->session.Model());
    if (m >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(s->model), (guint)m);
}

static void session_save_from_ui(UiState* s) {
    const char* p = dropdown_selected(s->provider, s->providerList);
    const char* m = dropdown_selected(s->model, s->modelList);
    if (p) s->session.SetProvider(p);
    if (m) s->session.SetModel(m);
    s->session.Save();
}

static void reload_workspaces(UiState* s) {
    s->mutatingWorkspace = true;
    s->workspaceCwds.clear();

    auto* newList = gtk_string_list_new(nullptr);

    std::vector<SessionInfo> all = SessionManager::ListSessions();
    std::sort(all.begin(), all.end(), [](const SessionInfo& a, const SessionInfo& b) {
        return a.lastUsed > b.lastUsed;
    });

    std::string cwd = current_cwd();
    gtk_string_list_append(newList, cwd.c_str());
    s->workspaceCwds.push_back(cwd);

    for (const auto& si : all) {
        if (si.cwd == cwd) continue;
        gtk_string_list_append(newList, si.cwd.c_str());
        s->workspaceCwds.push_back(si.cwd);
    }

    s->workspaceList = newList;
    gtk_drop_down_set_model(GTK_DROP_DOWN(s->workspace), G_LIST_MODEL(s->workspaceList));

    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->workspace), 0);
    s->mutatingWorkspace = false;
}

static std::string short_cwd(const std::string& cwd) {
    const char* home = getenv("HOME");
    if (home && cwd.rfind(home, 0) == 0) {
        return "~" + cwd.substr(strlen(home));
    }
    return cwd;
}

static void update_window_title(UiState* s) {
    if (!s->window) return;
    std::string title = "Grit — " + short_cwd(current_cwd());
    gtk_window_set_title(GTK_WINDOW(s->window), title.c_str());
}

static void load_session_for_cwd(UiState* s, const std::string& cwd) {
    s->session.SetCwd(cwd);
    s->session.LoadForCwd(cwd);

    clear_transcript(s);
    for (const auto& m : s->session.History()) append_from_message(s, m);

    sync_provider_model_from_session(s);

    std::string st = "Workspace: ";
    st += cwd;
    st += "  | Messages: ";
    st += std::to_string(s->session.History().size());
    set_status(s, st);
    set_details(s, "Provider: " + s->session.Provider() + "\nModel: " + s->session.Model());
    update_window_title(s);
}

static void configure_http(UiState* s) {
    const char* provider = dropdown_selected(s->provider, s->providerList);
    if (!provider) provider = "zen";

    std::string baseUrl = (std::string(provider) == "opencode-go")
        ? "https://opencode.ai/zen/go/v1"
        : "https://opencode.ai/zen/v1";

    s->http.SetBaseUrl(baseUrl);
    std::string api = keychain::LoadApiKey();
    s->http.SetApiKey(api);
}

static void replace_models(UiState* s, const std::vector<net::ModelInfo>& models) {
    std::string keep;
    if (const char* cur = dropdown_selected(s->model, s->modelList)) keep = cur;

    auto* newList = gtk_string_list_new(nullptr);
    if (models.empty()) {
        gtk_string_list_append(newList, "kimi-k2.5");
    } else {
        for (const auto& m : models) {
            gtk_string_list_append(newList, m.id.c_str());
        }
    }

    s->modelList = newList;
    gtk_drop_down_set_model(GTK_DROP_DOWN(s->model), G_LIST_MODEL(s->modelList));

    int idx = keep.empty() ? -1 : list_index_of(s->modelList, keep);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->model), (idx >= 0) ? (guint)idx : 0);
}

static void fetch_models(UiState* s) {
    set_status(s, "Loading models...");
    set_busy(s, true);
    s->http.FetchModels([s](std::vector<net::ModelInfo> models, int status) {
        post_main([s, models = std::move(models), status]() mutable {
            if (!s->alive) return;
            if (!s->streaming) set_busy(s, false);
            replace_models(s, models);
            if (status == 200) set_status(s, "Models loaded");
            else if (status == 401) set_status(s, "Invalid API key (401)");
            else set_status(s, "Model fetch failed");

            std::string detail = "HTTP: " + std::to_string(status) + "\nModels: " + std::to_string(models.size());
            if (!models.empty()) {
                detail += "\nFirst: " + models.front().id;
            }
            set_details(s, detail);
        });
    });
}

struct ApiKeyDialogCtx {
    UiState* state;
    GtkWidget* entry;
};

static void on_api_save_clicked(GtkButton*, gpointer user_data) {
    auto* w = GTK_WINDOW(user_data);
    auto* ctx = static_cast<ApiKeyDialogCtx*>(g_object_get_data(G_OBJECT(w), "api-ctx"));
    if (!ctx) return;

    const char* key = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
    if (key && *key) {
        keychain::SaveApiKey(key);
        set_status(ctx->state, "API key saved");
    } else {
        keychain::ClearApiKey();
        set_status(ctx->state, "API key cleared");
    }
    configure_http(ctx->state);
    fetch_models(ctx->state);
    gtk_window_destroy(w);
}

static void set_api_key(UiState* s) {
    auto* parent = GTK_WINDOW(gtk_widget_get_root(s->apiKeyBtn));
    auto* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Set API Key");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_default_size(GTK_WINDOW(win), 420, -1);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    auto* entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    std::string cur = keychain::LoadApiKey();
    if (!cur.empty()) gtk_editable_set_text(GTK_EDITABLE(entry), cur.c_str());

    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* clearBtn = gtk_button_new_with_label("Clear");
    auto* cancelBtn = gtk_button_new_with_label("Cancel");
    auto* saveBtn = gtk_button_new_with_label("Save");

    gtk_box_append(GTK_BOX(row), clearBtn);
    gtk_box_append(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(row), cancelBtn);
    gtk_box_append(GTK_BOX(row), saveBtn);

    gtk_box_append(GTK_BOX(root), entry);
    gtk_box_append(GTK_BOX(root), row);
    gtk_window_set_child(GTK_WINDOW(win), root);

    g_signal_connect(clearBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        auto* st = static_cast<UiState*>(user_data);
        keychain::ClearApiKey();
        configure_http(st);
        fetch_models(st);
        set_status(st, "API key cleared");
    }), s);

    g_signal_connect(cancelBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        gtk_window_destroy(GTK_WINDOW(user_data));
    }), win);

    auto* ctx = new ApiKeyDialogCtx{s, entry};
    g_object_set_data_full(G_OBJECT(win), "api-ctx", ctx, +[](gpointer p) {
        delete static_cast<ApiKeyDialogCtx*>(p);
    });

    g_signal_connect(saveBtn, "clicked", G_CALLBACK(on_api_save_clicked), win);

    gtk_window_present(GTK_WINDOW(win));
}

static net::CurlHttpClient::Protocol protocol_for_model(const std::string& model) {
    std::string m = model;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);
    if (m.find("claude") != std::string::npos) return net::CurlHttpClient::Protocol::Anthropic;
    return net::CurlHttpClient::Protocol::OpenAI;
}

static std::string system_prompt() {
    char cwdBuf[4096];
    std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf)) ? cwdBuf : ".";
    return
        "You are an AI coding assistant. You help the user with software engineering tasks "
        "by reading, writing, and editing files, and running shell commands.\n\n"
        "Working directory: " + cwd + "\n\n"
        "## Tools\n"
        "You have these tools: bash, read_file, write_file, edit_file, list_directory, web_fetch, web_search.\n"
        "- Use write_file to create new files or overwrite existing ones.\n"
        "- Use edit_file to make targeted changes — provide enough context in old_string to be unique.\n"
        "- Use read_file to read files before editing them.\n"
        "- Use bash to run commands, tests, install packages, etc.\n\n"
        "## Behavior\n"
        "- When given a task, work through it step by step using your tools.\n"
        "- Do not just describe what you would do — actually do it by calling tools.\n"
        "- After each tool result, assess whether the task is complete and continue if not.\n"
        "- When making changes, verify they work (e.g., run tests or the build).\n"
        "- Keep responses concise. Lead with actions, not explanations.\n";
}

// Compute a prune cutoff: tool-result messages at index <= cutoff get
// replaced with a placeholder on the wire. Protects roughly the most
// recent PROTECT_CHARS of tool output so the context doesn't blow up
// across a long session with many large tool results.
static int prune_before_idx(const std::vector<ChatMessage>& hist) {
    const size_t PROTECT_CHARS = 120000;
    size_t acc = 0;
    for (int i = (int)hist.size() - 1; i >= 0; --i) {
        if (hist[i].role == "tool") {
            acc += hist[i].content.size();
            if (acc > PROTECT_CHARS) return i;
        }
    }
    return -1;
}

static std::string build_openai_request(UiState* s, const std::string& model) {
    json j;
    j["model"] = model;
    j["stream"] = true;
    j["max_tokens"] = 32000;

    const auto& hist = s->session.History();
    int cutoff = prune_before_idx(hist);

    json msgs = json::array();
    msgs.push_back({{"role", "system"}, {"content", system_prompt()}});

    for (int i = 0; i < (int)hist.size(); ++i) {
        const auto& m = hist[i];
        json msg;
        msg["role"] = m.role;
        if (m.role == "tool") {
            msg["tool_call_id"] = m.toolCallId;
            msg["content"] = (i <= cutoff) ? std::string("[Old tool result content cleared]") : m.content;
        } else if (m.role == "assistant" && !m.toolCalls.empty()) {
            msg["content"] = m.content.empty() ? json(nullptr) : json(m.content);
            json tcs = json::array();
            for (auto& tc : m.toolCalls) {
                tcs.push_back({{"id", tc.value("id", "")}, {"type", "function"},
                               {"function", {{"name", tc.value("name", "")},
                                             {"arguments", tc.value("arguments", "")}}}});
            }
            msg["tool_calls"] = std::move(tcs);
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(std::move(msg));
    }
    j["messages"] = std::move(msgs);
    j["tools"] = json::parse(toolexec::ToolDefsJson());
    j["tool_choice"] = "auto";
    return j.dump();
}

static std::string build_anthropic_request(UiState* s, const std::string& model) {
    json j;
    j["model"] = model;
    j["stream"] = true;
    j["max_tokens"] = 32000;
    j["system"] = system_prompt();

    const auto& hist = s->session.History();
    int cutoff = prune_before_idx(hist);

    json msgs = json::array();
    auto pushOrMerge = [&](json msg) {
        if (!msgs.empty() && msgs.back()["role"] == msg["role"] && msg["role"] == "user") {
            auto& last = msgs.back();
            auto toArray = [](json& c) {
                if (c.is_string()) {
                    json arr = json::array();
                    arr.push_back({{"type","text"},{"text",c.get<std::string>()}});
                    c = std::move(arr);
                }
            };
            toArray(last["content"]);
            toArray(msg["content"]);
            for (auto& blk : msg["content"]) last["content"].push_back(blk);
        } else {
            msgs.push_back(std::move(msg));
        }
    };

    for (int i = 0; i < (int)hist.size(); ++i) {
        const auto& m = hist[i];
        json msg;
        if (m.role == "user") {
            msg["role"] = "user";
            msg["content"] = m.content;
        } else if (m.role == "assistant") {
            msg["role"] = "assistant";
            if (m.toolCalls.empty()) {
                if (m.content.empty()) continue;
                msg["content"] = m.content;
            } else {
                json blocks = json::array();
                if (!m.content.empty()) blocks.push_back({{"type","text"},{"text",m.content}});
                for (auto& tc : m.toolCalls) {
                    json tu;
                    tu["type"] = "tool_use";
                    tu["id"]   = tc.value("id", "");
                    tu["name"] = tc.value("name", "");
                    try { tu["input"] = json::parse(tc.value("arguments", std::string("{}"))); }
                    catch (...) { tu["input"] = json::object(); }
                    blocks.push_back(std::move(tu));
                }
                msg["content"] = std::move(blocks);
            }
        } else if (m.role == "tool") {
            msg["role"] = "user";
            json tr;
            tr["type"] = "tool_result";
            tr["tool_use_id"] = m.toolCallId;
            tr["content"] = (i <= cutoff) ? std::string("[Old tool result content cleared]") : m.content;
            json blocks = json::array();
            blocks.push_back(std::move(tr));
            msg["content"] = std::move(blocks);
        } else {
            continue;
        }
        pushOrMerge(std::move(msg));
    }
    j["messages"] = std::move(msgs);

    // Convert OpenAI-shape tool defs to Anthropic's form to keep one source of truth.
    json openaiTools = json::parse(toolexec::ToolDefsJson());
    json anthTools = json::array();
    for (auto& t : openaiTools) {
        if (!t.contains("function")) continue;
        json at;
        at["name"] = t["function"].value("name", "");
        at["description"] = t["function"].value("description", "");
        at["input_schema"] = t["function"].value("parameters", json::object());
        anthTools.push_back(std::move(at));
    }
    j["tools"] = std::move(anthTools);
    j["tool_choice"] = {{"type","auto"}};
    return j.dump();
}

static void append_tool_calls(UiState* s, const std::vector<json>& toolCalls) {
    for (const auto& tc : toolCalls) {
        std::string name = tc.value("name", "tool");
        std::string args = tc.value("arguments", "{}");
        std::string body = "name: " + name + "\narguments: " + args;
        append_block(s, "Tool Call", body.c_str(), s->tagThinking);
    }
}

struct ChooserCtx {
    UiState* state;
    GtkWidget* list;
};

static void chooser_activate_row(GtkWindow* win) {
    auto* ctx = static_cast<ChooserCtx*>(g_object_get_data(G_OBJECT(win), "chooser-ctx"));
    if (!ctx) return;
    auto* s = ctx->state;
    auto* list = GTK_LIST_BOX(ctx->list);
    auto* row = gtk_list_box_get_selected_row(list);
    if (!row) { gtk_window_destroy(win); return; }

    const char* cwd = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "cwd"));
    if (!cwd) { gtk_window_destroy(win); return; }

    std::error_code ec;
    if (fs::exists(cwd, ec) && fs::is_directory(cwd, ec)) {
        if (chdir(cwd) == 0) {
            load_session_for_cwd(s, cwd);
            reload_workspaces(s);
            set_status(s, std::string("Workspace: ") + cwd);
        } else {
            append_block(s, "Error", "Failed to switch to selected workspace.", s->tagError);
        }
    } else {
        append_block(s, "Error", "Selected workspace does not exist.", s->tagError);
    }
    gtk_window_destroy(win);
}

static void on_chooser_open_clicked(GtkButton*, gpointer user_data) {
    chooser_activate_row(GTK_WINDOW(user_data));
}

static void on_chooser_row_activated(GtkListBox*, GtkListBoxRow*, gpointer user_data) {
    chooser_activate_row(GTK_WINDOW(user_data));
}

static void open_workspace_chooser(UiState* s) {
    auto* parent = GTK_WINDOW(gtk_widget_get_root(s->workspace));
    auto* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Select workspace");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 520);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);

    auto* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    auto* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);

    std::vector<SessionInfo> all = SessionManager::ListSessions();
    std::sort(all.begin(), all.end(), [](const SessionInfo& a, const SessionInfo& b) {
        return a.lastUsed > b.lastUsed;
    });

    std::string cwd = current_cwd();
    auto add_row = [&](const std::string& path, const std::string& info) {
        auto* row = gtk_list_box_row_new();
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        auto* l1 = gtk_label_new(path.c_str());
        auto* l2 = gtk_label_new(info.c_str());
        gtk_label_set_xalign(GTK_LABEL(l1), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(l2), 0.0f);
        gtk_widget_add_css_class(l2, "dim-label");
        gtk_box_append(GTK_BOX(box), l1);
        gtk_box_append(GTK_BOX(box), l2);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data_full(G_OBJECT(row), "cwd", g_strdup(path.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    };

    add_row(cwd, "Current workspace");
    for (const auto& si : all) {
        if (si.cwd == cwd) continue;
        std::string meta = si.provider + " / " + si.model + "  ·  " + std::to_string(si.messageCount) + " messages";
        add_row(si.cwd, meta);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);

    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* cancel = gtk_button_new_with_label("Cancel");
    auto* open = gtk_button_new_with_label("Open");
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), open);

    gtk_box_append(GTK_BOX(root), sw);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(win), root);

    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer w) {
        gtk_window_destroy(GTK_WINDOW(w));
    }), win);

    auto* ctx = new ChooserCtx{s, list};
    g_object_set_data_full(G_OBJECT(win), "chooser-ctx", ctx, +[](gpointer p) {
        delete static_cast<ChooserCtx*>(p);
    });

    g_signal_connect(open, "clicked", G_CALLBACK(on_chooser_open_clicked), win);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_chooser_row_activated), win);

    gtk_window_present(GTK_WINDOW(win));
}

static void on_workspace_changed(GtkDropDown*, GParamSpec*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    if (s->mutatingWorkspace || s->streaming) return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(s->workspace));
    if (idx >= s->workspaceCwds.size()) return;
    std::string cwd = s->workspaceCwds[idx];

    std::error_code ec;
    if (fs::exists(cwd, ec) && fs::is_directory(cwd, ec)) {
        if (chdir(cwd.c_str()) != 0) {
            append_block(s, "Error", "Failed to switch working directory.", s->tagError);
            return;
        }
        load_session_for_cwd(s, cwd);
    } else {
        append_block(s, "Error", "Selected workspace no longer exists on disk.", s->tagError);
    }
}

static void on_provider_or_model_changed(GtkDropDown* which, GParamSpec*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    session_save_from_ui(s);
    configure_http(s);
    if (which == GTK_DROP_DOWN(s->provider)) {
        fetch_models(s);
    }
}

static void set_controls_enabled(UiState* s, bool enabled) {
    gtk_widget_set_sensitive(s->input, enabled);
    gtk_widget_set_sensitive(s->provider, enabled);
    gtk_widget_set_sensitive(s->model, enabled);
    gtk_widget_set_sensitive(s->workspace, enabled);
    gtk_widget_set_sensitive(s->chooserBtn, enabled);
    gtk_widget_set_sensitive(s->sendBtn, enabled);
}

static void set_streaming(UiState* s, bool streaming) {
    s->streaming = streaming;
    gtk_widget_set_visible(s->cancelBtn, streaming);
    set_controls_enabled(s, !streaming);
    set_busy(s, streaming);
}

// Bump the generation counter, abort the HTTP stream, kill any running
// bash tool, and patch up the history so the next user send is wire-valid.
// Mirrors app.cpp's CancelInFlight.
static void cancel_in_flight(UiState* s) {
    if (!s->streaming) return;
    s->requestGen.fetch_add(1);
    s->http.Abort();
    toolexec::KillRunningTool();

    auto& hist = s->session.History();
    if (!hist.empty()) {
        auto& last = hist.back();
        if (last.role == "assistant" && !last.toolCalls.empty()) {
            // Add synthetic tool_result[cancelled] for each pending tool_use
            // so the conversation passed on next send has matching pairs.
            for (auto& tc : last.toolCalls) {
                ChatMessage tr;
                tr.role = "tool";
                tr.content = "[cancelled by user]";
                tr.toolCallId = tc.value("id", "");
                hist.push_back(std::move(tr));
            }
            hist.push_back({"assistant", "[cancelled]", {}, {}});
        } else if (last.role == "user") {
            hist.push_back({"assistant", "[cancelled]", {}, {}});
        }
    }
    s->session.MarkDirty();
    s->session.Save();

    set_status(s, "Cancelling...");
}

static void on_cancel_clicked(GtkButton*, gpointer user_data) {
    cancel_in_flight(static_cast<UiState*>(user_data));
}

static std::string input_get_text(UiState* s) {
    auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->input));
    GtkTextIter a, e;
    gtk_text_buffer_get_bounds(buf, &a, &e);
    char* raw = gtk_text_buffer_get_text(buf, &a, &e, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

static void input_clear(UiState* s) {
    auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->input));
    gtk_text_buffer_set_text(buf, "", -1);
}

static void do_send_to_provider(UiState* s);

static void run_tool_calls_async(UiState* s, const std::vector<json>& toolCalls) {
    uint64_t gen = s->requestGen.load();
    std::thread([s, toolCalls, gen]() {
        std::vector<ChatMessage> results;
        results.reserve(toolCalls.size());
        for (const auto& tc : toolCalls) {
            if (s->requestGen.load() != gen) return;  // cancelled
            std::string name = tc.value("name", "");
            std::string args = tc.value("arguments", "{}");
            std::string id   = tc.value("id", "");
            std::string out  = toolexec::StripAnsi(toolexec::ExecuteTool(name, args));
            ChatMessage tr;
            tr.role = "tool";
            tr.content = out;
            tr.toolCallId = id;
            results.push_back(std::move(tr));
        }
        post_main([s, gen, results = std::move(results)]() mutable {
            if (!s->alive) return;
            if (s->requestGen.load() != gen) return;

            for (auto& r : results) {
                append_block(s, ("Tool Result [" + r.toolCallId + "]").c_str(),
                             r.content.c_str(), s->tagCode);
                s->session.History().push_back(std::move(r));
            }
            s->session.MarkDirty();
            s->session.Save();

            do_send_to_provider(s);
        });
    }).detach();
}

static void do_send_to_provider(UiState* s) {
    const char* provider = dropdown_selected(s->provider, s->providerList);
    const char* modelC   = dropdown_selected(s->model, s->modelList);
    std::string model = modelC ? modelC : "kimi-k2.5";

    if (provider) s->session.SetProvider(provider);
    s->session.SetModel(model);
    s->session.MarkDirty();
    s->session.Save();

    configure_http(s);
    auto protocol = protocol_for_model(model);
    std::string req = (protocol == net::CurlHttpClient::Protocol::Anthropic)
        ? build_anthropic_request(s, model)
        : build_openai_request(s, model);

    set_details(s, req.substr(0, std::min<size_t>(req.size(), 4000)));

    append_assistant_header(s);
    set_status(s, s->toolRound > 0 ? ("Streaming (tool round " + std::to_string(s->toolRound) + ")...")
                                   : std::string("Streaming..."));

    uint64_t gen = s->requestGen.load();

    s->http.SendStreaming(
        protocol,
        req,
        [s, gen](const std::string& chunk, bool thinking) {
            post_main([s, gen, chunk, thinking]() {
                if (!s->alive) return;
                if (s->requestGen.load() != gen) return;
                append_stream_chunk(s, chunk, thinking);
            });
        },
        [s, gen](bool ok, const std::string& content, const std::string& error,
                 const std::vector<json>& toolCalls, const std::string&, int, int) {
            post_main([s, gen, ok, content, error, toolCalls]() mutable {
                if (!s->alive) return;
                if (s->requestGen.load() != gen) return;
                append_stream_footer(s);

                if (!ok) {
                    if (error == "Cancelled") {
                        set_status(s, "Cancelled");
                        set_details(s, "Request cancelled by user.");
                    } else {
                        append_block(s, "Error", error.c_str(), s->tagError);
                        set_status(s, "Error");
                        set_details(s, error);
                    }
                    set_streaming(s, false);
                    reload_workspaces(s);
                    return;
                }

                // Record assistant turn, including tool_calls if any.
                ChatMessage a;
                a.role = "assistant";
                a.content = content;
                if (!toolCalls.empty()) a.toolCalls = toolCalls;
                s->session.History().push_back(a);
                if (!toolCalls.empty()) append_tool_calls(s, toolCalls);
                s->session.MarkDirty();
                s->session.Save();

                // If the turn produced tool calls and we haven't hit the
                // safety cap, execute them and loop back into the model.
                if (!toolCalls.empty() && s->toolRound < 40) {
                    ++s->toolRound;
                    run_tool_calls_async(s, toolCalls);
                    return;
                }

                set_status(s, "Done");
                set_details(s, "Response chars: " + std::to_string(content.size()));
                set_streaming(s, false);
                reload_workspaces(s);
            });
        }
    );
}

static void on_send_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    if (s->streaming) return;

    std::string text = input_get_text(s);
    size_t end = text.find_last_not_of(" \t\n\r");
    if (end == std::string::npos) {
        append_block(s, "Error", "Cannot send empty message.", s->tagError);
        return;
    }

    ChatMessage u;
    u.role = "user";
    u.content = text;
    s->session.History().push_back(u);
    append_from_message(s, u);
    input_clear(s);

    s->toolRound = 0;
    s->requestGen.fetch_add(1);
    set_streaming(s, true);
    do_send_to_provider(s);
}

static void on_new_workspace_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    s->http.Abort();
    s->session.NewSession();
    clear_transcript(s);
    append_block(s, "System", "New workspace session started.", s->tagAssistant);
    set_status(s, "New session");
    reload_workspaces(s);
}

// Input-scoped shortcuts. Window-level shortcuts handle Ctrl+K/O/N/L and
// Escape globally, but Ctrl+Enter lives here because it only makes sense
// while the input has focus (otherwise Enter means "activate focused
// control" on buttons/rows).
static gboolean on_input_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer data) {
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        on_send_clicked(nullptr, data);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget* build_dropdown(const char* const* items, GtkStringList** outList) {
    auto* list = gtk_string_list_new(items);
    auto* dd = gtk_drop_down_new(G_LIST_MODEL(list), nullptr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 0);
    *outList = list;
    return dd;
}

static GtkWidget* build_controls(UiState* s) {
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);

    const char* providers[] = {"zen", "opencode-go", nullptr};
    const char* models[] = {"kimi-k2.5", "gpt-4.1", "claude-3-7-sonnet-latest", nullptr};

    s->workspaceList = gtk_string_list_new(nullptr);
    s->workspace = gtk_drop_down_new(G_LIST_MODEL(s->workspaceList), nullptr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(s->workspace), FALSE);
    gtk_widget_set_tooltip_text(s->workspace, "Workspace / session");

    s->provider = build_dropdown(providers, &s->providerList);
    s->model = build_dropdown(models, &s->modelList);

    // Multi-line input: TextView in a scrolled window that grows up to
    // maxContentHeight then scrolls. Enter inserts a newline; Ctrl+Enter
    // sends. Placeholder is a dim overlay label that hides once the
    // buffer has any text.
    auto* inputScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inputScroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(inputScroll), 34);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(inputScroll), 200);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(inputScroll), TRUE);
    gtk_widget_set_hexpand(inputScroll, TRUE);

    s->input = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(s->input), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(s->input), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(s->input), 6);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(s->input), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(s->input), 6);

    auto* inputOverlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(inputOverlay), s->input);

    s->inputPlaceholder = gtk_label_new("Message... (Enter = newline, Ctrl+Enter = send)");
    gtk_widget_add_css_class(s->inputPlaceholder, "dim-label");
    gtk_widget_set_halign(s->inputPlaceholder, GTK_ALIGN_START);
    gtk_widget_set_valign(s->inputPlaceholder, GTK_ALIGN_START);
    gtk_widget_set_margin_start(s->inputPlaceholder, 10);
    gtk_widget_set_margin_top(s->inputPlaceholder, 8);
    gtk_widget_set_can_target(s->inputPlaceholder, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(inputOverlay), s->inputPlaceholder);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(inputScroll), inputOverlay);

    auto* inputBuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->input));
    auto placeholder_cb = +[](GtkTextBuffer* b, gpointer ud) {
        auto* st = static_cast<UiState*>(ud);
        GtkTextIter a, e;
        gtk_text_buffer_get_bounds(b, &a, &e);
        bool empty = gtk_text_iter_equal(&a, &e);
        if (st->inputPlaceholder) gtk_widget_set_visible(st->inputPlaceholder, empty);
    };
    g_signal_connect(inputBuf, "changed", G_CALLBACK(placeholder_cb), s);

    s->sendBtn = gtk_button_new_with_label("Send");
    s->cancelBtn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_visible(s->cancelBtn, FALSE);
    s->chooserBtn = gtk_button_new_with_label("Chooser");
    auto* newWsBtn = gtk_button_new_with_label("New");
    s->apiKeyBtn = gtk_button_new_with_label("API Key");

    auto* key = gtk_event_controller_key_new();
    gtk_widget_add_controller(s->input, key);

    g_signal_connect(key, "key-pressed", G_CALLBACK(on_input_key), s);
    g_signal_connect(s->sendBtn, "clicked", G_CALLBACK(on_send_clicked), s);
    g_signal_connect(s->cancelBtn, "clicked", G_CALLBACK(on_cancel_clicked), s);
    g_signal_connect(s->chooserBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        open_workspace_chooser(static_cast<UiState*>(user_data));
    }), s);
    g_signal_connect(newWsBtn, "clicked", G_CALLBACK(on_new_workspace_clicked), s);
    g_signal_connect(s->apiKeyBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        set_api_key(static_cast<UiState*>(user_data));
    }), s);
    g_signal_connect(s->workspace, "notify::selected", G_CALLBACK(on_workspace_changed), s);
    g_signal_connect(s->provider, "notify::selected", G_CALLBACK(on_provider_or_model_changed), s);
    g_signal_connect(s->model, "notify::selected", G_CALLBACK(on_provider_or_model_changed), s);

    gtk_box_append(GTK_BOX(row), s->workspace);
    gtk_box_append(GTK_BOX(row), s->chooserBtn);
    gtk_box_append(GTK_BOX(row), newWsBtn);
    gtk_box_append(GTK_BOX(row), s->provider);
    gtk_box_append(GTK_BOX(row), s->model);
    gtk_box_append(GTK_BOX(row), s->apiKeyBtn);
    gtk_box_append(GTK_BOX(row), s->cancelBtn);
    gtk_box_append(GTK_BOX(row), inputScroll);
    gtk_box_append(GTK_BOX(row), s->sendBtn);
    return row;
}

static void setup_tags(UiState* s) {
    s->tagUser = gtk_text_buffer_create_tag(
        s->buffer, "user",
        "foreground", "#D6E4FF",
        "family", "monospace",
        nullptr);

    s->tagAssistant = gtk_text_buffer_create_tag(
        s->buffer, "assistant",
        "foreground", "#E7E7E7",
        "family", "monospace",
        nullptr);

    s->tagMeta = gtk_text_buffer_create_tag(
        s->buffer, "meta",
        "foreground", "#9AA0A6",
        "weight", 700,
        "family", "sans",
        nullptr);

    s->tagError = gtk_text_buffer_create_tag(
        s->buffer, "error",
        "foreground", "#FFB3B3",
        "family", "monospace",
        nullptr);

    s->tagThinking = gtk_text_buffer_create_tag(
        s->buffer, "thinking",
        "foreground", "#7EC7FF",
        "style", PANGO_STYLE_ITALIC,
        "family", "monospace",
        nullptr);

    s->tagCode = gtk_text_buffer_create_tag(
        s->buffer, "code",
        "foreground", "#D8DEE9",
        "background", "#1A1E24",
        "family", "monospace",
        "pixels-above-lines", 2,
        "pixels-below-lines", 2,
        nullptr);

    s->tagInlineCode = gtk_text_buffer_create_tag(
        s->buffer, "inline-code",
        "foreground", "#EBCB8B",
        "background", "#1E222A",
        "family", "monospace",
        nullptr);

    s->tagHeading = gtk_text_buffer_create_tag(
        s->buffer, "heading",
        "foreground", "#ECEFF4",
        "weight", 700,
        "scale", 1.07,
        nullptr);

    s->tagBullet = gtk_text_buffer_create_tag(
        s->buffer, "bullet",
        "foreground", "#81A1C1",
        "weight", 700,
        nullptr);

    s->tagStrong = gtk_text_buffer_create_tag(
        s->buffer, "strong",
        "weight", 700,
        nullptr);

    s->tagEmph = gtk_text_buffer_create_tag(
        s->buffer, "emph",
        "style", PANGO_STYLE_ITALIC,
        nullptr);

    s->tagLink = gtk_text_buffer_create_tag(
        s->buffer, "link",
        "foreground", "#88C0D0",
        "underline", PANGO_UNDERLINE_SINGLE,
        nullptr);

    s->tagQuote = gtk_text_buffer_create_tag(
        s->buffer, "quote",
        "foreground", "#9AA0A6",
        "style", PANGO_STYLE_ITALIC,
        "left-margin", 24,
        "pixels-above-lines", 2,
        "pixels-below-lines", 2,
        nullptr);

    s->tagH1 = gtk_text_buffer_create_tag(
        s->buffer, "h1",
        "foreground", "#ECEFF4",
        "weight", 700,
        "scale", 1.45,
        "pixels-above-lines", 8,
        "pixels-below-lines", 4,
        nullptr);

    s->tagH2 = gtk_text_buffer_create_tag(
        s->buffer, "h2",
        "foreground", "#ECEFF4",
        "weight", 700,
        "scale", 1.28,
        "pixels-above-lines", 6,
        "pixels-below-lines", 3,
        nullptr);

    s->tagH3 = gtk_text_buffer_create_tag(
        s->buffer, "h3",
        "foreground", "#ECEFF4",
        "weight", 700,
        "scale", 1.14,
        "pixels-above-lines", 4,
        "pixels-below-lines", 2,
        nullptr);

    s->tagH4 = gtk_text_buffer_create_tag(
        s->buffer, "h4",
        "foreground", "#ECEFF4",
        "weight", 700,
        "scale", 1.05,
        nullptr);
}

static void ui_state_destroy(gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (!s) return;
    s->alive = false;
#ifdef GRIT_ENABLE_MCP
    s->mcp.Stop();
#endif
    s->http.Abort();
    delete s;
}

static void apply_compact_titlebar_css() {
    static bool applied = false;
    if (applied) return;
    applied = true;

    const char* css =
        "headerbar {"
        "  min-height: 30px;"
        "  padding-top: 0px;"
        "  padding-bottom: 0px;"
        "}"
        "headerbar button.titlebutton {"
        "  min-height: 22px;"
        "  min-width: 22px;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}";

    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    auto* display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

// Window-level shortcut callbacks. These fire regardless of focus — the
// per-input handler in on_input_key handles the same shortcuts when the
// entry has focus and swallows the event first.
static gboolean shortcut_clear(GtkWidget*, GVariant*, gpointer data) {
    clear_transcript(static_cast<UiState*>(data));
    return TRUE;
}

static gboolean shortcut_open_chooser(GtkWidget*, GVariant*, gpointer data) {
    open_workspace_chooser(static_cast<UiState*>(data));
    return TRUE;
}

static gboolean shortcut_new_session(GtkWidget*, GVariant*, gpointer data) {
    on_new_workspace_clicked(nullptr, data);
    return TRUE;
}

static gboolean shortcut_focus_input(GtkWidget*, GVariant*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (s->input) gtk_widget_grab_focus(s->input);
    return TRUE;
}

static gboolean shortcut_cancel(GtkWidget*, GVariant*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (s->streaming) {
        on_cancel_clicked(nullptr, data);
        return TRUE;
    }
    return FALSE;
}

#ifdef GRIT_ENABLE_MCP
// Helper: run `fn` on the GTK main loop and block until it finishes.
// Used for MCP callbacks that need to return a value synchronously —
// the read must happen on the UI thread where session_/UI state is mutated.
template <typename R>
static R run_on_main_sync(std::function<R()> fn) {
    struct Box {
        std::function<R()> fn;
        R result;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    auto* box = new Box{std::move(fn), R{}, {}, {}, false};
    g_idle_add_once([](gpointer d) {
        auto* b = static_cast<Box*>(d);
        b->result = b->fn();
        std::lock_guard<std::mutex> lk(b->m);
        b->done = true;
        b->cv.notify_all();
    }, box);
    std::unique_lock<std::mutex> lk(box->m);
    box->cv.wait(lk, [&]{ return box->done; });
    R out = std::move(box->result);
    lk.unlock();
    delete box;
    return out;
}

static void start_mcp(UiState* s) {
    MCPCallbacks cb;

    cb.sendMessage = [s](const std::string& msg) {
        post_main([s, msg]() {
            if (!s->alive) return;
            auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->input));
            gtk_text_buffer_set_text(buf, msg.c_str(), -1);
            on_send_clicked(nullptr, s);
        });
    };

    cb.getStatus = [s]() -> json {
        return run_on_main_sync<json>([s]() -> json {
            const char* provider = dropdown_selected(s->provider, s->providerList);
            const char* model    = dropdown_selected(s->model, s->modelList);
            return {
                {"connected", true},
                {"requestInProgress", s->streaming},
                {"provider", provider ? provider : ""},
                {"model", model ? model : ""},
                {"messageCount", (int)s->session.History().size()},
                {"sendButtonEnabled", !s->streaming}
            };
        });
    };

    cb.getConversation = [s]() -> json {
        return run_on_main_sync<json>([s]() -> json {
            json msgs = json::array();
            for (auto& m : s->session.History()) {
                json entry = {{"role", m.role}, {"content", m.content}};
                if (!m.toolCalls.empty()) {
                    json tcs = json::array();
                    for (auto& tc : m.toolCalls) {
                        tcs.push_back({{"name", tc.value("name", "")},
                                       {"arguments", tc.value("arguments", "")}});
                    }
                    entry["tool_calls"] = tcs;
                }
                if (!m.toolCallId.empty()) entry["tool_call_id"] = m.toolCallId;
                msgs.push_back(entry);
            }
            return msgs;
        });
    };

    cb.getLastAssistant = [s]() -> json {
        return run_on_main_sync<json>([s]() -> json {
            const auto& h = s->session.History();
            for (int i = (int)h.size() - 1; i >= 0; --i) {
                if (h[i].role == "assistant" && !h[i].content.empty()) {
                    return json{{"text", h[i].content}, {"index", i}};
                }
            }
            return json{{"text", ""}, {"index", -1}};
        });
    };

    cb.selectAllText = [s]() -> std::string {
        return run_on_main_sync<std::string>([s]() -> std::string {
            GtkTextIter a, e;
            gtk_text_buffer_get_bounds(s->buffer, &a, &e);
            gtk_text_buffer_select_range(s->buffer, &a, &e);
            char* raw = gtk_text_buffer_get_text(s->buffer, &a, &e, FALSE);
            std::string out = raw ? raw : "";
            g_free(raw);

            auto* clip = gtk_widget_get_clipboard(s->textView);
            if (clip) gdk_clipboard_set_text(clip, out.c_str());
            return out;
        });
    };

    cb.setWorkspace = [s](const std::string& cwd) {
        post_main([s, cwd]() {
            if (!s->alive) return;
            std::error_code ec;
            if (!std::filesystem::exists(cwd, ec) || !std::filesystem::is_directory(cwd, ec)) return;
            if (chdir(cwd.c_str()) != 0) return;
            load_session_for_cwd(s, cwd);
            reload_workspaces(s);
        });
    };

    cb.setProvider = [s](const std::string& provider, const std::string& model) {
        post_main([s, provider, model]() {
            if (!s->alive) return;
            int pi = list_index_of(s->providerList, provider);
            if (pi >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(s->provider), (guint)pi);
            if (!model.empty()) {
                int mi = list_index_of(s->modelList, model);
                if (mi >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(s->model), (guint)mi);
                s->session.SetModel(model);
            }
            if (!provider.empty()) s->session.SetProvider(provider);
            s->session.MarkDirty();
            s->session.Save();
            configure_http(s);
        });
    };

    cb.cancelRequest = [s]() {
        post_main([s]() {
            if (!s->alive) return;
            if (s->streaming) on_cancel_clicked(nullptr, s);
        });
    };

    s->mcp.Start(std::move(cb));
}
#else
static void start_mcp(UiState*) {}
#endif

static void install_window_shortcuts(UiState* s) {
    auto* controller = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(controller),
                                      GTK_SHORTCUT_SCOPE_GLOBAL);

    auto add = [&](const char* trigger, GtkShortcutFunc fn) {
        auto* sc = gtk_shortcut_new(
            gtk_shortcut_trigger_parse_string(trigger),
            gtk_callback_action_new(fn, s, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), sc);
    };

    add("<Control>k", shortcut_clear);
    add("<Control>o", shortcut_open_chooser);
    add("<Control>n", shortcut_new_session);
    add("<Control>l", shortcut_focus_input);
    add("Escape",     shortcut_cancel);

    gtk_widget_add_controller(s->window, controller);
}

static void activate(GtkApplication* app, gpointer user_data) {
    auto* win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 760);

    auto* hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    apply_compact_titlebar_css();

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), root);

    auto* sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    auto* tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 12);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);

    auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

    auto* state = new UiState();
    state->window = win;
    state->buffer = buf;
    state->textView = tv;

    setup_tags(state);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);

    state->scrolledWindow = sw;
    state->vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    g_signal_connect(state->vadj, "value-changed", G_CALLBACK(on_vadj_value_changed), state);
    g_signal_connect(state->vadj, "changed", G_CALLBACK(on_vadj_changed), state);

    auto* controls = build_controls(state);

    state->statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(state->statusRow, 10);
    gtk_widget_set_margin_end(state->statusRow, 10);
    gtk_widget_set_margin_bottom(state->statusRow, 4);

    state->spinner = gtk_spinner_new();
    gtk_widget_set_visible(state->spinner, FALSE);

    state->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->status), 0.0f);
    gtk_widget_set_hexpand(state->status, TRUE);

    gtk_box_append(GTK_BOX(state->statusRow), state->spinner);
    gtk_box_append(GTK_BOX(state->statusRow), state->status);

    state->statusExpander = gtk_expander_new("Details");
    gtk_widget_set_margin_start(state->statusExpander, 10);
    gtk_widget_set_margin_end(state->statusExpander, 10);
    gtk_widget_set_margin_bottom(state->statusExpander, 8);

    state->details = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->details), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(state->details), TRUE);
    gtk_expander_set_child(GTK_EXPANDER(state->statusExpander), state->details);

    gtk_box_append(GTK_BOX(root), sw);
    gtk_box_append(GTK_BOX(root), controls);
    gtk_box_append(GTK_BOX(root), state->statusRow);
    gtk_box_append(GTK_BOX(root), state->statusExpander);

    // Initial workspace/session load
    std::string cwd = current_cwd();
    load_session_for_cwd(state, cwd);
    if (state->session.History().empty()) {
        append_block(state, "System",
                     "Gritcode GTK4 WIP\n"
                     "- Native GTK4 text rendering and selection\n"
                     "- Native controls instead of hand-drawn GL widgets\n"
                     "- Incremental migration toward parity",
                     state->tagAssistant);
    }
    reload_workspaces(state);
    configure_http(state);
    fetch_models(state);

    install_window_shortcuts(state);

    start_mcp(state);

    g_object_set_data_full(G_OBJECT(win), "ui-state", state, ui_state_destroy);
    gtk_window_present(GTK_WINDOW(win));

    bool* openChooser = static_cast<bool*>(user_data);
    if (openChooser && *openChooser) {
        g_idle_add_once(
            +[](gpointer d) { open_workspace_chooser(static_cast<UiState*>(d)); },
            state);
    }
}

int main(int argc, char** argv) {
    // Strip --session-chooser before handing argv to GApplication, which
    // would otherwise reject the unknown option.
    bool openChooser = false;
    int outN = 0;
    std::vector<char*> outArgv;
    outArgv.reserve((size_t)argc + 1);
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--session-chooser") == 0) {
            openChooser = true;
            continue;
        }
        outArgv.push_back(argv[i]);
        ++outN;
    }
    outArgv.push_back(nullptr);

    auto* app = gtk_application_new("ai.gritcode.gtk4wip", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &openChooser);
    int rc = g_application_run(G_APPLICATION(app), outN, outArgv.data());
    g_object_unref(app);
    return rc;
}
