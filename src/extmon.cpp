// ExtMon — external monitor brightness/tone tuner for GNOME Wayland.
// Backend: org.gnome.Mutter.DisplayConfig SetCrtcGamma (1D CRTC gamma LUT):
// gain/contrast/lift/gamma/shadows/highlights/temperature/RGB balance.
// NOTE: SetOutputCTM is accepted by Mutter but has no visible effect on
// current GNOME (verified on GNOME 50), so no cross-channel color controls.
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>

#define BUS_NAME   "org.gnome.Mutter.DisplayConfig"
#define BUS_PATH   "/org/gnome/Mutter/DisplayConfig"
#define IFACE_NAME "org.gnome.Mutter.DisplayConfig"
#define RAMP_N 1024
#define APP_ID   "io.github.Aihuman.ExtMon"

// ---------------------------------------------------------------- Tuning ----
struct Tuning {
    double gain = 1.0;        // brightness multiplier
    double contrast = 1.0;
    double lift = 0.0;        // black lift
    double gamma = 1.0;
    double shadows = 0.0;     // -1..1
    double highlights = 0.0;  // -1..1
    double temp = 0.0;        // -1 cool .. +1 warm
    double tint_r = 1.0, tint_g = 1.0, tint_b = 1.0;  // channel balance 0.5..1.5
};

struct SliderSpec {
    const char *label;
    double lo, hi, step;
    double Tuning::*field;
    int decimals;
};

static const SliderSpec SPECS[] = {
    {"Brightness", 0.5, 1.6,  0.01,  &Tuning::gain,      2},
    {"Contrast",   0.7, 1.6,  0.01,  &Tuning::contrast,  2},
    {"Black lift", 0.0, 0.25, 0.005, &Tuning::lift,      3},
    {"Gamma",      0.5, 2.0,  0.01,  &Tuning::gamma,     2},
    {"Shadows",   -1.0, 1.0,  0.02,  &Tuning::shadows,   2},
    {"Highlights",-1.0, 1.0,  0.02,  &Tuning::highlights,2},
    {"Temperature",-1.0, 1.0, 0.02,  &Tuning::temp,      2},
    {"Red",        0.5, 1.5,  0.01,  &Tuning::tint_r,    2},
    {"Green",      0.5, 1.5,  0.01,  &Tuning::tint_g,    2},
    {"Blue",       0.5, 1.5,  0.01,  &Tuning::tint_b,    2},
};
static const int NSPEC = sizeof(SPECS)/sizeof(SPECS[0]);

struct Preset { std::string name; Tuning t; bool builtin = false; size_t idx = 0; };
struct OutputInfo { guint id = 0; int crtc = -1; std::string conn; std::string name; };

static struct App {
    GtkApplication *app = nullptr;
    GtkWidget *window = nullptr, *output_drop = nullptr, *status = nullptr, *preset_flow = nullptr;
    GtkWidget *scale[NSPEC], *val[NSPEC];
    GDBusProxy *proxy = nullptr;
    std::vector<OutputInfo> outputs;
    std::vector<Preset> presets;
    Tuning cur;
    guint apply_pending = 0;
    bool suppress = false;
} A;

// ------------------------------------------------------------------ paths ----
static char *state_path()  { return g_build_filename(g_get_user_config_dir(), "extmon.state.json", nullptr); }
static char *preset_path() { return g_build_filename(g_get_user_config_dir(), "extmon-presets.txt", nullptr); }

static void tune_from_state(Tuning &t, const char *buf) {
    auto getnum = [&](const char *key, double *out) {
        const char *k = strstr(buf, key);
        if (!k) return;
        const char *c = strchr(k + strlen(key), ':');
        if (c) sscanf(c + 1, "%lf", out);
    };
    getnum("\"gain\"", &t.gain);       getnum("\"contrast\"", &t.contrast);
    getnum("\"lift\"", &t.lift);       getnum("\"gamma\"", &t.gamma);
    getnum("\"shadows\"", &t.shadows); getnum("\"highlights\"", &t.highlights);
    getnum("\"temp\"", &t.temp);
    getnum("\"tint_r\"", &t.tint_r);   getnum("\"tint_g\"", &t.tint_g);
    getnum("\"tint_b\"", &t.tint_b);
}

static Tuning load_state(std::string *monitor = nullptr) {
    Tuning t;
    char *p = state_path();
    char *buf = nullptr;
    if (g_file_get_contents(p, &buf, nullptr, nullptr)) {
        tune_from_state(t, buf);
        if (monitor) {
            const char *k = strstr(buf, "\"monitor\"");
            if (k && (k = strchr(k, ':')) && (k = strchr(k, '"'))) {
                for (++k; *k && *k != '"'; ++k) *monitor += *k;
            }
        }
        g_free(buf);
    }
    g_free(p);
    return t;
}

static void save_state(const Tuning &t, const std::string &monitor) {
    char *p = state_path();
    char buf[512];
    snprintf(buf, sizeof buf,
        "{\"gain\": %.3f, \"contrast\": %.3f, \"lift\": %.3f, \"gamma\": %.3f, "
        "\"shadows\": %.3f, \"highlights\": %.3f, "
        "\"temp\": %.3f, \"tint_r\": %.3f, \"tint_g\": %.3f, \"tint_b\": %.3f, "
        "\"monitor\": \"%s\"}\n",
        t.gain, t.contrast, t.lift, t.gamma, t.shadows, t.highlights,
        t.temp, t.tint_r, t.tint_g, t.tint_b, monitor.c_str());
    g_file_set_contents(p, buf, -1, nullptr);
    g_free(p);
}

// ---------------------------------------------------------------- presets ----
static void builtin_presets(std::vector<Preset> &v) {
    auto mk = [&](const char *n, Tuning t){ Preset p; p.name = n; p.t = t; p.builtin = true; v.push_back(p); };
    Tuning t;
    mk("Default", t);
    t = Tuning(); t.gain = 1.30; t.contrast = 1.05; mk("Bright", t);
    t = Tuning(); t.gain = 1.50; t.contrast = 1.10; t.lift = 0.02; mk("Bright+", t);
    t = Tuning(); t.gain = 1.10; t.contrast = 1.25; t.temp = 0.15; mk("Movie", t);
    t = Tuning(); t.contrast = 1.30; t.gamma = 0.95; t.gain = 1.05; t.temp = 0.10; mk("Vivid", t);
    t = Tuning(); t.gain = 0.95; t.temp = 0.55; mk("Night", t);
    t = Tuning(); t.gain = 0.92; t.contrast = 0.90; t.lift = 0.05; mk("Soft", t);
    t = Tuning(); t.temp = -0.40; mk("Cool", t);
    t = Tuning(); t.temp = 0.40; mk("Warm", t);
}

static void load_presets() {
    A.presets.clear();
    builtin_presets(A.presets);
    char *p = preset_path();
    char *buf = nullptr;
    if (g_file_get_contents(p, &buf, nullptr, nullptr)) {
        char *save = nullptr;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
            Preset pr;
            char name[64]; double v[10];
            if (sscanf(line, "%63[^|]|%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                       name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                       &v[8], &v[9]) == 11) {
                pr.name = name;
                pr.t = Tuning();
                double *f = &pr.t.gain;
                for (int i = 0; i < 10; i++) f[i] = v[i];
                A.presets.push_back(pr);
            }
        }
        g_free(buf);
    }
    g_free(p);
    for (size_t i = 0; i < A.presets.size(); i++) A.presets[i].idx = i;
}

static void write_user_presets() {
    char *p = preset_path();
    GString *s = g_string_new(nullptr);
    for (auto &pr : A.presets) if (!pr.builtin) {
        const double *f = &pr.t.gain;
        g_string_append_printf(s, "%s|%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            pr.name.c_str(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
    }
    g_file_set_contents(p, s->str, -1, nullptr);
    g_string_free(s, TRUE);
    g_free(p);
}

// ------------------------------------------------------------------ proxy ----
static bool ensure_proxy() {
    if (A.proxy) return true;
    A.proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE,
        nullptr, BUS_NAME, BUS_PATH, IFACE_NAME, nullptr, nullptr);
    return A.proxy != nullptr;
}

static void refresh_outputs() {
    A.outputs.clear();
    if (!ensure_proxy()) return;
    GVariant *res = g_dbus_proxy_call_sync(A.proxy, "GetResources", g_variant_new("()"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
    if (!res) return;
    GVariant *outs = g_variant_get_child_value(res, 2);
    GVariantIter it; g_variant_iter_init(&it, outs);
    GVariant *item;
    while ((item = g_variant_iter_next_value(&it))) {
        OutputInfo oi;
        GVariant *v0 = g_variant_get_child_value(item, 0);
        GVariant *v2 = g_variant_get_child_value(item, 2);
        GVariant *v4 = g_variant_get_child_value(item, 4);
        oi.id = g_variant_get_uint32(v0);
        oi.crtc = g_variant_get_int32(v2);
        oi.conn = g_variant_get_string(v4, nullptr);
        g_variant_unref(v0); g_variant_unref(v2); g_variant_unref(v4);
        GVariant *props = g_variant_get_child_value(item, g_variant_n_children(item) - 1);
        char *dn = nullptr;
        g_variant_lookup(props, "display-name", "s", &dn);
        if (dn) { oi.name = dn; g_free(dn); }
        if (oi.name.empty() || oi.name == oi.conn) oi.name = oi.conn;
        if (oi.crtc >= 0) A.outputs.push_back(oi);
        g_variant_unref(props);
        g_variant_unref(item);
    }
    g_variant_unref(outs);
    g_variant_unref(res);
}

static const OutputInfo *selected_output() {
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(A.output_drop));
    if (sel == GTK_INVALID_LIST_POSITION || sel >= A.outputs.size()) return nullptr;
    return &A.outputs[sel];
}

// ------------------------------------------------------------ color maths ----
static inline double clamp01(double v){ return v < 0 ? 0 : (v > 1 ? 1 : v); }

// common per-channel tone curve (channel-independent part)
static double tone(double x, const Tuning &t) {
    x = clamp01(x);
    x = std::pow(x, 1.0 / std::max(t.gamma, 1e-4));           // gamma
    double dark = (1 - x) * (1 - x), light = x * x;
    if (t.shadows > 0)    x += t.shadows * 0.6 * dark * (1 - x);  // lift darks
    else                  x *= (1 + t.shadows * 0.8 * dark);      // crush darks
    if (t.highlights > 0) x += t.highlights * 0.5 * light * (1 - x);
    else                  x *= (1 + t.highlights * 0.5 * light);
    x = clamp01(x);
    x = t.contrast * (x - 0.5) + 0.5;                          // contrast
    x = t.gain * x + t.lift;                                   // brightness + lift
    return clamp01(x);
}

static void channel_scales(const Tuning &t, double s[3]) {
    s[0] = (1 + 0.25 * t.temp) * t.tint_r;
    s[1] = (1 - 0.05 * t.temp) * t.tint_g;
    s[2] = (1 - 0.25 * t.temp) * t.tint_b;
}

// build one channel LUT = tone(x) * channel_scale
static GVariant *make_ramp(std::vector<guint16> &buf, const Tuning &t, double scale) {
    buf.resize(RAMP_N);
    for (int i = 0; i < RAMP_N; i++) {
        double x = (double)i / (RAMP_N - 1);
        buf[i] = (guint16)std::round(clamp01(tone(x, t) * scale) * 65535);
    }
    return g_variant_new_fixed_array(G_VARIANT_TYPE_UINT16, buf.data(),
                                     buf.size(), sizeof(guint16));
}

// ------------------------------------------------------------------ apply ----
static void on_set_done(GObject *, GAsyncResult *res, gpointer ud) {
    GError *err = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(A.proxy, res, &err);
    const char *what = (const char *)ud;
    if (r) {
        char buf[200];
        snprintf(buf, sizeof buf,
            "Applied · gain %.2f · contrast %.2f · gamma %.2f · temp %+.2f",
            A.cur.gain, A.cur.contrast, A.cur.gamma, A.cur.temp);
        gtk_label_set_text(GTK_LABEL(A.status), buf);
        g_variant_unref(r);
    } else if (err) {
        char buf[240];
        snprintf(buf, sizeof buf, "%s failed: %s", what, err->message);
        gtk_label_set_text(GTK_LABEL(A.status), buf);
        g_clear_error(&err);
    }
}

static gboolean do_apply(gpointer) {
    A.apply_pending = 0;
    const OutputInfo *o = selected_output();
    if (!o || !ensure_proxy()) return G_SOURCE_REMOVE;
    GVariant *res = g_dbus_proxy_call_sync(A.proxy, "GetResources", g_variant_new("()"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
    if (!res) return G_SOURCE_REMOVE;
    guint serial = g_variant_get_uint32(g_variant_get_child_value(res, 0));
    g_variant_unref(res);

    // 1) LUT (tone)
    double s[3]; channel_scales(A.cur, s);
    static std::vector<guint16> rr, gg, bb;
    GVariant *lut = g_variant_new("(uu@aq@aq@aq)", serial, (guint32)o->crtc,
        make_ramp(rr, A.cur, s[0]),
        make_ramp(gg, A.cur, s[1]),
        make_ramp(bb, A.cur, s[2]));
    g_dbus_proxy_call(A.proxy, "SetCrtcGamma", lut, G_DBUS_CALL_FLAGS_NONE,
        2000, nullptr, on_set_done, (gpointer)"lut");

    save_state(A.cur, o->conn);
    return G_SOURCE_REMOVE;
}

static void schedule_apply() {
    if (!A.apply_pending) A.apply_pending = g_timeout_add(30, do_apply, nullptr);
}

// --------------------------------------------------------------------- UI ----
static void sync_labels() {
    for (int i = 0; i < NSPEC; i++) {
        char b[24];
        double v = A.cur.*(SPECS[i].field);
        if (SPECS[i].decimals == 0) snprintf(b, sizeof b, "%+.0f", v);
        else snprintf(b, sizeof b, "%.*f", SPECS[i].decimals, v);
        gtk_label_set_text(GTK_LABEL(A.val[i]), b);
    }
}

static void sliders_from_state() {
    A.suppress = true;
    for (int i = 0; i < NSPEC; i++)
        gtk_range_set_value(GTK_RANGE(A.scale[i]), A.cur.*(SPECS[i].field));
    A.suppress = false;
    sync_labels();
}

static void on_scale(GtkRange *r, gpointer ud) {
    if (A.suppress) return;
    int i = (int)(gssize)ud;
    A.cur.*(SPECS[i].field) = gtk_range_get_value(r);
    sync_labels();
    schedule_apply();
}

static void on_reset(GtkButton *, gpointer) {
    A.cur = Tuning();
    sliders_from_state();
    schedule_apply();
}

static GtkWidget *make_slider_row(int i) {
    const SliderSpec &sp = SPECS[i];
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 7);   gtk_widget_set_margin_bottom(box, 7);
    GtkWidget *lb = gtk_label_new(sp.label);
    gtk_widget_set_size_request(lb, 100, -1);
    gtk_widget_add_css_class(lb, "caption");
    A.scale[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, sp.lo, sp.hi, sp.step);
    gtk_scale_set_draw_value(GTK_SCALE(A.scale[i]), FALSE);
    gtk_widget_set_hexpand(A.scale[i], TRUE);
    A.val[i] = gtk_label_new("");
    gtk_widget_set_size_request(A.val[i], 52, -1);
    gtk_widget_add_css_class(A.val[i], "monospace");
    gtk_box_append(GTK_BOX(box), lb);
    gtk_box_append(GTK_BOX(box), A.scale[i]);
    gtk_box_append(GTK_BOX(box), A.val[i]);
    g_signal_connect(A.scale[i], "value-changed", G_CALLBACK(on_scale), (gpointer)(gssize)i);
    return box;
}

static GtkWidget *make_card(const char *title, int from, int to) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    GtkWidget *h = gtk_label_new(title);
    gtk_widget_add_css_class(h, "heading");
    gtk_widget_set_halign(h, GTK_ALIGN_START);
    gtk_widget_set_margin_start(h, 12); gtk_widget_set_margin_top(h, 10);
    gtk_widget_set_margin_bottom(h, 2);
    gtk_box_append(GTK_BOX(card), h);
    for (int i = from; i <= to; i++) gtk_box_append(GTK_BOX(card), make_slider_row(i));
    return card;
}

// preset chips ----------------------------------------------------------------
static void apply_preset_idx(size_t idx) {
    if (idx >= A.presets.size()) return;
    A.cur = A.presets[idx].t;
    sliders_from_state();
    schedule_apply();
}
static void on_chip_clicked(GtkButton *, gpointer ud) { apply_preset_idx((size_t)ud); }

static void rebuild_preset_chips();
static void on_chip_middle(GtkGestureClick *, gint, gdouble, gdouble, gpointer ud) {
    size_t idx = (size_t)ud;
    if (idx >= A.presets.size() || A.presets[idx].builtin) return;
    A.presets.erase(A.presets.begin() + idx);
    for (size_t i = 0; i < A.presets.size(); i++) A.presets[i].idx = i;
    write_user_presets();
    rebuild_preset_chips();
}
static GtkWidget *make_chip(const Preset &pr) {
    GtkWidget *btn = gtk_button_new_with_label(pr.name.c_str());
    gtk_widget_set_tooltip_text(btn, pr.builtin ? "Click to apply"
        : "Click to apply · Middle-click to delete");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_chip_clicked), (gpointer)pr.idx);
    if (!pr.builtin) {
        GtkGesture *mc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mc), 2);
        g_signal_connect(mc, "pressed", G_CALLBACK(on_chip_middle), (gpointer)pr.idx);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mc));
    }
    return btn;
}
static void rebuild_preset_chips() {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(A.preset_flow)))
        gtk_flow_box_remove(GTK_FLOW_BOX(A.preset_flow), child);
    for (auto &pr : A.presets) gtk_flow_box_append(GTK_FLOW_BOX(A.preset_flow), make_chip(pr));
}

static void on_save_preset(GtkButton *, gpointer ud) {
    GtkWidget *entry = (GtkWidget *)ud;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!name || !*name) return;
    for (auto &e : A.presets) if (!e.builtin && e.name == name) {
        e.t = A.cur; write_user_presets(); gtk_editable_set_text(GTK_EDITABLE(entry), ""); return;
    }
    Preset pr; pr.name = name; pr.t = A.cur;
    A.presets.push_back(pr);
    for (size_t i = 0; i < A.presets.size(); i++) A.presets[i].idx = i;
    write_user_presets();
    rebuild_preset_chips();
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

// ------------------------------------------------------------------ activate ----
static void activate(GtkApplication *) {
    refresh_outputs();
    load_presets();
    std::string saved_monitor;
    A.cur = load_state(&saved_monitor);

    A.window = gtk_application_window_new(A.app);
    gtk_window_set_title(GTK_WINDOW(A.window), "ExtMon");
    gtk_window_set_default_size(GTK_WINDOW(A.window), 480, 740);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset, "pill");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), reset);
    gtk_window_set_titlebar(GTK_WINDOW(A.window), header);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), nullptr);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 14); gtk_widget_set_margin_end(root, 14);
    gtk_widget_set_margin_top(root, 12);  gtk_widget_set_margin_bottom(root, 14);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), root);

    // output selector
    std::vector<const char *> names;
    for (auto &o : A.outputs) names.push_back(o.name.c_str());
    names.push_back(nullptr);
    A.output_drop = gtk_drop_down_new_from_strings(names.data());
    size_t pref = (size_t)-1;
    for (size_t i = 0; i < A.outputs.size(); i++) {
        if (!saved_monitor.empty() && A.outputs[i].conn == saved_monitor) { pref = i; break; }
        if (pref == (size_t)-1 &&
            (A.outputs[i].conn.rfind("HDMI", 0) == 0 || A.outputs[i].conn.rfind("DP", 0) == 0))
            pref = i;
    }
    if (pref != (size_t)-1) gtk_drop_down_set_selected(GTK_DROP_DOWN(A.output_drop), pref);
    g_signal_connect_swapped(A.output_drop, "notify::selected", G_CALLBACK(schedule_apply), nullptr);
    GtkWidget *out_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(out_card, "card");
    gtk_widget_set_margin_start(out_card, 12); gtk_widget_set_margin_end(out_card, 12);
    gtk_widget_set_margin_top(out_card, 10);  gtk_widget_set_margin_bottom(out_card, 10);
    GtkWidget *ol = gtk_label_new("Monitor");
    gtk_widget_add_css_class(ol, "caption");
    gtk_widget_set_hexpand(A.output_drop, TRUE);
    gtk_box_append(GTK_BOX(out_card), ol);
    gtk_box_append(GTK_BOX(out_card), A.output_drop);

    gtk_box_append(GTK_BOX(root), out_card);
    gtk_box_append(GTK_BOX(root), make_card("Light & Tone", 0, 5));
    gtk_box_append(GTK_BOX(root), make_card("Color", 6, 9));

    // presets
    GtkWidget *preset_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(preset_card, "card");
    gtk_widget_set_margin_start(preset_card, 12); gtk_widget_set_margin_end(preset_card, 12);
    gtk_widget_set_margin_top(preset_card, 12);  gtk_widget_set_margin_bottom(preset_card, 12);
    GtkWidget *ph = gtk_label_new("Presets");
    gtk_widget_add_css_class(ph, "heading");
    gtk_widget_set_halign(ph, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(preset_card), ph);
    A.preset_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(A.preset_flow), 4);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(A.preset_flow), GTK_SELECTION_NONE);
    gtk_box_append(GTK_BOX(preset_card), A.preset_flow);
    GtkWidget *save_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *save_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(save_entry), "New preset name");
    gtk_widget_set_hexpand(save_entry, TRUE);
    GtkWidget *save_btn = gtk_button_new_with_label("Save current");
    gtk_widget_add_css_class(save_btn, "suggested-action");
    gtk_box_append(GTK_BOX(save_row), save_entry);
    gtk_box_append(GTK_BOX(save_row), save_btn);
    gtk_box_append(GTK_BOX(preset_card), save_row);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_preset), save_entry);
    gtk_box_append(GTK_BOX(root), preset_card);

    A.status = gtk_label_new("Ready");
    gtk_widget_add_css_class(A.status, "caption");
    gtk_widget_set_halign(A.status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), A.status);

    gtk_window_set_child(GTK_WINDOW(A.window), scrolled);
    rebuild_preset_chips();
    sliders_from_state();
    gtk_window_present(GTK_WINDOW(A.window));
    schedule_apply();
}

int main(int argc, char **argv) {
    A.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(A.app, "activate", G_CALLBACK(activate), nullptr);
    int st = g_application_run(G_APPLICATION(A.app), argc, argv);
    g_object_unref(A.app);
    return st;
}
