#include <stdio.h>
#include <string.h>
#include "cmd_core.h"
#include "ring_proto.h"
#include "master_cmds.h"

/* RING/NODES CLI rows (spec §5.4) over the injected node_ops_t -- see
 * master_cmds.h for why two ops beyond the brief's own snippet were added.
 * SET FW ZONE/ZONES/ABORT and GET FW ZONE just call through ops->fw_*;
 * Task 15 replaces master/main/cmd_table_master.c's NULL-safe -1 stubs with
 * the real fleet sequencer -- these rows don't change. */

static const node_ops_t *s_ops;
static const net_ops_t  *s_net;

void master_cmds_init(const node_ops_t *ops) { master_cmds_init2(ops, NULL); }

void master_cmds_init2(const node_ops_t *ops, const net_ops_t *net) {
    s_ops = ops;
    s_net = net;
}

/* ---- shared formatting ---- */

static const char *health_name(node_health_t h) {
    switch (h) {
    case NODE_H_ONLINE:   return "ONLINE";
    case NODE_H_DEGRADED: return "DEGRADED";
    case NODE_H_OFFLINE:  return "OFFLINE";
    case NODE_H_UPDATING: return "UPDATING";
    default:              return "EMPTY";
    }
}

static void mac_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- GET RING ---- */

static int h_get_ring(cmd_req_t *q, char *r, int l) {
    (void)q;
    ring_status_t rs;
    s_ops->ring_status(&rs);
    const char *state = rs.state == RING_ST_OPEN ? "OPEN" : rs.state == RING_ST_OK ? "OK" : "IDLE";
    cmd_okf(r, l, "RING %s SIZE %d ONLINE 0x%04x TIME %s",
            state, (int)rs.size, (unsigned)rs.online_mask, s_ops->time_valid() ? "VALID" : "NONE");
    if (rs.state == RING_ST_OPEN) cmd_linef(r, l, "  Blame : %s", rs.blame);

    /* bench-aid counters (system design §6.1's "GET RING prints it"):
     * ring_status_t itself carries none, so these sum each used node's own
     * heartbeat-reported link counters -- disclosed simplification, not
     * spelled out further by the brief's grammar line. */
    uint32_t crc_err = 0, uart_err = 0, drop = 0, fwd = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        hg_node_t nd;
        if (s_ops->get(i, &nd) != 0) continue;
        crc_err += nd.hb.rx_crc_err; uart_err += nd.hb.rx_uart_err;
        drop += nd.hb.rx_drop;       fwd += nd.hb.fwd_count;
    }
    cmd_linef(r, l, "  RxCrcErr : %lu", (unsigned long)crc_err);
    cmd_linef(r, l, "  RxUartErr : %lu", (unsigned long)uart_err);
    cmd_linef(r, l, "  RxDrop : %lu", (unsigned long)drop);
    cmd_linef(r, l, "  Fwd : %lu", (unsigned long)fwd);
    return 0;
}

/* ---- GET NODES ---- */

static int h_get_nodes(cmd_req_t *q, char *r, int l) {
    (void)q;
    cmd_okf(r, l, "NODES %d", s_ops->node_count());
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        hg_node_t nd;
        if (s_ops->get(i, &nd) != 0) continue;
        char mac[18]; mac_str(nd.mac, mac);
        cmd_linef(r, l, "  Z%d : %s %s %s fw %u.%u.%u gen %lu",
                  (int)nd.id, nd.name, mac, health_name(nd.health),
                  (unsigned)nd.hb.fw_maj, (unsigned)nd.hb.fw_min, (unsigned)nd.hb.fw_patch,
                  (unsigned long)nd.hb.cfg_gen);
    }
    return 0;
}

/* ---- GET NODE <z> ---- */

static int h_get_node(cmd_req_t *q, char *r, int l) {
    uint8_t zone = (uint8_t)q->val[0];
    hg_node_t nd;
    if (s_ops->get(zone - 1, &nd) != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    uint32_t now = q->core->now_ms();
    uint32_t age_s = (now >= nd.last_hb_ms) ? (now - nd.last_hb_ms) / 1000u : 0u;
    char mac[18]; mac_str(nd.mac, mac);

    cmd_okf(r, l, "NODE %d %s", (int)zone, nd.name);
    cmd_linef(r, l, "  MAC : %s", mac);
    cmd_linef(r, l, "  Health : %s", health_name(nd.health));
    cmd_linef(r, l, "  FW : %u.%u.%u", (unsigned)nd.hb.fw_maj, (unsigned)nd.hb.fw_min, (unsigned)nd.hb.fw_patch);
    cmd_linef(r, l, "  Gen : %lu", (unsigned long)nd.hb.cfg_gen);
    cmd_linef(r, l, "  Hops : %u", (unsigned)nd.hops);
    cmd_linef(r, l, "  LinkFlags : 0x%02x", (unsigned)nd.link_flags);
    cmd_linef(r, l, "  CmdTimeouts : %u", (unsigned)nd.cmd_timeouts);
    cmd_linef(r, l, "  LastHB : %lus", (unsigned long)age_s);
    cmd_linef(r, l, "  RxCrcErr : %u", (unsigned)nd.hb.rx_crc_err);
    cmd_linef(r, l, "  RxUartErr : %u", (unsigned)nd.hb.rx_uart_err);
    cmd_linef(r, l, "  RxDrop : %u", (unsigned)nd.hb.rx_drop);
    cmd_linef(r, l, "  Fwd : %u", (unsigned)nd.hb.fwd_count);
    cmd_linef(r, l, "  MinHeapKB : %u", (unsigned)nd.hb.min_free_heap_kb);
    cmd_linef(r, l, "  SeqDrops : %lu", (unsigned long)nd.seq_drop_tally);
    cmd_linef(r, l, "  CfgSync : %s", s_ops->cfg_sync_failed(zone) ? "FAILED" : "OK");
    return 0;
}

/* ---- GET UNASSIGNED ---- */

static int h_get_unassigned(cmd_req_t *q, char *r, int l) {
    (void)q;
    uint8_t macs[HG_MAX_ZONES][6];
    int n = s_ops->unassigned(macs, HG_MAX_ZONES);
    cmd_okf(r, l, "UNASSIGNED %d", n);
    for (int i = 0; i < n; i++) {
        char mac[18]; mac_str(macs[i], mac);
        cmd_linef(r, l, "  %s", mac);
    }
    return 0;
}

/* ---- SET NODE <z> NAME <name> (split two-word noun, key at noun0+2) ---- */

static int h_set_node_name(cmd_req_t *q, char *r, int l) {
    uint8_t zone = (uint8_t)q->val[0];
    if (s_ops->set_name(zone, q->tok[1]) != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    return cmd_okf(r, l, "NODE %d NAME %s", (int)zone, q->tok[1]);
}

/* ---- CLEAR NODE <z> CONFIRM ---- */

static int h_clear_node(cmd_req_t *q, char *r, int l) {
    uint8_t zone = (uint8_t)q->val[0];
    if (s_ops->clear(zone) != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    return cmd_okf(r, l, "NODE %d CLEARED", (int)zone);
}

/* ---- SET RING TRACE <OFF|ON> ---- */

static int h_ring_trace(cmd_req_t *q, char *r, int l) {
    s_ops->trace((int)q->val[0]);
    return cmd_okf(r, l, "RING TRACE %s", q->val[0] ? "ON" : "OFF");
}

/* ---- fleet OTA shell (Task 15 fills ops->fw_*) ---- */

static int h_get_fw_zone(cmd_req_t *q, char *r, int l) {
    (void)q;
    char buf[64] = "";
    s_ops->fw_status(buf, sizeof buf);
    return cmd_okf(r, l, "FW ZONE %s", buf);
}

/* spec §5.3: "OK QUEUED" + NOTIFY style is SET FW ZONE's own contract (the
 * fleet sequencer genuinely outlives the request); mirrored here for
 * SET FW ZONES CONFIRM since it's the same fire-and-track shape, just
 * fleet-wide. Fix round ruling on ops rc: -2 (a sequence is already
 * running) -> ERR FW_BUSY; -1 (bad zone / no assigned zones) -> ERR
 * ZONE_UNKNOWN; 0 -> accepted. */
static int h_set_fw_zone(cmd_req_t *q, char *r, int l) {
    int rc = s_ops->fw_zone((uint8_t)q->val[0]);
    if (rc == -2) return cmd_err(r, l, "FW_BUSY");
    if (rc != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    return cmd_okf(r, l, "QUEUED");
}

static int h_set_fw_zones(cmd_req_t *q, char *r, int l) {
    (void)q;
    int rc = s_ops->fw_all();
    if (rc == -2) return cmd_err(r, l, "FW_BUSY");
    if (rc != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    return cmd_okf(r, l, "QUEUED");
}

/* fw_abort's only failure is "nothing running" -> ERR NOT_READY (fix
 * round ruling). */
static int h_set_fw_abort(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (s_ops->fw_abort() != 0) return cmd_err(r, l, "NOT_READY");
    return cmd_okf(r, l, "FW ABORT");
}

/* ---- NET/TIME rows (Task 8) over net_ops_t ----
 *
 * Every handler here starts with the same NULL guard: master_cmds_init()
 * (the SP3 entry point) installs no net ops, and a board that took that
 * path must answer ERR INTERNAL rather than fault in a CLI task. */
#define NEED_NET() do { if (!s_net) return cmd_err(r, l, "INTERNAL"); } while (0)

/* The CLI tokenizer splits on whitespace and has no way to spell an empty
 * token, so "-" is the documented stand-in for "nothing here" in both
 * credential slots. It matters on both: "SET WIFI STA <ssid> -" is an open
 * network, and "SET WIFI STA - -" is the only way to UNconfigure the STA
 * again and stop the master reaching for a house Wi-Fi that has gone away.
 * On the AP row the same "-" is accepted syntactically and then fails
 * hg_mcfg_validate (ap_ssid 1..32, ap_pass 8..63) as ERR INVALID -- the
 * right answer: WPA2 has no passwordless mode and the AP is never optional.
 *
 * Disclosed consequence rather than a worked-around one: an SSID or password
 * that is literally "-" cannot be set from the CLI. The web UI (Task 12)
 * can, since it passes JSON strings through untouched. */
static const char *dash_is_empty(const char *s) {
    return (s[0] == '-' && s[1] == '\0') ? "" : s;
}

/* A field that is empty prints as "-" so every GET WIFI line has a value and
 * the column layout never collapses. */
static const char *or_dash(const char *s) { return s[0] ? s : "-"; }

/* Shared rc mapping for the net ops that persist something: -2 is "the value
 * was fine, storing it failed" (NVS, a commit-mutex timeout, an unavailable
 * hash), which an operator must not answer by retyping the value -- hence its
 * own token rather than ERR INVALID. */
static int net_rc_err(char *r, int l, int rc) {
    return cmd_err(r, l, rc == -2 ? "STORAGE" : "INVALID");
}

/* ---- GET WIFI ---- */

static int h_get_wifi(cmd_req_t *q, char *r, int l) {
    (void)q;
    NEED_NET();
    wifi_status_t w;
    memset(&w, 0, sizeof w);
    s_net->wifi_status(&w);
    cmd_okf(r, l, "WIFI STA %s %s AP %s %u",
            w.sta_up ? "UP" : "DOWN", or_dash(w.sta_ip),
            or_dash(w.ap_ssid), (unsigned)w.ap_clients);
    cmd_linef(r, l, "  StaSsid : %s", or_dash(w.sta_ssid));
    cmd_linef(r, l, "  StaReason : %s", or_dash(w.sta_reason));
    /* RSSI only means something while associated; a bare "0" while down
     * reads like a perfect signal, so it prints as "-" instead. */
    if (w.sta_up) cmd_linef(r, l, "  Rssi : %d", (int)w.rssi);
    else          cmd_linef(r, l, "  Rssi : -");
    return 0;
}

/* ---- SET WIFI STA <ssid> <pass> / SET WIFI AP <ssid> <pass> ---- */

static int h_set_wifi_sta(cmd_req_t *q, char *r, int l) {
    NEED_NET();
    int rc = s_net->set_sta(dash_is_empty(q->tok[0]), dash_is_empty(q->tok[1]));
    if (rc != 0) return net_rc_err(r, l, rc);
    /* Echoes the token as typed, so clearing the STA reads back as
     * "OK WIFI STA -". */
    return cmd_okf(r, l, "WIFI STA %s", q->tok[0]);
}

static int h_set_wifi_ap(cmd_req_t *q, char *r, int l) {
    NEED_NET();
    int rc = s_net->set_ap(dash_is_empty(q->tok[0]), dash_is_empty(q->tok[1]));
    if (rc != 0) return net_rc_err(r, l, rc);
    return cmd_okf(r, l, "WIFI AP %s", q->tok[0]);
}

/* ---- SET WEB PASSWORD <pw> ----
 * The new password is deliberately NOT echoed back. */

static int h_set_web_password(cmd_req_t *q, char *r, int l) {
    NEED_NET();
    int rc = s_net->set_web_password(q->tok[0]);
    if (rc != 0) return net_rc_err(r, l, rc);
    return cmd_okf(r, l, "WEB PASSWORD");
}

/* ---- GET TZ / SET TZ <posix> ----
 * SET validates through mcfg_commit's installed tz_check (time_core's
 * tz_parse), so an unparseable POSIX string comes back as -1 -> ERR INVALID. */

static int h_tz(cmd_req_t *q, char *r, int l) {
    NEED_NET();
    if (q->verb == CMDV_GET) {
        hg_mcfg_t m;
        s_net->get_mcfg(&m);
        return cmd_okf(r, l, "TZ %s", m.tz);
    }
    int rc = s_net->set_tz(q->tok[0]);
    if (rc != 0) return net_rc_err(r, l, rc);
    return cmd_okf(r, l, "TZ %s", q->tok[0]);
}

/* ---- SET NODE <z> MAC <mac> (split two-word noun, same shape as NAME) ----
 * Pre-seeds a zone id -> MAC binding so a replacement board is adopted into
 * the right zone the moment it enrols, instead of landing in GET UNASSIGNED.
 * ops rc -1 is an unknown/unusable zone, reported exactly like SET NODE <z>
 * NAME's own failure. */

static int h_set_node_mac(cmd_req_t *q, char *r, int l) {
    NEED_NET();
    uint8_t zone = (uint8_t)q->val[0];
    if (s_net->seed_mac(zone, q->mac) != 0) return cmd_err(r, l, "ZONE_UNKNOWN");
    char mac[18]; mac_str(q->mac, mac);
    return cmd_okf(r, l, "NODE %d MAC %s", (int)zone, mac);
}

/* ---- row table ---- */

static const cmd_arg_t A_ZONE[]       = { { "zone", ARG_INT, 1, HG_MAX_ZONES, NULL } };
static const cmd_arg_t A_NODE_NAME[]  = { { "zone", ARG_INT, 1, HG_MAX_ZONES, NULL }, { "name", ARG_STR, 0, 15, NULL } };
static const cmd_arg_t A_CLEAR_NODE[] = { { "zone", ARG_INT, 1, HG_MAX_ZONES, NULL }, { "confirm", ARG_ENUM, 0, 0, "CONFIRM" } };
static const cmd_arg_t A_TRACE[]      = { { "mode", ARG_ENUM, 0, 1, "OFF|ON" } };
static const cmd_arg_t A_CONF[]       = { { "confirm", ARG_ENUM, 0, 0, "CONFIRM" } };
/* Maxima mirror hg_mcfg_validate exactly (ssid 32, pass 63, tz 47, web
 * password 63), so an over-long value is rejected as ERR BAD_ARGS by the
 * table before any commit is attempted. */
static const cmd_arg_t A_WIFI_CRED[] = { { "ssid", ARG_STR, 0, 32, NULL }, { "pass", ARG_STR, 0, 63, NULL } };
static const cmd_arg_t A_WEB_PW[]    = { { "password", ARG_STR, 0, 63, NULL } };
static const cmd_arg_t A_TZ[]        = { { "posix", ARG_STR, 0, 47, NULL } };
static const cmd_arg_t A_NODE_MAC[]  = { { "zone", ARG_INT, 1, HG_MAX_ZONES, NULL }, { "mac", ARG_MAC, 0, 0, NULL } };

const cmd_entry_t MASTER_CMD_ROWS[] = {
  { CMDV_GET,  CMD_AREA_RING, "RING",       NULL,   NULL,         0, 0, 0, CMDF_MASTER, h_get_ring,       NULL },
  { CMDV_GET,  CMD_AREA_RING, "NODES",      NULL,   NULL,         0, 0, 0, CMDF_MASTER, h_get_nodes,      NULL },
  { CMDV_GET,  CMD_AREA_RING, "NODE",       NULL,   A_ZONE,       1, 1, 1, CMDF_MASTER, h_get_node,       NULL },
  { CMDV_GET,  CMD_AREA_RING, "UNASSIGNED", NULL,   NULL,         0, 0, 0, CMDF_MASTER, h_get_unassigned, NULL },
  { CMDV_SET,  CMD_AREA_RING, "NODE",  "NAME",      A_NODE_NAME,  1, 2, 2, CMDF_MASTER, h_set_node_name,  NULL },
  { CMDV_BARE, CMD_AREA_RING, "CLEAR", "NODE",      A_CLEAR_NODE, 0, 2, 2, CMDF_MASTER, h_clear_node,     "retire a node id" },
  { CMDV_SET,  CMD_AREA_RING, "RING",  "TRACE",     A_TRACE,      0, 1, 1, CMDF_MASTER, h_ring_trace,     NULL },
  { CMDV_GET,  CMD_AREA_FW,   "FW",    "ZONE",      NULL,         0, 0, 0, CMDF_MASTER, h_get_fw_zone,    NULL },
  { CMDV_SET,  CMD_AREA_FW,   "FW",    "ZONE",      A_ZONE,       0, 1, 1, CMDF_MASTER, h_set_fw_zone,    NULL },
  { CMDV_SET,  CMD_AREA_FW,   "FW",    "ZONES",     A_CONF,       0, 1, 1, CMDF_MASTER, h_set_fw_zones,   "fleet update, all zones" },
  { CMDV_SET,  CMD_AREA_FW,   "FW",    "ABORT",     NULL,         0, 0, 0, CMDF_MASTER, h_set_fw_abort,   NULL },
  /* GET WIFI is GET-only and SET WIFI STA/AP are SET-only on purpose:
   * cmd_table_check rejects a one-word row that shares noun1 AND a verb bit
   * with a two-word row once the one-word row can take 2 args, because the
   * two-word noun would then be ambiguous with its first argument. Splitting
   * the verbs keeps "GET WIFI" and "SET WIFI STA ..." unambiguous. */
  { CMDV_GET,  CMD_AREA_NET,  "WIFI",  NULL,        NULL,         0, 0, 0, CMDF_MASTER, h_get_wifi,       NULL },
  { CMDV_SET,  CMD_AREA_NET,  "WIFI",  "STA",       A_WIFI_CRED,  0, 2, 2, CMDF_MASTER, h_set_wifi_sta,   "join house wifi; pass - = open" },
  { CMDV_SET,  CMD_AREA_NET,  "WIFI",  "AP",        A_WIFI_CRED,  0, 2, 2, CMDF_MASTER, h_set_wifi_ap,    NULL },
  { CMDV_SET,  CMD_AREA_NET,  "WEB",   "PASSWORD",  A_WEB_PW,     0, 1, 1, CMDF_MASTER, h_set_web_password, "web UI login, 8..63 chars" },
  { CMDV_SET|CMDV_GET, CMD_AREA_TIME, "TZ", NULL,   A_TZ,         0, 1, 1, CMDF_MASTER, h_tz,             NULL },
  { CMDV_SET,  CMD_AREA_RING, "NODE",  "MAC",       A_NODE_MAC,   1, 2, 2, CMDF_MASTER, h_set_node_mac,   "pre-seed a zone id to a MAC" },
};
const int MASTER_CMD_ROWS_N = (int)(sizeof MASTER_CMD_ROWS / sizeof MASTER_CMD_ROWS[0]);
