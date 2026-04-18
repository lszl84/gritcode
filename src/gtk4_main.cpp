// GTK4 WIP frontend for Gritcode
// Incremental migration path from custom GL UI to native GTK rendering.

#include <gtk/gtk.h>
#include <string>
#include <vector>

struct UiState {
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* textView = nullptr;
    GtkWidget* input = nullptr;
    GtkWidget* provider = nullptr; // GtkDropDown
    GtkWidget* model = nullptr;    // GtkDropDown
    GtkStringList* providerList = nullptr;
    GtkStringList* modelList = nullptr;
    GtkTextTag* tagUser = nullptr;
    GtkTextTag* tagAssistant = nullptr;
    GtkTextTag* tagMeta = nullptr;
    GtkTextTag* tagError = nullptr;
};

static const char* dropdown_selected(GtkWidget* dd, GtkStringList* list) {
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (!list) return nullptr;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
    if (idx >= n) return nullptr;
    return gtk_string_list_get_string(list, idx);
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

    // Header
    std::string hdr = role;
    hdr += "\n";
    gtk_text_buffer_insert_with_tags(s->buffer, &end, hdr.c_str(), -1, s->tagMeta, nullptr);

    // Body
    gtk_text_buffer_insert_with_tags(s->buffer, &end, text, -1, bodyTag, nullptr);
    gtk_text_buffer_insert(s->buffer, &end, "\n\n", -1);

    scroll_to_end(s);
}

static void append_user(UiState* s, const char* text) {
    append_block(s, "You", text, s->tagUser);
}

static void append_assistant(UiState* s, const std::string& text) {
    append_block(s, "Assistant", text.c_str(), s->tagAssistant);
}

static void append_error(UiState* s, const char* text) {
    append_block(s, "Error", text, s->tagError);
}

static void on_send_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    const char* text = gtk_editable_get_text(GTK_EDITABLE(s->input));
    if (!text || !*text) {
        append_error(s, "Cannot send empty message.");
        return;
    }

    append_user(s, text);

    const char* provider = dropdown_selected(s->provider, s->providerList);
    const char* model = dropdown_selected(s->model, s->modelList);

    std::string reply = "GTK4 WIP response path active.\n";
    reply += "provider: ";
    reply += (provider ? provider : "unknown");
    reply += "\nmodel: ";
    reply += (model ? model : "unknown");
    reply += "\n\n";
    reply += "This branch is migrating rendering/input to GTK4 while preserving behavior parity.";

    append_assistant(s, reply);
    gtk_editable_set_text(GTK_EDITABLE(s->input), "");
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

    append_block(state, "System",
                 "Gritcode GTK4 WIP\n"
                 "- Native GTK4 text rendering and selection\n"
                 "- Native controls instead of hand-drawn GL widgets\n"
                 "- Incremental migration toward parity",
                 state->tagAssistant);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    auto* controls = build_controls(state);

    gtk_box_append(GTK_BOX(root), sw);
    gtk_box_append(GTK_BOX(root), controls);

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
