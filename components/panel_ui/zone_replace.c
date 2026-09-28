#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES */
#include "pnl_input.h"      /* PCFG_KB_HEX, pnl_mac_parse, pnl_zero */
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "zone_sections.h"

/* The web's Replace board card (app.js:840-845, 1684-1700): SET NODE <zone> MAC <mac> on the panel's command session,
 * the reply shown verbatim. One replace in flight at a time, panel-wide. */

static char     s_mac[HG_MAX_ZONES][18];   /* per-zone draft, like the web's macDraftKey (app.js:62-81) */
static uint8_t  s_zone, s_busy;
static lv_obj_t *s_mac_lbl, *s_out, *s_btn;

typedef struct { uint8_t zone; char line[48]; } rep_arg_t;

static void show_draft(void) {
    if (!s_mac_lbl || !s_zone) return;
    lv_label_set_text(s_mac_lbl, s_mac[s_zone - 1][0] ? s_mac[s_zone - 1] : "aa:bb:cc:dd:ee:ff");
}

static void rep_run(pnl_job_t *j) {
    const rep_arg_t *a = (const rep_arg_t *)j->arg;
    j->irc = pnl_cmd_run(a->line, (char *)j->out, PNL_JOB_OUT_MAX);
}

static void rep_done(pnl_job_t *j) {
    const rep_arg_t *a = (const rep_arg_t *)j->arg;
    s_busy = 0;
    if (j->irc == 0) pnl_zero(s_mac[a->zone - 1], sizeof s_mac[0]);   /* the web clears the draft on success only */
    /* Widget pointers are cleared by zone_replace_teardown(), so they are the liveness test (not screen_gen): the
     * section rebuilt while this ran was built with Replace disabled and must be re-enabled now. */
    pnl_kit_enable(s_btn, 1);
    if (!s_out || s_zone != a->zone) return;
    pnl_kit_msg_set(s_out, (const char *)j->out, j->irc == 0 ? PNL_KIT_OK : PNL_KIT_ERR);   /* the reply, verbatim */
    show_draft();
}

static void mac_kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (!accepted || !s_zone || !s_mac_lbl) return;
    pnl_zero(s_mac[s_zone - 1], sizeof s_mac[0]);
    snprintf(s_mac[s_zone - 1], sizeof s_mac[0], "%s", text ? text : "");
    show_draft();
}

static void mac_click(lv_event_t *e) {
    (void)e;
    if (!s_zone) return;
    char title[40];
    snprintf(title, sizeof title, "New board MAC for zone %u", (unsigned)s_zone);
    wdg_keyboard_open(title, PCFG_KB_HEX, s_mac[s_zone - 1], 1, 17, 0, mac_kb_done, NULL);
}

static void replace_click(lv_event_t *e) {
    (void)e;
    if (s_busy || !s_zone) return;
    uint8_t mac[6];
    const char *d = s_mac[s_zone - 1];
    if (pnl_mac_parse(d, mac) != 0) { pnl_kit_msg_set(s_out, "Enter a MAC like aa:bb:cc:dd:ee:ff", PNL_KIT_ERR); return; }
    rep_arg_t a;
    memset(&a, 0, sizeof a);
    a.zone = s_zone;
    snprintf(a.line, sizeof a.line, "SET NODE %u MAC %s", (unsigned)s_zone, d);
    int rc = pnl_worker_submit(rep_run, rep_done, &a, sizeof a);   /* the pool wipes its copy of arg after done() */
    pnl_zero(&a, sizeof a);
    if (rc != 0) {
        pnl_kit_msg_set(s_out, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_busy = 1;
    pnl_kit_enable(s_btn, 0);
    pnl_kit_msg_set(s_out, "...", PNL_KIT_INFO);
}

void zone_replace_build(lv_obj_t *parent, uint8_t zone) {
    s_zone = zone;
    lv_obj_t *card = pnl_kit_card(parent, "Replace board");
    lv_obj_t *note = pnl_kit_msg(card);
    pnl_kit_msg_set(note, "Binds this zone slot to a new board's MAC (SET NODE <zone> MAC <mac>).", PNL_KIT_INFO);
    s_mac_lbl = pnl_kit_field(card, "New MAC", mac_click, NULL);
    s_btn = pnl_kit_button(card, "Replace", replace_click, NULL);
    pnl_kit_enable(s_btn, !s_busy);
    s_out = pnl_kit_msg(card);
    show_draft();
}

void zone_replace_teardown(void) {
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    s_mac_lbl = s_out = s_btn = NULL;
    s_zone = 0;
}

void zone_replace_wipe(void) {
    pnl_zero(s_mac, sizeof s_mac);
    pnl_kit_msg_set(s_out, "", PNL_KIT_INFO);
    show_draft();
}
