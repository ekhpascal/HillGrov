/* scr_config.c -- the Config destination (glue; LVGL task only).
 * Owns the picker (Master + enrolled zones), the generated-editor frame both editors share (tabs in table order,
 * shelf/aux index selector, Save, row list), the title, the status line and the save-state reconciliation both
 * editors use (cfg_save_*). The editors (cfg_zone.c, cfg_master.c) own their documents, edit sets and worker jobs. */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "scr_config.h"
#include "scr_shell.h"
#include "pnl_fmt.h"
#include "pnl_input.h"
#include "pnl_palette.h"
#include "hg_mcfg.h"
#include "wdg_keyboard.h"

static const char *TAG = "scr_config";

#define CFG_ROWS_MAX 16         /* the largest group (HWSHELF) has 11 rows */
#define CFG_SLOW_US  200000     /* the <= 200 ms callback budget */
#define PICK_TXT     24
/* tabs, index and picker switch on RELEASE and never repeat while held (a held tab would re-render the list ~10x/s);
 * the same CLICK_TRIG | NO_REPEAT the Task 19 segmented control uses */
#define BTNM_CTRL    (LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)

typedef struct { lv_obj_t *row; uint8_t group; int idx; const hg_field_t *f; pcfg_kb_t kb; } cfg_row_t;

static struct {
    lv_obj_t *title, *picker, *body, *status;
    int       open_zone;                 /* -1 none, 0 master (Task 21), 1..8 a zone */
    uint16_t  used_mask;
    int       n_pick;
    uint8_t   zone_of[1 + HG_MAX_ZONES];
    uint8_t   route_pending;             /* built before the first poll: route on the first started snapshot */
    int       route_arg;
} s_ui = { .open_zone = -1 };

/* button-matrix maps must outlive their widgets: file scope, never cleared */
static char        s_pick_txt[1 + HG_MAX_ZONES][PICK_TXT];
static const char *s_pick_map[1 + HG_MAX_ZONES + 1];
static const char *s_tab_map[HG_G_COUNT + 2];
static const char *const IDX4[] = { "0", "1", "2", "3", "" };
static const char *const IDX2[] = { "0", "1", "" };

static struct {
    const cfg_view_t *v;
    cfg_save_fn       on_save;
    lv_obj_t         *tabs, *idxsel, *list, *save, *save_lbl;
    uint8_t           groups[HG_G_COUNT];
    int               n_groups, tab, idx, idx_scope;
    cfg_row_t         rows[CFG_ROWS_MAX];
    int               n_rows;
} s_fr;

static int s_last_zone = -1;                                        /* "-1 = last used" */
static int s_keep_table = -1, s_keep_zone = -1, s_keep_tab, s_keep_idx;
/* No snapshot copy here (controller ruling C11): the shell's PSRAM copy, pnl_shell_snap(), may be NULL. */

/* ---------- table helpers ---------- */
static const hg_field_t *rows_of(pcfg_table_t t, int *n) {
    if (t == PCFG_TABLE_MASTER) { *n = HG_MFIELD_COUNT; return HG_MFIELDS; }
    *n = HG_FIELD_COUNT;
    return HG_FIELDS;
}
static const char *gname(pcfg_table_t t, uint8_t g) { return t == PCFG_TABLE_MASTER ? HG_MGROUP_NAMES[g] : HG_GROUP_NAMES[g]; }
static int gscope(pcfg_table_t t, uint8_t g) { return t == PCFG_TABLE_MASTER ? 0 : hg_group_scope(g); }

/* ---------- title / status ---------- */
void cfg_set_title(const char *text) {
    if (s_ui.title) pnl_label_set_if_changed(s_ui.title, text ? text : "");
}
void cfg_set_status(const char *text, int is_error) {
    if (!s_ui.status) return;
    pnl_label_set_if_changed(s_ui.status, text ? text : "");
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(is_error ? PNL_C_OFFLINE_TEXT : PNL_C_MUTED), 0);
}

/* ---------- the save state both editors share (C14) ---------- */
void cfg_save_begin(cfg_save_t *s) {
    s->saving = 1;
    s->ui_saving = 1;
    s->kept[0] = '\0';
    cfg_frame_set_saving(1);
    cfg_set_status("Saving...", 0);
}
void cfg_save_end(cfg_save_t *s, int live, const char *msg, int is_err) {
    s->saving = 0;
    if (!live) {                                    /* THE RULE: a stale done() leaves its message for the next frame */
        snprintf(s->kept, sizeof s->kept, "%s", msg ? msg : "");
        s->kept_err = is_err ? 1 : 0;
        return;
    }
    s->kept[0] = '\0';
    s->ui_saving = 0;
    cfg_frame_set_saving(0);
    cfg_set_status(msg, is_err);
}
void cfg_save_sync(cfg_save_t *s, int frame_rebuilt) {
    if (!frame_rebuilt && s->ui_saving == s->saving) return;
    s->ui_saving = s->saving;                       /* a save that finished while this screen was rebuilt */
    cfg_frame_set_saving(s->saving);
    if (!s->saving && s->kept[0]) { cfg_set_status(s->kept, s->kept_err); s->kept[0] = '\0'; }
}
void cfg_save_forget(cfg_save_t *s) { s->kept[0] = '\0'; }

/* ---------- rows ---------- */
static cfg_row_t *row_find(const hg_field_t *f) {
    for (int i = 0; i < s_fr.n_rows; i++) if (s_fr.rows[i].f == f) return &s_fr.rows[i];
    return NULL;       /* one group/index is on screen at a time, so the row pointer is unique */
}
static void on_changed(void *ctx, uint8_t group, int idx, const hg_field_t *f, const char *raw) {
    const cfg_view_t *v = ctx;
    cfg_row_t *r = row_find(f);
    uint8_t mac[6];
    if (r && r->kb == PCFG_KB_HEX && pnl_mac_parse(raw, mac) != 0) {   /* the MAC rule before any MAC is recorded */
        wdg_field_set_dirty(r->row, 0);
        wdg_field_set_error(r->row, "Not a MAC (aa:bb:cc:dd:ee:ff)");
        return;
    }
    int rc = v->edits ? pcfg_edits_set(v->edits, group, idx, f, raw) : -3;
    if (rc == 0) return;
    if (r) wdg_field_set_dirty(r->row, 0);
    cfg_set_status(rc == -1 ? "Too many unsaved changes -- save first" :
                   rc == -2 ? "Hardware plane is read-only" : "Out of memory -- the change was not kept", 1);
}

int cfg_render_group(lv_obj_t *list, const cfg_view_t *v, uint8_t group, int idx) {
    int64_t t0 = esp_timer_get_time();
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    lv_obj_clean(list);
    s_fr.n_rows = 0;
    if (v->table == PCFG_TABLE_ZONE && hg_group_is_hw(group)) {
        lv_obj_t *n = lv_label_create(list);
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(n, lv_color_hex(PNL_C_MUTED), 0);
        lv_label_set_text(n, v->hw_or_null ? "hardware plane -- set at the zone console"
                                           : "hardware plane -- set at the zone console (not received from the zone yet: shown as 0)");
    }
    int nrows;
    const hg_field_t *tab = rows_of(v->table, &nrows);
    for (int i = 0; i < nrows && s_fr.n_rows < CFG_ROWS_MAX; i++) {
        const hg_field_t *f = &tab[i];
        if (f->group != group) continue;
        pcfg_spec_t sp;
        (void)pcfg_spec_for(v->table, f, &sp);       /* -1 still yields a READONLY/TEXT spec that renders */
        if (v->table == PCFG_TABLE_ZONE) pcfg_tighten(&sp, f, idx, v->hw_or_null);
        const psvc_fedit_t *e = v->edits ? pcfg_edits_get(v->edits, group, idx, f) : NULL;
        char buf[PSVC_FEDIT_TEXT_MAX];
        const char *raw;
        if (sp.kind == PCFG_K_SECRET) raw = "";      /* a secret never enters a row */
        else if (e) raw = e->text;
        else raw = v->value(v->ctx, group, idx, f, buf, sizeof buf);
        lv_obj_t *row = wdg_field_create(list, &sp, group, idx, f, raw, on_changed, (void *)v);
        if (!row) continue;
        if (e) wdg_field_set_dirty(row, 1);
        if (sp.kind == PCFG_K_SECRET && v->reveal) wdg_field_set_reveal(row, v->reveal, v->ctx);
        s_fr.rows[s_fr.n_rows++] = (cfg_row_t){ row, group, idx, f, sp.keyboard };
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > CFG_SLOW_US) ESP_LOGW(TAG, "render %s took %lld ms", gname(v->table, group), (long long)(dt / 1000));
    return s_fr.n_rows;
}

/* ---------- frame ---------- */
static int frame_idx(void) {
    int sc = gscope(s_fr.v->table, s_fr.groups[s_fr.tab]);
    if (sc == 0) return -1;
    int max = (sc == 1 ? HG_MAX_SHELVES : HG_MAX_AUX) - 1;
    if (s_fr.idx < 0) s_fr.idx = 0;
    if (s_fr.idx > max) s_fr.idx = max;              /* a tab change clamps the index (app.js:1703-1715) */
    return s_fr.idx;
}
static void frame_render(void) {
    if (!s_fr.list || !s_fr.v) return;
    int ix = frame_idx();
    int sc = gscope(s_fr.v->table, s_fr.groups[s_fr.tab]);
    if (sc == 0) {
        lv_obj_add_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (sc != s_fr.idx_scope) {
            lv_buttonmatrix_set_map(s_fr.idxsel, sc == 1 ? IDX4 : IDX2);
            lv_buttonmatrix_set_button_ctrl_all(s_fr.idxsel, BTNM_CTRL);
            lv_obj_set_width(s_fr.idxsel, sc == 1 ? 4 * 72 : 2 * 72);
        }
        lv_buttonmatrix_set_button_ctrl(s_fr.idxsel, (uint32_t)ix, LV_BUTTONMATRIX_CTRL_CHECKED);
        lv_obj_remove_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
    }
    s_fr.idx_scope = sc;
    lv_buttonmatrix_set_button_ctrl(s_fr.tabs, (uint32_t)s_fr.tab, LV_BUTTONMATRIX_CTRL_CHECKED);
    s_keep_table = (int)s_fr.v->table; s_keep_zone = s_fr.v->zone; s_keep_tab = s_fr.tab; s_keep_idx = s_fr.idx;
    cfg_render_group(s_fr.list, s_fr.v, s_fr.groups[s_fr.tab], ix);
}
static void ev_tab(lv_event_t *e) {
    (void)e;
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_fr.tabs);
    if (sel >= (uint32_t)s_fr.n_groups || (int)sel == s_fr.tab) return;   /* a re-tap changes nothing */
    s_fr.tab = (int)sel;
    frame_render();
}
static void ev_idx(lv_event_t *e) {
    (void)e;
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_fr.idxsel);
    if (sel == LV_BUTTONMATRIX_BUTTON_NONE || (int)sel == s_fr.idx) return;
    s_fr.idx = (int)sel;
    frame_render();
}
static void ev_save(lv_event_t *e) {
    (void)e;
    if (s_fr.on_save) s_fr.on_save();
}

lv_obj_t *cfg_frame_build(lv_obj_t *body, const cfg_view_t *v, cfg_save_fn on_save) {
    memset(&s_fr, 0, sizeof s_fr);
    s_fr.v = v;
    s_fr.on_save = on_save;
    s_fr.idx_scope = -1;
    int nrows;
    const hg_field_t *rows = rows_of(v->table, &nrows);
    int ng = v->table == PCFG_TABLE_MASTER ? HG_MG_COUNT : HG_G_COUNT;
    for (int g = 0; g < ng; g++) {
        int has = 0;
        for (int i = 0; i < nrows && !has; i++) has = rows[i].group == g;
        if (has) s_fr.groups[s_fr.n_groups++] = (uint8_t)g;    /* WEB has no rows: it never appears */
    }
    int m = 0;
    for (int t = 0; t < s_fr.n_groups; t++) {
        if (t == 5 && s_fr.n_groups > 6) s_tab_map[m++] = "\n";  /* ten zone tabs: two rows of five */
        s_tab_map[m++] = gname(v->table, s_fr.groups[t]);
    }
    s_tab_map[m] = "";
    if (s_keep_table == (int)v->table && s_keep_zone == v->zone && s_keep_tab < s_fr.n_groups) {
        s_fr.tab = s_keep_tab;
        s_fr.idx = s_keep_idx;
    }

    s_fr.tabs = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(s_fr.tabs, s_tab_map);
    lv_buttonmatrix_set_button_ctrl_all(s_fr.tabs, BTNM_CTRL);
    lv_buttonmatrix_set_one_checked(s_fr.tabs, true);
    lv_obj_set_size(s_fr.tabs, lv_pct(100), s_fr.n_groups > 6 ? 112 : 56);
    lv_obj_add_event_cb(s_fr.tabs, ev_tab, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_fr.idxsel = lv_buttonmatrix_create(bar);
    lv_buttonmatrix_set_one_checked(s_fr.idxsel, true);
    lv_obj_set_height(s_fr.idxsel, 56);
    lv_obj_add_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_fr.idxsel, ev_idx, LV_EVENT_VALUE_CHANGED, NULL);
    s_fr.save = lv_button_create(bar);
    lv_obj_set_size(s_fr.save, 180, 56);
    lv_obj_add_flag(s_fr.save, LV_OBJ_FLAG_FLOATING);           /* stays right-aligned when idxsel is hidden */
    lv_obj_align(s_fr.save, LV_ALIGN_RIGHT_MID, 0, 0);
    s_fr.save_lbl = lv_label_create(s_fr.save);
    lv_label_set_text(s_fr.save_lbl, "Save");
    lv_obj_center(s_fr.save_lbl);
    lv_obj_add_event_cb(s_fr.save, ev_save, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_min_height(bar, 56, 0);

    s_fr.list = lv_obj_create(body);
    lv_obj_set_width(s_fr.list, lv_pct(100));
    lv_obj_set_flex_grow(s_fr.list, 1);
    lv_obj_set_flex_flow(s_fr.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_fr.list, 4, 0);
    lv_obj_set_style_pad_row(s_fr.list, 0, 0);
    frame_render();
    return s_fr.list;
}
void cfg_frame_rerender(void) { frame_render(); }
void cfg_frame_set_saving(int saving) {
    if (!s_fr.save) return;
    lv_label_set_text(s_fr.save_lbl, saving ? "Saving..." : "Save");
    lv_obj_set_state(s_fr.save, LV_STATE_DISABLED, saving != 0);
}

void cfg_show_error(const cfg_view_t *v, const char *path, const char *code) {
    char msg[160];
    uint8_t g;
    int ix;
    const hg_field_t *row;
    const char *c = (code && code[0]) ? code : "ERROR";
    /* an open keyboard is never yanked by a re-render: then the refusal is the banner alone */
    if (!s_fr.list || v != s_fr.v || !path || wdg_keyboard_is_open() || pcfg_locate(v->table, path, &g, &ix, &row) != 0) {
        snprintf(msg, sizeof msg, "%s%s%s", c, (path && path[0]) ? ": " : "", path ? path : "");
        cfg_set_status(msg, 1);
        return;
    }
    int sc = gscope(v->table, g);
    for (int t = 0; t < s_fr.n_groups; t++) if (s_fr.groups[t] == g) s_fr.tab = t;
    /* Task 17 review: pcfg_locate returns idx -1 for a scoped group named without "shelf[N]." / "aux[N]."
     * ("hw.HWSHELF.PUMP", hg_cfg_validate's "aux.pulse_s"). That -1 is never an index: the tab switches, the index
     * stays where it was, and no row is outlined -- the row on screen may belong to another shelf. */
    int indexed = sc == 0 || ix >= 0;
    if (sc != 0 && ix >= 0) s_fr.idx = ix;
    frame_render();
    if (indexed) {
        cfg_row_t *r = row_find(row);
        if (r) wdg_field_set_error(r->row, c);
    }
    pcfg_spec_t sp;
    (void)pcfg_spec_for(v->table, row, &sp);
    if (sc == 0) snprintf(msg, sizeof msg, "%s: %s", c, sp.label);
    else if (ix >= 0) snprintf(msg, sizeof msg, "%s: %s (%s %d)", c, sp.label, sc == 1 ? "shelf" : "aux", ix);
    else snprintf(msg, sizeof msg, "%s: %s (%s index not reported)", c, sp.label, sc == 1 ? "shelf" : "aux");
    cfg_set_status(msg, 1);
}

/* ---------- picker ---------- */
static int zone_used(const pnl_snap_t *s, int z) {   /* node ids are untrusted: slot = id-1 (psvc_state_fill) */
    return s && z >= 1 && z <= HG_MAX_ZONES && s->st.node[z - 1].used && s->st.node[z - 1].id == (uint8_t)z;
}
static uint16_t used_mask(const pnl_snap_t *s) {
    uint16_t m = 0;
    for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) m |= (uint16_t)(1u << z);
    return m;
}
static void picker_check(int zone) {
    if (!s_ui.picker) return;
    lv_buttonmatrix_clear_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_CHECKED);
    for (int i = 0; i < s_ui.n_pick; i++)
        if (zone >= 0 && s_ui.zone_of[i] == zone) lv_buttonmatrix_set_button_ctrl(s_ui.picker, (uint32_t)i, LV_BUTTONMATRIX_CTRL_CHECKED);
}
static int master_enabled(void);
static void picker_rebuild(const pnl_snap_t *s) {   /* s NULL (no snapshot yet): Master only */
    int n = 0;
    snprintf(s_pick_txt[0], PICK_TXT, "Master");
    s_pick_map[n] = s_pick_txt[0];
    s_ui.zone_of[n++] = 0;
    for (int z = 1; z <= HG_MAX_ZONES; z++) {
        if (!zone_used(s, z)) continue;
        char nm[17];
        pnl_zone_name(&s->st.node[z - 1], nm);
        snprintf(s_pick_txt[n], PICK_TXT, "%s", nm);
        s_pick_map[n] = s_pick_txt[n];
        s_ui.zone_of[n++] = (uint8_t)z;
    }
    s_pick_map[n] = "";
    s_ui.n_pick = n;
    s_ui.used_mask = used_mask(s);
    lv_buttonmatrix_set_map(s_ui.picker, s_pick_map);
    lv_buttonmatrix_clear_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_DISABLED);   /* same count keeps old bits */
    lv_buttonmatrix_set_button_ctrl_all(s_ui.picker, BTNM_CTRL);
    if (!master_enabled()) lv_buttonmatrix_set_button_ctrl(s_ui.picker, 0, LV_BUTTONMATRIX_CTRL_DISABLED);
    picker_check(s_ui.open_zone);
}

/* ---------- editor routing (Task 21 replaces this whole block to add the master) ---------- */
static int master_enabled(void) { return 0; }
static void close_editor(void) {
    if (s_ui.open_zone >= 1) cfg_zone_close();
    s_ui.open_zone = -1;
    s_fr.v = NULL; s_fr.list = NULL; s_fr.tabs = NULL; s_fr.idxsel = NULL; s_fr.save = NULL; s_fr.save_lbl = NULL;
    s_fr.n_rows = 0;
}
static void open_editor(int zone) {
    close_editor();
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    lv_obj_clean(s_ui.body);
    cfg_set_status("", 0);
    s_ui.open_zone = zone;
    picker_check(zone);
    if (zone >= 1) { s_last_zone = zone; cfg_zone_open(s_ui.body, (uint8_t)zone); return; }
    cfg_set_title("Config");
    lv_obj_t *l = lv_label_create(s_ui.body);
    lv_label_set_text(l, "No zones enrolled.");
}
static int pick_target(const pnl_snap_t *s, int arg) {
    if (arg >= 1 && zone_used(s, arg)) return arg;
    if (s_last_zone >= 1 && zone_used(s, s_last_zone)) return s_last_zone;
    for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) return z;
    return -1;
}
static void ev_pick(lv_event_t *e) {
    (void)e;
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_ui.picker);
    if (sel >= (uint32_t)s_ui.n_pick) return;
    int z = s_ui.zone_of[sel];
    if (z == 0 || z == s_ui.open_zone) { picker_check(s_ui.open_zone); return; }
    open_editor(z);
}
static void route_update(const pnl_snap_t *snap) {
    if (s_ui.open_zone >= 1) { cfg_zone_update(snap); return; }
    int z = pick_target(snap, -1);
    if (z >= 1) open_editor(z);
}
/* ---------- end of editor routing ---------- */

/* ---------- the destination ---------- */
static void route_first(const pnl_snap_t *sn, int arg) {   /* the build's routing, once a snapshot has started */
    int z = pick_target(sn, arg);
    open_editor(z);
    if (arg >= 1 && z != arg) {
        char m[48];
        snprintf(m, sizeof m, "Zone %d is not enrolled.", arg);
        cfg_set_status(m, 1);
    }
}
static void cfg_build(lv_obj_t *content, int arg) {
    int64_t t0 = esp_timer_get_time();
    s_ui.open_zone = -1; s_ui.n_pick = 0; s_ui.used_mask = 0; s_ui.route_pending = 0;
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    s_ui.title = lv_label_create(content);
    lv_obj_set_style_text_font(s_ui.title, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_ui.title, "Config");
    s_ui.picker = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_one_checked(s_ui.picker, true);
    lv_obj_set_size(s_ui.picker, lv_pct(100), 56);
    lv_obj_add_event_cb(s_ui.picker, ev_pick, LV_EVENT_VALUE_CHANGED, NULL);
    s_ui.body = lv_obj_create(content);
    lv_obj_remove_style_all(s_ui.body);
    lv_obj_set_width(s_ui.body, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.body, 1);
    lv_obj_set_flex_flow(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.body, 6, 0);
    s_ui.status = lv_label_create(content);
    lv_obj_set_width(s_ui.status, lv_pct(100));
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_ui.status, "");
    const pnl_snap_t *sn = pnl_shell_snap();
    if (sn && sn->started) {
        picker_rebuild(sn);
        route_first(sn, arg);
    } else {                                     /* NULL = no shell copy yet: the same as "not started" */
        picker_rebuild(NULL);
        lv_label_set_text(lv_label_create(s_ui.body), "Starting...");
        s_ui.route_pending = 1;
        s_ui.route_arg = arg;
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > CFG_SLOW_US) ESP_LOGW(TAG, "build took %lld ms", (long long)(dt / 1000));
}
static void cfg_update(const pnl_snap_t *snap) {
    if (!s_ui.picker || !snap || !snap->started) return;
    if (used_mask(snap) != s_ui.used_mask) picker_rebuild(snap);
    if (s_ui.route_pending) { s_ui.route_pending = 0; route_first(snap, s_ui.route_arg); return; }
    route_update(snap);
}
static void cfg_teardown(void) {
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    close_editor();
    s_ui.title = NULL; s_ui.picker = NULL; s_ui.body = NULL; s_ui.status = NULL;
    s_ui.route_pending = 0;
}

const pnl_screen_ops_t PNL_SCR_CONFIG = {
    .title = "Config", .build = cfg_build, .update = cfg_update, .teardown = cfg_teardown, .in_rail = 1,
};
