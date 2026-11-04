// Native GTK 3 / Cairo frontend. No browser or terminal emulator is embedded.
#include "dashboard.h"
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <signal.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <map>

namespace {
Config config;
MonitorHub hub;
GtkApplication* application = nullptr;
GtkWidget *widget = nullptr, *drawing = nullptr, *settings = nullptr;
GtkStatusIcon* tray = nullptr;
guint timer = 0;
int viewport = 400, scroll = 0;
double zoom = 1;
bool smoke = false, settingsOnLaunch = false;
std::string settingsScreenshot;
/** One Pango layout is reused for a complete frame instead of allocating one per label. */
class CairoCanvas final : public Canvas {
  public:
    explicit CairoCanvas(cairo_t* context) : cr(context) {
        layout = pango_cairo_create_layout(cr);
        for (int i = 0; i < 4; ++i) {
            fonts[i] = pango_font_description_new();
            pango_font_description_set_family(fonts[i], "monospace");
            pango_font_description_set_absolute_size(fonts[i], (i == 0   ? 10
                                                                : i == 1 ? 12
                                                                : i == 2 ? 13
                                                                         : 16) *
                                                                   PANGO_SCALE);
            if (i >= 2)
                pango_font_description_set_weight(fonts[i], PANGO_WEIGHT_SEMIBOLD);
        }
    }
    ~CairoCanvas() override {
        for (auto* font : fonts)
            pango_font_description_free(font);
        g_object_unref(layout);
    }
    void fill(int x, int y, int w, int h, Color color) override {
        set(color);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    }
    void line(int x, int y, int xx, int yy, Color color) override {
        set(color);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x + 0.5, y + 0.5);
        cairo_line_to(cr, xx + 0.5, yy + 0.5);
        cairo_stroke(cr);
    }
    void text(int x, int y, int w, int h, const std::wstring& value, int font, Color color,
              bool right = false) override {
        auto utf8 = wideToUtf8(value);
        pango_layout_set_font_description(layout, fonts[std::clamp(font, 0, 3)]);
        pango_layout_set_text(layout, utf8.c_str(), static_cast<int>(utf8.size()));
        pango_layout_set_width(layout, w * PANGO_SCALE);
        pango_layout_set_height(layout, h * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(layout, TRUE);
        pango_layout_set_alignment(layout, right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
        int th = 0;
        pango_layout_get_pixel_size(layout, nullptr, &th);
        set(color);
        cairo_move_to(cr, x, y + std::max(0, (h - th) / 2));
        pango_cairo_show_layout(cr, layout);
    }

  private:
    void set(Color c) {
        cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
    }
    cairo_t* cr;
    PangoLayout* layout;
    PangoFontDescription* fonts[4]{};
};
void openSettings();
void resizeWidget();
/** Restore the existing GTK application window rather than launching a duplicate monitor. */
void showWidget() {
    if (widget) {
        gtk_widget_show(widget);
        gtk_window_present(GTK_WINDOW(widget));
    }
}
/** Drain incoming frames at the chosen timer cadence, even while the widget is hidden. */
gboolean update(gpointer) {
    hub.tick(config);
    if (widget && gtk_widget_get_visible(widget))
        gtk_widget_queue_draw(drawing);
    return G_SOURCE_CONTINUE;
}
/** Monitor work-area and user zoom bound the viewport; wheel scrolling reaches all visible cards. */
void resizeWidget() {
    if (!widget)
        return;
    zoom = config.sizePercent / 100.0;
    auto state = hub.snapshot(config);
    auto layout = dashboardLayout(state, config);
    auto display = gtk_widget_get_display(widget);
    auto monitor = gdk_display_get_primary_monitor(display);
    GdkRectangle work{0, 0, 1920, 1080};
    if (monitor)
        gdk_monitor_get_workarea(monitor, &work);
    viewport = std::min(layout.height, std::max(200, static_cast<int>((work.height - 32) / zoom)));
    scroll = std::clamp(scroll, 0, std::max(0, layout.height - viewport));
    gtk_window_resize(GTK_WINDOW(widget), static_cast<int>(WIDGET_WIDTH * zoom),
                      static_cast<int>(viewport * zoom));
    gtk_widget_set_opacity(widget, config.opacity / 100.0);
    gtk_window_set_keep_above(GTK_WINDOW(widget), config.pinned);
    gtk_widget_queue_draw(drawing);
}
/** Persist only validated drafts and preserve unaffected transports through the shared hub. */
bool apply(const Config& draft) {
    if (!smoke && !draft.save()) {
        auto dialog = gtk_message_dialog_new(GTK_WINDOW(settings), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                             GTK_BUTTONS_CLOSE,
                                             "Could not save settings. The previous session was kept.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return false;
    }
    config = draft;
    hub.configure(config);
    if (timer)
        g_source_remove(timer);
    timer = g_timeout_add(static_cast<guint>(std::min(config.intervalMs, 1000)), update, nullptr);
    scroll = 0;
    resizeWidget();
    return true;
}
enum Field {
    ServerList = 1,
    Name,
    Host,
    User,
    Port,
    Key,
    Enabled,
    Visible,
    Collapsed,
    Detail,
    Size,
    Interval,
    Opacity,
    Local,
    Pin,
    Graphs,
    Cpu,
    Ram,
    Disk,
    Network,
    Sensors,
    Swap,
    LogEnabled,
    Duration,
    Forever,
    Every,
    Directory,
    MaxMiB,
    Keep,
    CpuThreshold,
    RamThreshold
};
/** A settings draft owns native controls and is discarded on Close unless Apply succeeds. */
struct Editor {
    Config draft;
    int selected = 0;
    bool loading = false;
    std::map<int, GtkWidget*> controls;
    GtkWidget* notebook = nullptr;
    GtkWidget* entry(int id, const std::wstring& value) {
        auto h = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(h), wideToUtf8(value).c_str());
        gtk_entry_set_max_length(GTK_ENTRY(h), 2048);
        controls[id] = h;
        return h;
    }
    GtkWidget* spin(int id, int value, int minimum, int maximum) {
        auto h = gtk_spin_button_new_with_range(minimum, maximum, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(h), value);
        controls[id] = h;
        return h;
    }
    GtkWidget* check(int id, const char* title, bool value) {
        auto h = gtk_check_button_new_with_label(title);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(h), value);
        controls[id] = h;
        return h;
    }
    GtkWidget* combo(int id, const std::vector<const char*>& labels, int selectedValue) {
        auto h = gtk_combo_box_text_new();
        for (auto title : labels)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(h), title);
        gtk_combo_box_set_active(GTK_COMBO_BOX(h), selectedValue);
        controls[id] = h;
        return h;
    }
    std::wstring text(int id) {
        return utf8ToWide(gtk_entry_get_text(GTK_ENTRY(controls[id])));
    }
    bool checked(int id) {
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(controls[id]));
    }
    int number(int id) {
        return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(controls[id]));
    }
    int choice(int id) {
        return gtk_combo_box_get_active(GTK_COMBO_BOX(controls[id]));
    }
    void error(const char* text) {
        auto d = gtk_message_dialog_new(GTK_WINDOW(settings), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                        GTK_BUTTONS_CLOSE, "%s", text);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
    bool saveProfile() {
        if (loading || draft.servers.empty())
            return true;
        auto p = draft.servers[selected];
        p.name = text(Name);
        p.host = text(Host);
        p.user = text(User);
        p.port = number(Port);
        p.key = text(Key);
        if (p.name.empty() || p.name.size() > 80 || !validConnection(p)) {
            error("Enter a display name and a valid SSH destination.");
            return false;
        }
        p.enabled = checked(Enabled);
        p.visible = checked(Visible);
        p.collapsed = checked(Collapsed);
        draft.servers[selected] = p;
        return true;
    }
    void loadProfile() {
        loading = true;
        const auto& p = draft.servers[selected];
        for (const auto& field : std::vector<std::pair<int, std::wstring>>{
                 {Name, p.name}, {Host, p.host}, {User, p.user}, {Key, p.key}})
            gtk_entry_set_text(GTK_ENTRY(controls[field.first]), wideToUtf8(field.second).c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(controls[Port]), p.port);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls[Enabled]), p.enabled);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls[Visible]), p.visible);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls[Collapsed]), p.collapsed);
        loading = false;
    }
    void list() {
        loading = true;
        auto children = gtk_container_get_children(GTK_CONTAINER(controls[ServerList]));
        for (auto* item = children; item; item = item->next)
            gtk_widget_destroy(GTK_WIDGET(item->data));
        g_list_free(children);
        for (const auto& p : draft.servers) {
            auto label = gtk_label_new(wideToUtf8(p.name + (p.visible ? L"" : L" [hidden]")).c_str());
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_widget_set_margin_start(label, 10);
            gtk_widget_set_margin_end(label, 10);
            gtk_widget_set_margin_top(label, 12);
            gtk_widget_set_margin_bottom(label, 12);
            gtk_list_box_insert(GTK_LIST_BOX(controls[ServerList]), label, -1);
        }
        gtk_widget_show_all(controls[ServerList]);
        gtk_list_box_select_row(GTK_LIST_BOX(controls[ServerList]),
                                gtk_list_box_get_row_at_index(GTK_LIST_BOX(controls[ServerList]), selected));
        loading = false;
    }
    bool saveAll() {
        if (!saveProfile()) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
            return false;
        }
        draft.detail = choice(Detail);
        int sizes[] = {80, 100, 125}, rates[] = {100, 250, 500, 1000, 2000, 5000, 10000},
            summaries[] = {100, 250, 500, 1000, 5000, 10000, 60000};
        draft.sizePercent = sizes[std::clamp(choice(Size), 0, 2)];
        draft.intervalMs = rates[std::clamp(choice(Interval), 0, 6)];
        draft.opacity = number(Opacity);
        draft.local = checked(Local);
        draft.pinned = checked(Pin);
        draft.graphs = checked(Graphs);
        draft.showCpu = checked(Cpu);
        draft.showRam = checked(Ram);
        draft.showDisk = checked(Disk);
        draft.showNetwork = checked(Network);
        draft.showSensors = checked(Sensors);
        draft.showSwap = checked(Swap);
        draft.logEnabled = checked(LogEnabled);
        draft.logDurationMinutes = checked(Forever) ? 0 : number(Duration);
        draft.logEveryMs = summaries[std::clamp(choice(Every), 0, 6)];
        draft.logDirectory = text(Directory);
        draft.logMaxMiB = number(MaxMiB);
        draft.logKeep = number(Keep);
        draft.spikeCpu = number(CpuThreshold);
        draft.spikeRam = number(RamThreshold);
        if (draft.logDirectory.empty() || draft.logDirectory.find_first_of(L"\r\n") != std::wstring::npos) {
            error("Choose a valid recording folder.");
            return false;
        }
        draft.selected = selected;
        draft.syncSelected();
        if (!apply(draft))
            return false;
        list();
        return true;
    }
};

/** Provide consistent spacing and padding for the native settings pages. */
GtkWidget* pageGrid() {
    auto grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 24);
    return grid;
}
/** Attach one accessible GTK label/control pair to a shared grid. */
void row(GtkWidget* grid, int index, const char* title, GtkWidget* control) {
    auto label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, index, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), control, 1, index, 1, 1);
}
/** GTK notebook pages expose the same monitoring/appearance/recording model as Windows. */
void openSettings() {
    if (settings) {
        gtk_window_present(GTK_WINDOW(settings));
        return;
    }
    settings = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(settings), "Node-Sight · Settings");
    gtk_window_set_transient_for(GTK_WINDOW(settings), GTK_WINDOW(widget));
    gtk_window_set_default_size(GTK_WINDOW(settings), 860, 670);
    auto* editor = new Editor;
    editor->draft = config;
    editor->selected = config.selected;
    g_object_set_data_full(G_OBJECT(settings), "editor", editor,
                           [](gpointer p) { delete static_cast<Editor*>(p); });
    g_signal_connect(settings, "destroy", G_CALLBACK((+[](GtkWidget*, gpointer) { settings = nullptr; })),
                     nullptr);
    auto outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 14);
    gtk_container_add(GTK_CONTAINER(settings), outer);
    auto heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(heading),
                         "<span foreground='#97c579' weight='bold'>node-sight / configuration</span>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_set_margin_start(heading, 12);
    gtk_box_pack_start(GTK_BOX(outer), heading, FALSE, FALSE, 6);
    editor->notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(outer), editor->notebook, TRUE, TRUE, 0);
    auto servers = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(servers), 16);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), servers, gtk_label_new("1 servers"));
    auto sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(sidebar, 205, -1);
    gtk_box_pack_start(GTK_BOX(servers), sidebar, FALSE, FALSE, 0);
    auto scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_box_pack_start(GTK_BOX(sidebar), scroller, TRUE, TRUE, 0);
    editor->controls[ServerList] = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroller), editor->controls[ServerList]);
    auto actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto add = gtk_button_new_with_label("+ Add server"), remove = gtk_button_new_with_label("Remove");
    gtk_box_pack_start(GTK_BOX(actions), add, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), remove, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), actions, FALSE, FALSE, 0);
    auto fields = pageGrid();
    gtk_box_pack_start(GTK_BOX(servers), fields, TRUE, TRUE, 0);
    row(fields, 0, "Display name", editor->entry(Name, L""));
    row(fields, 1, "Host / address", editor->entry(Host, L""));
    row(fields, 2, "SSH user", editor->entry(User, L""));
    row(fields, 3, "Port", editor->spin(Port, 22, 1, 65535));
    row(fields, 4, "Private key", editor->entry(Key, L""));
    gtk_grid_attach(GTK_GRID(fields), editor->check(Enabled, "Monitor this server", true), 0, 5, 2, 1);
    gtk_grid_attach(GTK_GRID(fields),
                    editor->check(Visible, "Show in widget (hidden nodes can keep recording)", true), 0, 6, 2,
                    1);
    gtk_grid_attach(GTK_GRID(fields), editor->check(Collapsed, "Start collapsed", false), 0, 7, 2, 1);
    auto auth = gtk_label_new("Linux authentication uses your SSH keys or ssh-agent.\nUse an empty key path "
                              "for normal OpenSSH defaults.");
    gtk_label_set_xalign(GTK_LABEL(auth), 0);
    gtk_grid_attach(GTK_GRID(fields), auth, 0, 8, 2, 1);
    editor->list();
    editor->loadProfile();
    g_signal_connect(editor->controls[ServerList], "row-selected",
                     G_CALLBACK((+[](GtkListBox* box, GtkListBoxRow* selected, gpointer p) {
                         auto* e = static_cast<Editor*>(p);
                         if (e->loading || !selected)
                             return;
                         int index = gtk_list_box_row_get_index(selected);
                         if (e->saveProfile()) {
                             e->selected = index;
                             e->loadProfile();
                         } else {
                             e->loading = true;
                             gtk_list_box_select_row(box, gtk_list_box_get_row_at_index(box, e->selected));
                             e->loading = false;
                         }
                     })),
                     editor);
    g_signal_connect(add, "clicked", G_CALLBACK((+[](GtkButton*, gpointer p) {
                         auto* e = static_cast<Editor*>(p);
                         if (e->draft.servers.size() >= 32 || !e->saveProfile())
                             return;
                         ServerProfile profile;
                         profile.id = newProfileId();
                         profile.name = L"Server " + std::to_wstring(e->draft.servers.size() + 1);
                         e->draft.servers.push_back(profile);
                         e->selected = static_cast<int>(e->draft.servers.size()) - 1;
                         e->list();
                         e->loadProfile();
                     })),
                     editor);
    g_signal_connect(remove, "clicked", G_CALLBACK((+[](GtkButton*, gpointer p) {
                         auto* e = static_cast<Editor*>(p);
                         if (e->draft.servers.size() <= 1) {
                             e->error("Keep one profile and disable its monitoring if it is not needed.");
                             return;
                         }
                         e->draft.servers.erase(e->draft.servers.begin() + e->selected);
                         e->selected = std::max(0, e->selected - 1);
                         e->list();
                         e->loadProfile();
                     })),
                     editor);
    auto appearance = pageGrid();
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), appearance, gtk_label_new("2 appearance"));
    row(appearance, 0, "Information density",
        editor->combo(Detail,
                      {"Minimal · one line per node", "Balanced · rates and graphs",
                       "Detailed · capacities and sensors"},
                      config.detail));
    row(appearance, 1, "Widget size",
        editor->combo(Size, {"80% · mini", "100% · compact", "125% · enlarged"},
                      config.sizePercent < 90    ? 0
                      : config.sizePercent > 110 ? 2
                                                 : 1));
    int rates[] = {100, 250, 500, 1000, 2000, 5000, 10000}, selected = 4;
    for (int i = 0; i < 7; ++i)
        if (config.intervalMs == rates[i])
            selected = i;
    row(appearance, 2, "Collection interval",
        editor->combo(Interval,
                      {"100 ms · 10 samples/s", "250 ms · 4 samples/s", "500 ms · 2 samples/s", "1 second",
                       "2 seconds", "5 seconds", "10 seconds"},
                      selected));
    row(appearance, 3, "Opacity (%)", editor->spin(Opacity, config.opacity, 45, 100));
    gtk_grid_attach(GTK_GRID(appearance), editor->check(Local, "Monitor this computer", config.local), 0, 4,
                    1, 1);
    gtk_grid_attach(GTK_GRID(appearance), editor->check(Pin, "Keep above other apps", config.pinned), 1, 4, 1,
                    1);
    gtk_grid_attach(GTK_GRID(appearance), editor->check(Graphs, "Draw history graphs", config.graphs), 0, 5,
                    2, 1);
    const int ids[] = {Cpu, Ram, Disk, Network, Sensors, Swap};
    const char* names[] = {"CPU", "Memory", "Disk capacity", "Network rates", "Sensors / uptime", "Swap"};
    bool flags[] = {config.showCpu,     config.showRam,     config.showDisk,
                    config.showNetwork, config.showSensors, config.showSwap};
    for (int i = 0; i < 6; ++i)
        gtk_grid_attach(GTK_GRID(appearance), editor->check(ids[i], names[i], flags[i]), i % 2, 6 + i / 2, 1,
                        1);
    auto cost = gtk_label_new("100 ms increases client CPU, SSH traffic and remote collector work.");
    gtk_grid_attach(GTK_GRID(appearance), cost, 0, 10, 2, 1);
    auto recording = pageGrid();
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), recording, gtk_label_new("3 recording"));
    gtk_grid_attach(
        GTK_GRID(recording),
        editor->check(LogEnabled, "Record automatically while the app is running", config.logEnabled), 0, 0,
        2, 1);
    row(recording, 1, "Duration (minutes)",
        editor->spin(Duration, std::max(1, config.logDurationMinutes), 1, 525600));
    gtk_grid_attach(GTK_GRID(recording),
                    editor->check(Forever, "Continuous / no deadline", config.logDurationMinutes == 0), 0, 2,
                    2, 1);
    int summaries[] = {100, 250, 500, 1000, 5000, 10000, 60000};
    selected = 3;
    for (int i = 0; i < 7; ++i)
        if (config.logEveryMs == summaries[i])
            selected = i;
    row(recording, 3, "Write summaries every",
        editor->combo(Every,
                      {"100 ms", "250 ms", "500 ms", "1 second", "5 seconds", "10 seconds", "60 seconds"},
                      selected));
    row(recording, 4, "Local recording folder", editor->entry(Directory, config.logDirectory));
    row(recording, 5, "Rotate at (MiB)", editor->spin(MaxMiB, config.logMaxMiB, 1, 1024));
    row(recording, 6, "Archives to keep", editor->spin(Keep, config.logKeep, 1, 20));
    row(recording, 7, "CPU spike threshold (%)", editor->spin(CpuThreshold, config.spikeCpu, 1, 100));
    row(recording, 8, "RAM spike threshold (%)", editor->spin(RamThreshold, config.spikeRam, 1, 100));
    auto logInfo =
        gtk_label_new("Received subsecond samples contribute to peaks even with slower summaries.\nFiles "
                      "rotate locally; stopped monitoring or a closed app cannot collect new data.");
    gtk_label_set_xalign(GTK_LABEL(logInfo), 0);
    gtk_grid_attach(GTK_GRID(recording), logInfo, 0, 9, 2, 1);
    auto about = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_container_set_border_width(GTK_CONTAINER(about), 28);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), about, gtk_label_new("4 about / usage"));
    for (const char* text :
         {"Node-Sight 2.0 development · MIT license", "Linux: GTK 3 / Cairo · Windows: Win32 / GDI",
          "One persistent system OpenSSH process per monitored server.",
          "App RSS, normalized CPU and session peak RAM are shown in the footer.",
          "Each SSH process uses additional memory. Logs stay on this device.",
          "Wayland compositors may restrict pinning, positioning or tray support."}) {
        auto label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_pack_start(GTK_BOX(about), label, FALSE, FALSE, 0);
    }
    auto bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    auto applyButton = gtk_button_new_with_label("[ Apply changes ]"), close = gtk_button_new_with_label("[ Close ]");
    gtk_style_context_add_class(gtk_widget_get_style_context(applyButton), "suggested-action");
    gtk_box_pack_end(GTK_BOX(bottom), close, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), applyButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bottom, FALSE, FALSE, 0);
    g_signal_connect(applyButton, "clicked",
                     G_CALLBACK((+[](GtkButton*, gpointer p) { static_cast<Editor*>(p)->saveAll(); })),
                     editor);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), settings);
    gtk_widget_show_all(settings);
}
/** Native tray/context menu actions remain available when the undecorated window is hidden. */
void menu(GdkEvent* event) {
    auto m = gtk_menu_new();
    auto add = [&](const char* text, GCallback callback) {
        auto item = gtk_menu_item_new_with_label(text);
        g_signal_connect(item, "activate", callback, nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(m), item);
    };
    add("Settings...", G_CALLBACK((+[](GtkMenuItem*, gpointer) { openSettings(); })));
    add("Show widget", G_CALLBACK((+[](GtkMenuItem*, gpointer) { showWidget(); })));
    add("Toggle always on top", G_CALLBACK((+[](GtkMenuItem*, gpointer) {
            config.pinned = !config.pinned;
            config.save();
            resizeWidget();
        })));
    add("Start / restart recording",
        G_CALLBACK((+[](GtkMenuItem*, gpointer) { hub.restartRecording(config); })));
    add("Stop recording", G_CALLBACK((+[](GtkMenuItem*, gpointer) { hub.stopRecording(); })));
    add("Reconnect servers", G_CALLBACK((+[](GtkMenuItem*, gpointer) {
            hub.stop();
            hub.configure(config);
        })));
    add("Quit Node-Sight",
        G_CALLBACK((+[](GtkMenuItem*, gpointer) { g_application_quit(G_APPLICATION(application)); })));
    g_signal_connect(
        m, "selection-done",
        G_CALLBACK((+[](GtkMenuShell* shell, gpointer) { gtk_widget_destroy(GTK_WIDGET(shell)); })), nullptr);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), event);
}
/** Create the undecorated utility window once; later activations restore the existing instance. */
void activate(GtkApplication* app, gpointer) {
    if (widget) {
        showWidget();
        return;
    }
    widget = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(widget), "Node-Sight");
    gtk_window_set_decorated(GTK_WINDOW(widget), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(widget), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(widget), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(widget), GDK_WINDOW_TYPE_HINT_UTILITY);
    auto visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(widget));
    if (visual)
        gtk_widget_set_visual(widget, visual);
    drawing = gtk_drawing_area_new();
    gtk_widget_add_events(drawing, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_container_add(GTK_CONTAINER(widget), drawing);
    g_signal_connect(drawing, "draw", G_CALLBACK((+[](GtkWidget*, cairo_t* cr, gpointer) -> gboolean {
                         cairo_save(cr);
                         cairo_scale(cr, zoom, zoom);
                         CairoCanvas canvas(cr);
                         auto state = hub.snapshot(config);
                         drawDashboard(canvas, state, config, scroll, false, viewport);
                         drawFooter(canvas, state, viewport);
                         cairo_restore(cr);
                         return FALSE;
                     })),
                     nullptr);
    g_signal_connect(drawing, "button-press-event",
                     G_CALLBACK((+[](GtkWidget*, GdkEventButton* event, gpointer) -> gboolean {
                         if (event->button == 3) {
                             menu(reinterpret_cast<GdkEvent*>(event));
                             return TRUE;
                         }
                         int x = static_cast<int>(event->x / zoom), y = static_cast<int>(event->y / zoom);
                         if (y < 36 && event->button == 1) {
                             if (x >= HEADER_CLOSE)
                                 g_application_quit(G_APPLICATION(application));
                             else if (x >= HEADER_HIDE)
                                 gtk_widget_hide(widget);
                             else if (x >= HEADER_SETTINGS)
                                 openSettings();
                             else if (x >= HEADER_PIN) {
                                 config.pinned = !config.pinned;
                                 config.save();
                                 resizeWidget();
                             } else
                                 gtk_window_begin_move_drag(GTK_WINDOW(widget), 1,
                                                            static_cast<int>(event->x_root),
                                                            static_cast<int>(event->y_root), event->time);
                             return TRUE;
                         }
                         if (y >= HEADER_HEIGHT && y < viewport - FOOTER_HEIGHT && x > WIDGET_WIDTH - 48) {
                             auto layout = dashboardLayout(hub.snapshot(config), config);
                             for (const auto& hit : layout.hosts)
                                 if (y + scroll >= hit.top && y + scroll < hit.top + 28) {
                                     if (hit.id == L"local") {
                                         config.localCollapsed = !config.localCollapsed;
                                         config.save();
                                         resizeWidget();
                                         break;
                                     }
                                     for (auto& profile : config.servers)
                                         if (profile.id == hit.id) {
                                             profile.collapsed = !profile.collapsed;
                                             config.save();
                                             resizeWidget();
                                         }
                                     break;
                                 }
                         }
                         return TRUE;
                     })),
                     nullptr);
    g_signal_connect(
        drawing, "scroll-event", G_CALLBACK((+[](GtkWidget*, GdkEventScroll* event, gpointer) -> gboolean {
            double dy = 0;
            if (!gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), nullptr, &dy))
                dy = event->direction == GDK_SCROLL_UP ? -1 : 1;
            auto layout = dashboardLayout(hub.snapshot(config), config);
            scroll = std::clamp(scroll + static_cast<int>(dy * 48), 0, std::max(0, layout.height - viewport));
            gtk_widget_queue_draw(drawing);
            return TRUE;
        })),
        nullptr);
    g_signal_connect(widget, "delete-event", G_CALLBACK((+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
                         g_application_quit(G_APPLICATION(application));
                         return TRUE;
                     })),
                     nullptr);
    hub.configure(config);
    resizeWidget();
    timer = g_timeout_add(static_cast<guint>(std::min(config.intervalMs, 1000)), update, nullptr);
    // GtkStatusIcon is supported by X11 trays and some Wayland tray extensions; it is optional.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    tray = gtk_status_icon_new_from_icon_name("utilities-system-monitor");
    gtk_status_icon_set_tooltip_text(tray, "Node-Sight · click to restore");
    g_signal_connect(tray, "activate", G_CALLBACK((+[](GtkStatusIcon*, gpointer) { showWidget(); })),
                     nullptr);
    g_signal_connect(tray, "popup-menu",
                     G_CALLBACK((+[](GtkStatusIcon*, guint, guint, gpointer) { menu(nullptr); })), nullptr);
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_widget_show_all(widget);
    if (config.x != -32000 && config.y != -32000)
        gtk_window_move(GTK_WINDOW(widget), config.x, config.y);
    if (settingsOnLaunch)
        openSettings();
    if (smoke)
        g_timeout_add(
            1600,
            +[](gpointer) -> gboolean {
                auto state = hub.snapshot(config);
                if (settingsOnLaunch && settings && !settingsScreenshot.empty()) {
                    int width = gtk_widget_get_allocated_width(settings),
                        height = gtk_widget_get_allocated_height(settings);
                    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                    auto cr = cairo_create(surface);
                    gtk_widget_draw(settings, cr);
                    cairo_surface_write_to_png(surface, settingsScreenshot.c_str());
                    cairo_destroy(cr);
                    cairo_surface_destroy(surface);
                }
                if (settingsOnLaunch && settings) {
                    auto* editor = static_cast<Editor*>(g_object_get_data(G_OBJECT(settings), "editor"));
                    editor->saveAll();
                }
                bool good = gtk_widget_get_visible(widget) &&
                            (!settingsOnLaunch || (settings && gtk_widget_get_visible(settings))) &&
                            !state.hosts.empty() && state.hosts.back().metrics.ratesValid;
                std::cout << "{\"gtk_visible\":" << (gtk_widget_get_visible(widget) ? "true" : "false")
                          << ",\"local_rates_valid\":" << (good ? "true" : "false") << "}\n";
                g_object_set_data(G_OBJECT(application), "smoke-failed", GINT_TO_POINTER(!good));
                g_application_quit(G_APPLICATION(application));
                return G_SOURCE_REMOVE;
            },
            nullptr);
}
/** Render anonymous synthetic data through Cairo without capturing the desktop. */
int exportPreview(const std::string& path, bool serverOnly) {
    if (!gtk_init_check(nullptr, nullptr)) {
        std::cerr << "A display (or Xvfb) is required for native font rendering.\n";
        return 1;
    }
    Config c;
    c.detail = 1;
    c.local = !serverOnly;
    auto state = demoDashboard(c);
    state.hosts[1].profile.collapsed = true;
    int h = dashboardLayout(state, c).height;
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WIDGET_WIDTH, h);
    auto cr = cairo_create(surface);
    {
        CairoCanvas canvas(cr);
        drawDashboard(canvas, state, c, 0, true);
        drawFooter(canvas, state, h);
    }
    auto result = cairo_surface_write_to_png(surface, path.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return result == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
} // namespace

/** Handle headless diagnostics before requiring a graphical GTK session. */
int main(int argc, char** argv) {
    // Protect the parent from a pipe closed during SSH cancellation; SSH handles its own I/O errors.
    signal(SIGPIPE, SIG_IGN);
    std::string mode = argc > 1 ? argv[1] : "";
    std::wstring configPath;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config")
            configPath = utf8ToWide(argv[i + 1]);
    config.load(configPath);
    if ((mode == "--demo-preview" || mode == "--demo-server-preview") && argc > 2)
        return exportPreview(argv[2], mode == "--demo-server-preview");
    if (mode == "--probe") {
        hub.configure(config);
        auto deadline = monotonicSeconds() + 18;
        bool ready = false;
        DashboardState state;
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hub.tick(config);
            state = hub.snapshot(config, true);
            ready = !state.hosts.empty();
            for (const auto& view : state.hosts)
                if (!view.metrics.ratesValid)
                    ready = false;
            if (ready)
                break;
        } while (monotonicSeconds() < deadline);
        std::cout << "{\"enabled_servers\":" << state.activeServers
                  << ",\"visible_nodes\":" << state.hosts.size()
                  << ",\"rates_valid\":" << (ready ? "true" : "false") << "}\n";
        hub.stop();
        return ready ? 0 : 1;
    }
    smoke = mode == "--smoke-test" || mode == "--settings-smoke-test";
    settingsOnLaunch = mode == "--settings" || mode == "--settings-smoke-test";
    if (smoke && mode == "--settings-smoke-test" && argc > 2 && std::string(argv[2]).rfind("--", 0) != 0)
        settingsScreenshot = argv[2];
    if (smoke) {
        ServerProfile fixture;
        fixture.enabled = false;
        config.servers = {fixture};
        config.local = true;
        config.intervalMs = 100;
        config.logEnabled = false;
        config.logDirectory = L"./logs";
    }
    if (!gtk_init_check(nullptr, nullptr)) {
        std::cerr << "Node-Sight requires a graphical GTK desktop.\n";
        return 1;
    }
    // Request this process's dark native variant; explicit notebook colors also work on light desktops.
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    auto css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, R"CSS(
        * {font-family:monospace;font-size:13px;border-radius:0;box-shadow:none;}
        window, notebook, notebook > stack {background-color:#000000;color:#dee6da;}
        notebook {border:1px solid #656d60;}
        notebook > header {background-color:#000000;border-color:#656d60;}
        notebook > header > tabs > tab {background-color:#000000;border:1px solid #2b2f2a;padding:8px 14px;}
        notebook > header > tabs > tab:checked {background-color:#000000;border-color:#97c579;}
        notebook > header > tabs > tab:checked label {color:#97c579;}
        label, checkbutton label, notebook label {color:#dee6da;}
        entry, spinbutton, combobox, list, scrolledwindow {background-color:#000000;color:#dee6da;}
        entry {border:1px solid #656d60;padding:6px;caret-color:#97c579;}
        entry:focus {border-color:#97c579;}
        button {background-image:none;background-color:#000000;color:#9ba492;border:1px solid #656d60;padding:6px 10px;}
        button:hover, button:focus {background-color:#000000;color:#97c579;border-color:#97c579;}
        button.suggested-action {background-color:#000000;color:#97c579;border-color:#97c579;}
        checkbutton check {background-image:none;background-color:#000000;color:#97c579;border:1px solid #656d60;}
        checkbutton check:checked {background-color:#000000;color:#97c579;border-color:#97c579;}
        row {background-color:#000000;color:#dee6da;}
        row:selected {background-color:#0d150a;color:#97c579;}
        menu {background-color:#000000;color:#dee6da;}
        scrollbar {background-color:#000000;}
        scrollbar slider {background-color:#656d60;min-width:5px;min-height:5px;}
    )CSS",
                                    -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    application = gtk_application_new("io.github.liveira.NodeSight",
                                      smoke ? G_APPLICATION_NON_UNIQUE : static_cast<GApplicationFlags>(0));
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    g_unix_signal_add(
        SIGTERM,
        +[](gpointer) -> gboolean {
            g_application_quit(G_APPLICATION(application));
            return G_SOURCE_REMOVE;
        },
        nullptr);
    g_unix_signal_add(
        SIGINT,
        +[](gpointer) -> gboolean {
            g_application_quit(G_APPLICATION(application));
            return G_SOURCE_REMOVE;
        },
        nullptr);
    char* runArgs[] = {argv[0], nullptr};
    int result = g_application_run(G_APPLICATION(application), 1, runArgs);
    bool failed = g_object_get_data(G_OBJECT(application), "smoke-failed") != nullptr;
    if (widget && !smoke) {
        gtk_window_get_position(GTK_WINDOW(widget), &config.x, &config.y);
        config.save();
    }
    hub.stop();
    if (timer)
        g_source_remove(timer);
    if (tray)
        g_object_unref(tray);
    g_object_unref(application);
    return failed ? 1 : result;
}
