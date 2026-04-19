// GTK4 WIP frontend for Gritcode
// Incremental migration path from custom GL UI to native GTK rendering.

#include <gtk/gtk.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>

#include "session.h"
#include "curl_http.h"
#include "keychain.h"
#include <nlohmann/json.hpp>

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
    GtkTextTag* tagHeading = nullptr;
    GtkTextTag* tagBullet = nullptr;

    SessionManager session;
    net::CurlHttpClient http;
    std::vector<std::string> workspaceCwds;
    bool mutatingWorkspace = false;
    bool streaming = false;
    bool alive = true;

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

static void append_stream_chunk(UiState* s, const std::string& text, bool thinking) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert_with_tags(s->buffer, &end, text.c_str(), -1,
                                     thinking ? s->tagThinking : s->tagAssistant,
                                     nullptr);
    scroll_to_end(s);
}

static void append_stream_footer(UiState* s) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert(s->buffer, &end, "\n\n", -1);
    scroll_to_end(s);
}

static void clear_transcript(UiState* s) {
    gtk_text_buffer_set_text(s->buffer, "", -1);
}

static void insert_inline_markdown(UiState* s, GtkTextIter* it, const std::string& line, GtkTextTag* normalTag) {
    size_t i = 0;
    while (i < line.size()) {
        size_t tick = line.find('`', i);
        if (tick == std::string::npos) {
            std::string tail = line.substr(i);
            if (!tail.empty()) gtk_text_buffer_insert_with_tags(s->buffer, it, tail.c_str(), -1, normalTag, nullptr);
            break;
        }
        if (tick > i) {
            std::string plain = line.substr(i, tick - i);
            gtk_text_buffer_insert_with_tags(s->buffer, it, plain.c_str(), -1, normalTag, nullptr);
        }
        size_t end = line.find('`', tick + 1);
        if (end == std::string::npos) {
            std::string rest = line.substr(tick);
            gtk_text_buffer_insert_with_tags(s->buffer, it, rest.c_str(), -1, normalTag, nullptr);
            break;
        }
        std::string code = line.substr(tick + 1, end - tick - 1);
        gtk_text_buffer_insert_with_tags(s->buffer, it, code.c_str(), -1, s->tagInlineCode, nullptr);
        i = end + 1;
    }
}

static void append_assistant_markdown(UiState* s, const std::string& text) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    gtk_text_buffer_insert_with_tags(s->buffer, &end, "Assistant\n", -1, s->tagMeta, nullptr);

    bool inCode = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);

        if (line.rfind("```", 0) == 0) {
            inCode = !inCode;
            gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
        } else if (inCode) {
            gtk_text_buffer_insert_with_tags(s->buffer, &end, line.c_str(), -1, s->tagCode, nullptr);
            gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
        } else if (line.rfind("### ", 0) == 0 || line.rfind("## ", 0) == 0 || line.rfind("# ", 0) == 0) {
            size_t off = (line[1] == '#') ? ((line[2] == '#') ? 4 : 3) : 2;
            std::string heading = line.substr(off);
            gtk_text_buffer_insert_with_tags(s->buffer, &end, heading.c_str(), -1, s->tagHeading, nullptr);
            gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
        } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            gtk_text_buffer_insert_with_tags(s->buffer, &end, "• ", -1, s->tagBullet, nullptr);
            insert_inline_markdown(s, &end, line.substr(2), s->tagAssistant);
            gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
        } else {
            insert_inline_markdown(s, &end, line, s->tagAssistant);
            gtk_text_buffer_insert(s->buffer, &end, "\n", 1);
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
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
    s->http.FetchModels([s](std::vector<net::ModelInfo> models, int status) {
        post_main([s, models = std::move(models), status]() mutable {
            if (!s->alive) return;
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

static std::string build_openai_request(UiState* s, const std::string& model) {
    json j;
    j["model"] = model;
    j["stream"] = true;
    j["max_tokens"] = 32000;

    json msgs = json::array();
    std::string sys =
        "You are an AI coding assistant. Keep responses concise and practical. "
        "When code changes are required, provide exact steps and patches.";
    msgs.push_back({{"role", "system"}, {"content", sys}});

    for (const auto& m : s->session.History()) {
        if (m.role == "user" || m.role == "assistant") {
            msgs.push_back({{"role", m.role}, {"content", m.content}});
        }
    }
    j["messages"] = std::move(msgs);
    return j.dump();
}

static std::string build_anthropic_request(UiState* s, const std::string& model) {
    json j;
    j["model"] = model;
    j["stream"] = true;
    j["max_tokens"] = 32000;
    j["system"] =
        "You are an AI coding assistant. Keep responses concise and practical. "
        "When code changes are required, provide exact steps and patches.";

    json msgs = json::array();
    for (const auto& m : s->session.History()) {
        if (m.role == "user" || m.role == "assistant") {
            msgs.push_back({{"role", m.role}, {"content", m.content}});
        }
    }
    j["messages"] = std::move(msgs);
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
}

static void on_cancel_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    if (!s->streaming) return;
    s->http.Abort();
    set_status(s, "Cancelling...");
}

static void on_send_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    if (s->streaming) return;

    const char* text = gtk_editable_get_text(GTK_EDITABLE(s->input));
    if (!text || !*text) {
        append_block(s, "Error", "Cannot send empty message.", s->tagError);
        return;
    }

    ChatMessage u;
    u.role = "user";
    u.content = text;
    s->session.History().push_back(u);
    append_from_message(s, u);

    const char* provider = dropdown_selected(s->provider, s->providerList);
    const char* modelC = dropdown_selected(s->model, s->modelList);
    std::string model = modelC ? modelC : "kimi-k2.5";

    if (provider) s->session.SetProvider(provider);
    s->session.SetModel(model);
    s->session.MarkDirty();
    s->session.Save();

    gtk_editable_set_text(GTK_EDITABLE(s->input), "");

    configure_http(s);
    auto protocol = protocol_for_model(model);
    std::string req = (protocol == net::CurlHttpClient::Protocol::Anthropic)
        ? build_anthropic_request(s, model)
        : build_openai_request(s, model);

    set_details(s, req.substr(0, std::min<size_t>(req.size(), 4000)));

    set_streaming(s, true);
    append_assistant_header(s);
    set_status(s, "Streaming...");

    s->http.SendStreaming(
        protocol,
        req,
        [s](const std::string& chunk, bool thinking) {
            post_main([s, chunk, thinking]() {
                if (!s->alive) return;
                append_stream_chunk(s, chunk, thinking);
            });
        },
        [s](bool ok, const std::string& content, const std::string& error,
            const std::vector<json>& toolCalls, const std::string&, int, int) {
            post_main([s, ok, content, error, toolCalls]() mutable {
                if (!s->alive) return;
                append_stream_footer(s);

                if (ok) {
                    ChatMessage a;
                    a.role = "assistant";
                    a.content = content;
                    if (!toolCalls.empty()) a.toolCalls = toolCalls;
                    s->session.History().push_back(a);
                    if (!toolCalls.empty()) append_tool_calls(s, toolCalls);
                    s->session.MarkDirty();
                    s->session.Save();
                    set_status(s, "Done");
                    set_details(s, "Response chars: " + std::to_string(content.size()));
                } else {
                    if (error == "Cancelled") {
                        set_status(s, "Cancelled");
                        set_details(s, "Request cancelled by user.");
                    } else {
                        append_block(s, "Error", error.c_str(), s->tagError);
                        set_status(s, "Error");
                        set_details(s, error);
                    }
                }

                set_streaming(s, false);
                reload_workspaces(s);
            });
        }
    );
}

static void on_input_activate(GtkEditable*, gpointer user_data) {
    on_send_clicked(nullptr, user_data);
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

static gboolean on_input_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_Return) {
        on_send_clicked(nullptr, data);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_k) {
        clear_transcript(s);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_o) {
        open_workspace_chooser(s);
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

    s->input = gtk_entry_new();
    gtk_widget_set_hexpand(s->input, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->input), "Message...");

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
    g_signal_connect(s->input, "activate", G_CALLBACK(on_input_activate), s);
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
    gtk_box_append(GTK_BOX(row), s->input);
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
}

static void ui_state_destroy(gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (!s) return;
    s->alive = false;
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

static void activate(GtkApplication* app, gpointer) {
    auto* win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 760);
    gtk_window_set_title(GTK_WINDOW(win), "Gritcode (GTK4 WIP)");

    auto* hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), gtk_label_new("Gritcode (GTK4 WIP)"));
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
    state->buffer = buf;
    state->textView = tv;

    setup_tags(state);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);

    state->scrolledWindow = sw;
    state->vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    g_signal_connect(state->vadj, "value-changed", G_CALLBACK(on_vadj_value_changed), state);
    g_signal_connect(state->vadj, "changed", G_CALLBACK(on_vadj_changed), state);

    auto* controls = build_controls(state);

    state->status = gtk_label_new("");
    gtk_widget_set_margin_start(state->status, 10);
    gtk_widget_set_margin_end(state->status, 10);
    gtk_widget_set_margin_bottom(state->status, 4);
    gtk_label_set_xalign(GTK_LABEL(state->status), 0.0f);

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
    gtk_box_append(GTK_BOX(root), state->status);
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

    g_object_set_data_full(G_OBJECT(win), "ui-state", state, ui_state_destroy);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char** argv) {
    auto* app = gtk_application_new("ai.gritcode.gtk4wip", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
