#include <stdio.h>
#include <string.h>
#include "cmd_core.h"
#include "notify.h"
#include "cmd_common.h"

static const app_if_t *s_app;

void cmd_common_init(const app_if_t *app) { s_app = app; }

static int no_app(char *r, int l) { return cmd_err(r, l, "INTERNAL"); }

/* ---- GET ID / VERSION / STATUS / FW ---- */

static int h_id(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    uint8_t mac[6];
    s_app->get_mac(mac);
    return cmd_okf(r, l, "ID %s %02x:%02x:%02x:%02x:%02x:%02x %u %s",
                   s_app->role_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                   (unsigned)s_app->zone_id(), s_app->node_name());
}

static int fw_info_reply(const char *word, char *r, int l) {
    if (!s_app) return no_app(r, l);
    char buf[64];
    if (s_app->fw_info(buf, sizeof buf) != 0) return cmd_err(r, l, "INTERNAL");
    return cmd_okf(r, l, "%s %s", word, buf);
}

static int h_version(cmd_req_t *q, char *r, int l) { (void)q; return fw_info_reply("VERSION", r, l); }
static int h_fw_get(cmd_req_t *q, char *r, int l)  { (void)q; return fw_info_reply("FW", r, l); }

static int h_status(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    cmd_okf(r, l, "STATUS");
    s_app->status_lines(r, l);
    return 0;
}

/* ---- SET|GET ECHO -- writes ses->echo ---- */

static int h_echo(cmd_req_t *q, char *r, int l) {
    if (q->verb == CMDV_SET) q->ses->echo = (uint8_t)q->val[0];
    return cmd_okf(r, l, "ECHO %s", q->ses->echo ? "ON" : "OFF");
}

/* ---- SET|GET NOTIFY -- edits ses->notify_mask, rendered via notify's own type names ---- */

static void notify_mask_str(uint16_t mask, char *out, size_t cap) {
    size_t off = 0;
    for (int t = 0; t < NTF_COUNT; t++) {
        int w = snprintf(out + off, cap - off, "%s%s=%d", t ? " " : "", notify_type_name(t),
                          (mask & NTF_MASK(t)) ? 1 : 0);
        if (w < 0 || off + (size_t)w >= cap) break;
        off += (size_t)w;
    }
}

static int h_notify(cmd_req_t *q, char *r, int l) {
    if (q->verb == CMDV_SET) {
        int t = notify_parse(q->tok[0]);
        if (t < 0) return cmd_err(r, l, "BAD_ARGS");
        uint16_t bit = (t == NTF_COUNT) ? NTF_MASK_ALL : NTF_MASK(t);
        if (q->val[1]) q->ses->notify_mask = (uint16_t)(q->ses->notify_mask | bit);
        else           q->ses->notify_mask = (uint16_t)(q->ses->notify_mask & ~bit);
    }
    char list[160];
    notify_mask_str(q->ses->notify_mask, list, sizeof list);
    return cmd_okf(r, l, "NOTIFY %s", list);
}

/* ---- SET LOG <level> [tag] / GET LOG -- app owns the effective-level table;
 * GET queries it by passing a NULL level (no change). ---- */

static const char *const LOG_LEVELS[] = { "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE" };

static int h_log_set(cmd_req_t *q, char *r, int l) {
    if (!s_app) return no_app(r, l);
    const char *tag = q->n > 1 ? q->tok[1] : NULL;
    char eff[16];
    if (s_app->log_set(LOG_LEVELS[q->val[0]], tag, eff, sizeof eff) != 0) return cmd_err(r, l, "BAD_ARGS");
    return cmd_okf(r, l, "LOG %s", eff);
}

static int h_log_get(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    char eff[16];
    if (s_app->log_set(NULL, NULL, eff, sizeof eff) != 0) return cmd_err(r, l, "INTERNAL");
    return cmd_okf(r, l, "LOG %s", eff);
}

/* ---- DEBUG ENABLE <key> / DEBUG DISABLE / GET DEBUG ---- */

static int h_debug_enable(cmd_req_t *q, char *r, int l) {
    if (!q->core->debug_key || strcmp(q->tok[0], q->core->debug_key) != 0) return cmd_err(r, l, "AUTH_FAILED");
    q->ses->unlock_until_ms = q->core->now_ms() + CMD_UNLOCK_MS;
    return cmd_okf(r, l, "DEBUG ENABLE %u", (unsigned)(CMD_UNLOCK_MS / 1000u));
}

static int h_debug_disable(cmd_req_t *q, char *r, int l) {
    q->ses->unlock_until_ms = 0;
    return cmd_okf(r, l, "DEBUG DISABLE");
}

static int h_debug_get(cmd_req_t *q, char *r, int l) {
    uint32_t now = q->core->now_ms();
    uint32_t remain = (q->ses->unlock_until_ms > now) ? (q->ses->unlock_until_ms - now) / 1000u : 0u;
    return cmd_okf(r, l, "DEBUG %u", (unsigned)remain);
}

/* ---- GET TIME / SET TIME <date> <time> -- date/time parsed here, not by cmd_core ---- */

static int h_time(cmd_req_t *q, char *r, int l) {
    if (!s_app) return no_app(r, l);
    if (q->verb == CMDV_GET) {
        char buf[48];
        if (s_app->time_get(buf, sizeof buf) != 0) return cmd_err(r, l, "INTERNAL");
        return cmd_okf(r, l, "TIME %s", buf);
    }
    /* sscanf's return value only counts successful conversions -- it stops
     * silently at the first non-matching character, so "2026-08-1x" (within
     * the ARG_STR length bound) would parse as y/mo/d with 'x' discarded.
     * %n plus a strlen() check forces full-token consumption. */
    int y, mo, d, h, mi, s, n = 0;
    if (sscanf(q->tok[0], "%d-%d-%d%n", &y, &mo, &d, &n) != 3 || n != (int)strlen(q->tok[0]))
        return cmd_err(r, l, "BAD_ARGS");
    n = 0;
    if (sscanf(q->tok[1], "%d:%d:%d%n", &h, &mi, &s, &n) != 3 || n != (int)strlen(q->tok[1]))
        return cmd_err(r, l, "BAD_ARGS");
    if (s_app->time_set(y, mo, d, h, mi, s) != 0) return cmd_err(r, l, "BAD_ARGS");
    return cmd_okf(r, l, "TIME %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

/* ---- SAVE / REBOOT / FACTORY RESET ---- */

static int h_save(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    if (s_app->save_flush(1000) != 0) return cmd_err(r, l, "NVS_WRITE");
    return cmd_okf(r, l, "SAVE");
}

static int h_reboot(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    s_app->reboot();
    return cmd_okf(r, l, "REBOOT");
}

static int h_factory_reset(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    if (s_app->factory_reset() != 0) return cmd_err(r, l, "INTERNAL");
    return cmd_okf(r, l, "FACTORY RESET");
}

/* ---- SET FW ROLLBACK <CONFIRM> / SET FW UPDATE <ssid> <pass> <url> ---- */

static int h_fw_rollback(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (!s_app) return no_app(r, l);
    if (s_app->fw_rollback() != 0) return cmd_err(r, l, "INTERNAL");
    return cmd_okf(r, l, "FW ROLLBACK");
}

/* spec §5.4: firmware updates take effect from the local console only, even
 * once the CMDF_UNLOCK/CMDF_ZONE/CMDF_SLOW gates above have let the request
 * through -- an HTTP-sourced caller never reaches an unlocked session. */
static int h_fw_update(cmd_req_t *q, char *r, int l) {
    if (!s_app) return no_app(r, l);
    if (q->ses->source != CMD_SRC_CLI) return cmd_err(r, l, "NOT_LOCAL");
    if (s_app->fw_update(q->tok[0], q->tok[1], q->tok[2]) != 0) return cmd_err(r, l, "INTERNAL");
    return cmd_okf(r, l, "FW UPDATE");
}

/* ---- row table ---- */

static const cmd_arg_t A_MODE[]  = { { "mode", ARG_ENUM, 0, 1, "OFF|ON" } };
static const cmd_arg_t A_NOTIFY[] = { { "type", ARG_STR, 0, 8, NULL }, { "mode", ARG_ENUM, 0, 1, "OFF|ON" } };
static const cmd_arg_t A_LOG[]   = { { "level", ARG_ENUM, 0, 5, "NONE|ERROR|WARN|INFO|DEBUG|VERBOSE" },
                                      { "tag", ARG_STR, 0, 15, NULL } };
static const cmd_arg_t A_KEY[]   = { { "key", ARG_STR, 0, 31, NULL } };
static const cmd_arg_t A_TIME[]  = { { "date", ARG_STR, 0, 10, NULL }, { "time", ARG_STR, 0, 8, NULL } };
static const cmd_arg_t A_CONF[]  = { { "confirm", ARG_ENUM, 0, 0, "CONFIRM" } };
/* Maxes mirror hg_handover_t's field capacities (ssid[33]/pass[65]/url[64] --
 * one byte reserved for the NUL in each) so a value that fits ARG_STR's bound
 * always fits the handover record; url was 96 and silently truncated. */
static const cmd_arg_t A_FWUP[]  = { { "ssid", ARG_STR, 0, 32, NULL }, { "pass", ARG_STR, 0, 64, NULL },
                                      { "url", ARG_STR, 0, 63, NULL } };

const cmd_entry_t CMD_COMMON_ROWS[] = {
  { CMDV_GET,          CMD_AREA_SYSTEM, "ID",      NULL,      NULL,   0, 0, 0, 0,                                  h_id,            NULL },
  { CMDV_GET,          CMD_AREA_SYSTEM, "VERSION", NULL,      NULL,   0, 0, 0, 0,                                  h_version,       NULL },
  { CMDV_GET,          CMD_AREA_SYSTEM, "STATUS",  NULL,      NULL,   0, 0, 0, 0,                                  h_status,        NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SESSION,"ECHO",    NULL,      A_MODE, 0, 1, 1, CMDF_SESSION,                       h_echo,          NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SESSION,"NOTIFY",  NULL,      A_NOTIFY, 0, 2, 2, CMDF_SESSION,                     h_notify,        NULL },
  { CMDV_SET,          CMD_AREA_SYSTEM, "LOG",     NULL,      A_LOG,  0, 1, 2, CMDF_SESSION,                       h_log_set,       NULL },
  { CMDV_GET,          CMD_AREA_SYSTEM, "LOG",     NULL,      NULL,   0, 0, 0, CMDF_SESSION,                       h_log_get,       NULL },
  { CMDV_BARE,         CMD_AREA_DEBUG,  "DEBUG",   "ENABLE",  A_KEY,  0, 1, 1, CMDF_SESSION,                       h_debug_enable,  NULL },
  { CMDV_BARE,         CMD_AREA_DEBUG,  "DEBUG",   "DISABLE", NULL,   0, 0, 0, 0,                                  h_debug_disable, NULL },
  { CMDV_GET,          CMD_AREA_DEBUG,  "DEBUG",   NULL,      NULL,   0, 0, 0, 0,                                  h_debug_get,     NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_TIME,   "TIME",    NULL,      A_TIME, 0, 2, 2, 0,                                  h_time,          NULL },
  { CMDV_BARE,         CMD_AREA_SYSTEM, "SAVE",    NULL,      NULL,   0, 0, 0, CMDF_SLOW,                          h_save,          "flush config to NVS" },
  { CMDV_BARE,         CMD_AREA_SYSTEM, "REBOOT",  NULL,      A_CONF, 0, 1, 1, 0,                                  h_reboot,        "restart the node" },
  { CMDV_BARE,         CMD_AREA_SYSTEM, "FACTORY", "RESET",   A_CONF, 0, 1, 1, CMDF_SLOW,                          h_factory_reset, "erase NVS and restart" },
  { CMDV_GET,          CMD_AREA_FW,     "FW",      NULL,      NULL,   0, 0, 0, 0,                                  h_fw_get,        NULL },
  { CMDV_SET,          CMD_AREA_FW,     "FW",      "ROLLBACK",A_CONF, 0, 1, 1, CMDF_UNLOCK,                        h_fw_rollback,   NULL },
  { CMDV_SET,          CMD_AREA_FW,     "FW",      "UPDATE",  A_FWUP, 0, 3, 3, CMDF_ZONE|CMDF_UNLOCK|CMDF_SLOW,    h_fw_update,     NULL },
};
const int CMD_COMMON_ROWS_N = (int)(sizeof CMD_COMMON_ROWS / sizeof CMD_COMMON_ROWS[0]);
