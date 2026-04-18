// GTK4 WIP frontend for Gritcode
// Incremental migration path from custom GL UI to native GTK rendering.

#include <gtk/gtk.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "session.h"

namespace fs = std::filesystem;

struct UiState {
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* textView = nullptr;
    GtkWidget* input = nullptr;
    GtkWidget* provider = nullptr;  // GtkDropDown
    GtkWidget* model = nullptr;     // GtkDropDown
    GtkWidget* workspace = nullptr; // GtkDropDown
    GtkWidget* status = nullptr;

    GtkStringList* providerList = nullptr;
    GtkStringList* modelList = nullptr;
    GtkStringList* workspaceList = nullptr;

    GtkTextTag* tagUser = nullptr;
    GtkTextTag* tagAssistant = nullptr;
    GtkTextTag* tagMeta = nullptr;
    GtkTextTag* tagError = nullptr;

    SessionManager session;
    std::vector<std::string> workspaceCwds;
    bool mutatingWorkspace = false;
};

static std::string current_cwd() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return ".";
}

static void set_status(UiState* s, const std::string& t) {
    gtk_label_set_text(GTK_LABEL(s->status), t.c_str());
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

static void scroll_to_end(UiState* s) {
    if (!s || !s->buffer || !s->textView) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(s->buffer, &end);
    GtkTextMark* mark = gtk_text_buffer_create_mark(s->buffer, nullptr, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(s->textView), mark, 0.0, TRUE, 0.0, 1.0);
    gtk_text_buffer_delete_mark(s->buffer, mark);
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

static void clear_transcript(UiState* s) {
    gtk_text_buffer_set_text(s->buffer, "", -1);
}

static void append_from_message(UiState* s, const ChatMessage& m) {
    if (m.role == "user") append_block(s, "You", m.content.c_str(), s->tagUser);
    else if (m.role == "assistant") append_block(s, "Assistant", m.content.c_str(), s->tagAssistant);
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

    if (s->workspaceList) g_object_unref(s->workspaceList);
    s->workspaceList = gtk_string_list_new(nullptr);

    std::vector<SessionInfo> all = SessionManager::ListSessions();
    std::sort(all.begin(), all.end(), [](const SessionInfo& a, const SessionInfo& b) {
        return a.lastUsed > b.lastUsed;
    });

    std::string cwd = current_cwd();
    gtk_string_list_append(s->workspaceList, cwd.c_str());
    s->workspaceCwds.push_back(cwd);

    for (const auto& si : all) {
        if (si.cwd == cwd) continue;
        gtk_string_list_append(s->workspaceList, si.cwd.c_str());
        s->workspaceCwds.push_back(si.cwd);
    }

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
}

static void on_workspace_changed(GtkDropDown*, GParamSpec*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    if (s->mutatingWorkspace) return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(s->workspace));
    if (idx >= s->workspaceCwds.size()) return;
    std::string cwd = s->workspaceCwds[idx];

    std::error_code ec;
    if (fs::exists(cwd, ec) && fs::is_directory(cwd, ec)) {
        chdir(cwd.c_str());
        load_session_for_cwd(s, cwd);
    } else {
        append_block(s, "Error", "Selected workspace no longer exists on disk.", s->tagError);
    }
}

static void on_provider_or_model_changed(GtkDropDown*, GParamSpec*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    session_save_from_ui(s);
}

static void on_send_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
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
    const char* model = dropdown_selected(s->model, s->modelList);
    if (provider) s->session.SetProvider(provider);
    if (model) s->session.SetModel(model);

    ChatMessage a;
    a.role = "assistant";
    a.content = "GTK4 WIP response path active.\n"
                "This branch is migrating rendering/input to GTK4 while preserving behavior parity.\n"
                "(Backend network path wiring is next milestone.)";
    s->session.History().push_back(a);
    append_from_message(s, a);

    s->session.MarkDirty();
    s->session.Save();

    gtk_editable_set_text(GTK_EDITABLE(s->input), "");
    set_status(s, "Saved");
    reload_workspaces(s);
}

static void on_input_activate(GtkEditable*, gpointer user_data) {
    on_send_clicked(nullptr, user_data);
}

static gboolean on_input_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer data) {
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_Return) {
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

    const char* providers[] = {"openai", "anthropic", "local", nullptr};
    const char* models[] = {"gpt-4.1", "o4-mini", "claude-sonnet", nullptr};

    s->workspaceList = gtk_string_list_new(nullptr);
    s->workspace = gtk_drop_down_new(G_LIST_MODEL(s->workspaceList), nullptr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(s->workspace), FALSE);
    gtk_widget_set_tooltip_text(s->workspace, "Workspace / session");

    s->provider = build_dropdown(providers, &s->providerList);
    s->model = build_dropdown(models, &s->modelList);

    s->input = gtk_entry_new();
    gtk_widget_set_hexpand(s->input, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->input), "Message...");

    auto* send = gtk_button_new_with_label("Send");

    auto* key = gtk_event_controller_key_new();
    gtk_widget_add_controller(s->input, key);

    g_signal_connect(key, "key-pressed", G_CALLBACK(on_input_key), s);
    g_signal_connect(send, "clicked", G_CALLBACK(on_send_clicked), s);
    g_signal_connect(s->input, "activate", G_CALLBACK(on_input_activate), s);
    g_signal_connect(s->workspace, "notify::selected", G_CALLBACK(on_workspace_changed), s);
    g_signal_connect(s->provider, "notify::selected", G_CALLBACK(on_provider_or_model_changed), s);
    g_signal_connect(s->model, "notify::selected", G_CALLBACK(on_provider_or_model_changed), s);

    gtk_box_append(GTK_BOX(row), s->workspace);
    gtk_box_append(GTK_BOX(row), s->provider);
    gtk_box_append(GTK_BOX(row), s->model);
    gtk_box_append(GTK_BOX(row), s->input);
    gtk_box_append(GTK_BOX(row), send);
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
}

static void activate(GtkApplication* app, gpointer) {
    auto* win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 760);
    gtk_window_set_title(GTK_WINDOW(win), "Gritcode (GTK4 WIP)");

    auto* hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);

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

    auto* state = g_new0(UiState, 1);
    state->buffer = buf;
    state->textView = tv;

    setup_tags(state);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    auto* controls = build_controls(state);

    state->status = gtk_label_new("");
    gtk_widget_set_margin_start(state->status, 10);
    gtk_widget_set_margin_end(state->status, 10);
    gtk_widget_set_margin_bottom(state->status, 8);
    gtk_label_set_xalign(GTK_LABEL(state->status), 0.0f);

    gtk_box_append(GTK_BOX(root), sw);
    gtk_box_append(GTK_BOX(root), controls);
    gtk_box_append(GTK_BOX(root), state->status);

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

    g_object_set_data_full(G_OBJECT(win), "ui-state", state, (GDestroyNotify)g_free);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char** argv) {
    auto* app = gtk_application_new("ai.gritcode.gtk4wip", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
