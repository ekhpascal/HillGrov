#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES */
#include "pnl_input.h"      /* PCFG_KB_TEXT, pnl_zero */
#include "pnl_console.h"
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "zone_sections.h"

/* The web's zone console (app.js:1647-1682) on the panel's own command session (pnl_cmd, CMD_SRC_HTTP). One line in
 * flight at a time, panel-wide. Transcripts, history and drafts live in PSRAM and survive navigation, like the web's;
 * zone_console_wipe_all() is the idle wipe (Task 27). */

static const char *TAG = "zone_console";

#define CON_VIEW_MAX   8192u   /* transcript text shown on glass (PSRAM, lv_label_set_text_static: never copied into LVGL's pool) */
#define CON_REPLY_VIEW 600     /* characters of each reply shown; the stored reply stays whole */

typedef struct { pnl_console_t con; char store[PNL_CON_LOG][CMD_RESP_MAX]; } zcon_t;   /* ~88 KB each, PSRAM */
typedef struct { uint8_t zone; pnl_con_entry_t *e; } con_arg_t;

static zcon_t  *s_con[HG_MAX_ZONES];   /* lazily allocated, never freed; a zone's console survives navigation */
static char    *s_view;                /* CON_VIEW_MAX */
static char    *s_scratch;             /* CMD_RESP_MAX: the worker writes the reply here, done() copies it */
static uint8_t  s_zone;                /* zone shown, 0 = not built */
static uint8_t  s_busy_zone;           /* zone whose line is in flight, 0 = none (one line at a time, panel-wide) */
static lv_obj_t *s_log, *s_log_lbl, *s_draft_lbl, *s_fwd_cb, *s_msg;

static zcon_t *con_for(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return NULL;
    zcon_t **pp = &s_con[zone - 1];
    if (!*pp) {
        *pp = heap_caps_calloc(1, sizeof **pp, MALLOC_CAP_SPIRAM);
        if (*pp) pnl_con_init(&(*pp)->con, (*pp)->store);
        else ESP_LOGE(TAG, "no PSRAM for zone %u's console", (unsigned)zone);
    }
    return *pp;
}

static void render(void) {
    zcon_t *z = con_for(s_zone);
    if (!z || !s_log_lbl || !s_view) return;
    pnl_zero(s_view, CON_VIEW_MAX);                        /* a shorter transcript must not leave the old one's tail (C3) */
    size_t o = 0;
    for (int first = 0; first < z->con.n_log; first++) {   /* drop the oldest until the rest fits */
        int fits = 1;
        o = 0;
        for (int i = first; i < z->con.n_log && fits; i++) {
            const pnl_con_entry_t *e = pnl_con_log_at(&z->con, i);
            if (!e || !e->used) continue;
            int w = snprintf(s_view + o, CON_VIEW_MAX - o, "> %s\n%.*s\n", e->sent, CON_REPLY_VIEW,
                             e->pending ? "..." : e->reply);
            if (w < 0 || (size_t)w >= CON_VIEW_MAX - o) fits = 0;
            else o += (size_t)w;
        }
        if (fits) break;
    }
    if (o == 0) snprintf(s_view, CON_VIEW_MAX, "No commands sent yet.");
    lv_label_set_text_static(s_log_lbl, s_view);
    lv_obj_update_layout(s_log);                           /* the label's new height, so the scroll reaches the end */
    lv_obj_scroll_to_y(s_log, LV_COORD_MAX, LV_ANIM_OFF);
    lv_label_set_text(s_draft_lbl, z->con.draft[0] ? z->con.draft : "Tap to type a command");
    if (s_fwd_cb) {
        if (z->con.forward) lv_obj_add_state(s_fwd_cb, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_fwd_cb, LV_STATE_CHECKED);
    }
}

static void con_run(pnl_job_t *j) {
    const con_arg_t *a = (const con_arg_t *)j->arg;
    j->irc = pnl_cmd_run(a->e->sent, s_scratch, CMD_RESP_MAX);
}

static void con_done(pnl_job_t *j) {
    const con_arg_t *a = (const con_arg_t *)j->arg;
    pnl_con_reply(a->e, s_scratch);
    pnl_zero(s_scratch, CMD_RESP_MAX);
    s_busy_zone = 0;
    /* The section's widget pointers are cleared by zone_console_teardown(), so they are the liveness test here rather
     * than screen_gen: leaving and reopening the same zone must still see its pending "..." replaced. */
    if (s_log_lbl && s_zone == a->zone) {
        pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
        render();
    }
}

/* line: trimmed, 1..CMD_LINE_MAX-1 bytes. sent: CMD_LINE_MAX scratch. 1 queued (log entry + history, draft cleared) / 0 */
static int queue_line(zcon_t *z, const char *line, char *sent) {
    if (z->con.forward) {
        if (pnl_con_forward(line, s_zone, sent, CMD_LINE_MAX) != 0) {
            pnl_kit_msg_set(s_msg, "Line too long once addressed to the zone -- 191 characters at most", PNL_KIT_ERR);
            return 0;
        }
    } else {
        snprintf(sent, CMD_LINE_MAX, "%s", line);
    }
    pnl_con_entry_t *e = pnl_con_push(&z->con, sent);
    if (!e) { pnl_kit_msg_set(s_msg, "A command is still running -- wait for its reply", PNL_KIT_ERR); return 0; }
    pnl_con_hist_add(&z->con, line);             /* the ORIGINAL line, as the web keeps it */
    pnl_zero(z->con.draft, sizeof z->con.draft);
    con_arg_t a = { .zone = s_zone, .e = e };
    if (pnl_worker_submit(con_run, con_done, &a, sizeof a) != 0) {
        pnl_con_reply(e, "ERR BUSY (panel worker queue full)\n");
        pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_busy_zone = s_zone;
        pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
    }
    return 1;
}

/* text: what the operator typed. 1 when it went into the log (the draft is cleared then), 0 when refused. */
static int send_line(const char *text) {
    zcon_t *z = con_for(s_zone);
    if (!z || !s_scratch) { pnl_kit_msg_set(s_msg, "Console unavailable (no memory)", PNL_KIT_ERR); return 0; }
    size_t n;
    const char *p = pnl_con_span(text, &n);      /* the web's .trim(): space/tab/CR/LF at both ends */
    if (n == 0) return 0;                        /* only whitespace: nothing to send, nothing to complain about */
    if (n > (size_t)(CMD_LINE_MAX - 1)) {
        pnl_kit_msg_set(s_msg, "Line too long -- 191 characters at most", PNL_KIT_ERR);
        return 0;
    }
    if (s_busy_zone) { pnl_kit_msg_set(s_msg, "A command is still running -- wait for its reply", PNL_KIT_ERR); return 0; }
    char line[CMD_LINE_MAX], sent[CMD_LINE_MAX];
    pnl_zero(line, sizeof line);
    memcpy(line, p, n);
    int queued = queue_line(z, line, sent);
    pnl_zero(line, sizeof line);   /* a console line can carry a credential (C3) */
    pnl_zero(sent, sizeof sent);
    if (queued) render();
    return queued;
}

static void kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    zcon_t *z = con_for(s_zone);
    if (!z || !s_draft_lbl) return;             /* the section was torn down while the keyboard was open */
    if (!accepted) return;                      /* Cancel: con.draft is left as it was */
    if (!text) text = "";
    /* Keep what was typed as the draft when it could ever be sent, so a refusal (a line still running) loses nothing;
     * an over-long line is refused visibly by send_line and never cut down to fit the draft. */
    if (pnl_con_line_ok(text)) {
        pnl_zero(z->con.draft, sizeof z->con.draft);   /* a shorter line must not leave the old draft's tail (C3) */
        snprintf(z->con.draft, sizeof z->con.draft, "%s", text);
    }
    send_line(text);
    render();
}

static void edit_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    char title[40];
    snprintf(title, sizeof title, "Command (zone %u)", (unsigned)s_zone);
    /* max_len CMD_LINE_MAX, one over the limit, so an over-long line is refused here, visibly, not silently cut */
    wdg_keyboard_open(title, PCFG_KB_TEXT, z->con.draft, 0, CMD_LINE_MAX, 0, kb_done, NULL);
}

static void send_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z || !z->con.draft[0]) return;
    char d[CMD_LINE_MAX];
    snprintf(d, sizeof d, "%s", z->con.draft);
    send_line(d);
    pnl_zero(d, sizeof d);
}

static void prev_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    const char *h = pnl_con_hist_prev(&z->con);
    if (h) {
        pnl_zero(z->con.draft, sizeof z->con.draft);   /* the draft it replaces may be an unsent credential (C3) */
        snprintf(z->con.draft, sizeof z->con.draft, "%s", h);
    }
    render();
}

static void next_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    const char *h = pnl_con_hist_next(&z->con);
    pnl_zero(z->con.draft, sizeof z->con.draft);
    snprintf(z->con.draft, sizeof z->con.draft, "%s", h);
    render();
}

static void fwd_changed(lv_event_t *e) {
    zcon_t *z = con_for(s_zone);
    if (z) z->con.forward = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1 : 0;
}

void zone_console_build(lv_obj_t *parent, uint8_t zone) {
    s_zone = zone;
    if (!s_view) s_view = heap_caps_calloc(1, CON_VIEW_MAX, MALLOC_CAP_SPIRAM);
    if (!s_scratch) s_scratch = heap_caps_calloc(1, CMD_RESP_MAX, MALLOC_CAP_SPIRAM);
    char title[32];
    snprintf(title, sizeof title, "Console -- zone %u", (unsigned)zone);
    lv_obj_t *card = pnl_kit_card(parent, title);
    s_log = lv_obj_create(card);
    lv_obj_set_size(s_log, LV_PCT(100), 220);
    s_log_lbl = lv_label_create(s_log);
    lv_obj_set_width(s_log_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *dr = pnl_kit_row(card);
    lv_obj_t *db = lv_button_create(dr);
    lv_obj_set_height(db, 56);
    lv_obj_set_flex_grow(db, 1);
    lv_obj_add_event_cb(db, edit_click, LV_EVENT_CLICKED, NULL);
    s_draft_lbl = lv_label_create(db);
    lv_obj_set_width(s_draft_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_draft_lbl, LV_LABEL_LONG_MODE_DOTS);   /* a 191-character draft stays one line */
    lv_obj_align(s_draft_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    pnl_kit_button(dr, "Send", send_click, NULL);
    lv_obj_t *r = pnl_kit_row(card);
    pnl_kit_button(r, LV_SYMBOL_UP " Prev", prev_click, NULL);
    pnl_kit_button(r, LV_SYMBOL_DOWN " Next", next_click, NULL);
    s_fwd_cb = lv_checkbox_create(r);
    char fw[32];
    snprintf(fw, sizeof fw, "Forward to zone %u", (unsigned)zone);
    lv_checkbox_set_text(s_fwd_cb, fw);
    lv_obj_add_event_cb(s_fwd_cb, fwd_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_msg = pnl_kit_msg(card);
    if (!con_for(zone) || !s_view || !s_scratch) pnl_kit_msg_set(s_msg, "Console unavailable (no memory)", PNL_KIT_ERR);
    render();
}

void zone_console_teardown(void) {
    if (wdg_keyboard_is_open()) wdg_keyboard_close();   /* the typed line goes with the screen (wiped by the keyboard) */
    s_log = s_log_lbl = s_draft_lbl = s_fwd_cb = s_msg = NULL;
    s_zone = 0;
}

void zone_console_wipe_all(void) {
    for (int i = 0; i < HG_MAX_ZONES; i++)
        if (s_con[i]) pnl_con_wipe(&s_con[i]->con);   /* an entry still in flight is left to the worker */
    if (s_view) pnl_zero(s_view, CON_VIEW_MAX);
    if (s_log_lbl) render();
}
