// GTK4 WIP frontend for Gritcode
// Experimental replacement for custom GL renderer path.

#include <gtk/gtk.h>
#include <string>

struct UiState {
    GtkTextBuffer* buffer = nullptr;
    GtkWidget* input = nullptr;
    GtkWidget* provider = nullptr;
    GtkWidget* model = nullptr;
};

static void append_line(GtkTextBuffer* buffer, const std::string& line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, line.c_str(), -1);
    gtk_text_buffer_insert(buffer, &end, "\n", 1);
}

static void on_send_clicked(GtkButton*, gpointer user_data) {
    auto* s = static_cast<UiState*>(user_data);
    const char* text = gtk_editable_get_text(GTK_EDITABLE(s->input));
    if (!text || !*text) return;

    append_line(s->buffer, std::string("You: ") + text);

    const char* provider = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(s->provider));
    const char* model = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(s->model));
    std::string reply = "Assistant (";
    reply += provider ? provider : "provider";
    reply += "/";
    reply += model ? model : "model";
    reply += "): GTK4 WIP response path active.";
    append_line(s->buffer, reply);

    gtk_editable_set_text(GTK_EDITABLE(s->input), "");
}

static void on_input_activate(GtkEditable*, gpointer user_data) {
    on_send_clicked(nullptr, user_data);
}

static GtkWidget* build_controls(UiState* s) {
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);

    s->provider = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->provider), "openai");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->provider), "anthropic");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->provider), "local");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->provider), 0);

    s->model = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->model), "gpt-4.1");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->model), "o4-mini");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->model), "claude-sonnet");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->model), 0);

    s->input = gtk_entry_new();
    gtk_widget_set_hexpand(s->input, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->input), "Message...");

    auto* send = gtk_button_new_with_label("Send");

    g_signal_connect(send, "clicked", G_CALLBACK(on_send_clicked), s);
    g_signal_connect(s->input, "activate", G_CALLBACK(on_input_activate), s);

    gtk_box_append(GTK_BOX(row), s->provider);
    gtk_box_append(GTK_BOX(row), s->model);
    gtk_box_append(GTK_BOX(row), s->input);
    gtk_box_append(GTK_BOX(row), send);
    return row;
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
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);

    auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    append_line(buf, "Gritcode GTK4 WIP");
    append_line(buf, "- Native GTK4 text rendering and selection");
    append_line(buf, "- Native controls (no custom GL widgets)");
    append_line(buf, "- This branch is the migration path toward full parity");

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);

    auto* state = g_new0(UiState, 1);
    state->buffer = buf;

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
