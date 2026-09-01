#pragma once
/* Shared cmd_core fixture: command table + handlers, used by both
 * test_cmd_core.c (dispatch) and test_cmd_help.c (HELP / table-check).
 * TAG (one-word) and TAG MAC (two-word, same noun1) are deliberately kept
 * adjacent as the LAST two rows: test_cmd_core.c exercises the runtime
 * precedence between them (single-lookahead split-noun match), and
 * test_cmd_help.c's table-check test relies on them being table[TBL_N-2]
 * and table[TBL_N-1] to build a "clean" copy that excludes the pair. */
#include "cmd_core.h"

static int h_calls;

static int h_light(cmd_req_t *q, char *r, int l) {
    h_calls++;
    if (q->verb == CMDV_GET) return cmd_okf(r, l, "LIGHT %d 70 45 MANUAL 60", (int)q->val[0]);
    return cmd_okf(r, l, "LIGHT %d %d %d MANUAL %d", (int)q->val[0], (int)q->val[1], (int)q->val[2], (int)q->val[3]);
}
static int h_name(cmd_req_t *q, char *r, int l) { return cmd_okf(r, l, "NODE %d %s", (int)q->val[0], q->tok[1]); }
static int h_mac(cmd_req_t *q, char *r, int l)  { return cmd_okf(r, l, "MAC %02X%02X", q->mac[0], q->mac[5]); }
static int h_when(cmd_req_t *q, char *r, int l) { return cmd_okf(r, l, "WHEN %d", (int)q->val[0]); }
static int h_reboot(cmd_req_t *q, char *r, int l){ (void)q; return cmd_okf(r, l, "REBOOT"); }
static int h_poke(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "POKE"); }
static int h_wifi(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "WIFI"); }
static int h_echo(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "ECHO"); }
static int h_silent(cmd_req_t *q, char *r, int l){ (void)q; (void)r; (void)l; return 0; }  /* writes nothing */
static int h_tag(cmd_req_t *q, char *r, int l)  { return cmd_okf(r, l, "TAG %d %s %s", (int)q->val[0], q->tok[1], q->tok[2]); }
static int h_tag_mac(cmd_req_t *q, char *r, int l) { return cmd_okf(r, l, "TAGMAC %d %02X%02X", (int)q->val[0], q->mac[0], q->mac[5]); }

static const cmd_arg_t A_LIGHT[] = {{"shelf",ARG_INT,1,4,NULL},{"white",ARG_INT,0,100,NULL},
                                    {"red",ARG_INT,0,100,NULL},{"minutes",ARG_INT,1,720,NULL}};
static const cmd_arg_t A_NAME[]  = {{"zone",ARG_INT,1,8,NULL},{"name",ARG_STR,0,15,NULL}};
static const cmd_arg_t A_MAC[]   = {{"zone",ARG_INT,1,8,NULL},{"mac",ARG_MAC,0,0,NULL}};
static const cmd_arg_t A_WHEN[]  = {{"at",ARG_TIME,0,1439,NULL}};
static const cmd_arg_t A_CONF[]  = {{"confirm",ARG_ENUM,0,0,"CONFIRM"}};
static const cmd_arg_t A_MODE[]  = {{"mode",ARG_ENUM,0,1,"OFF|ON"}};
static const cmd_arg_t A_TAG[]   = {{"zone",ARG_INT,1,9,NULL},{"x",ARG_STR,0,15,NULL},{"mac",ARG_STR,0,15,NULL}};
static const cmd_arg_t A_TAGMAC[] = {{"zone",ARG_INT,1,8,NULL},{"mac",ARG_MAC,0,0,NULL}};

static const cmd_entry_t TBL[] = {
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF, "LIGHT", NULL, A_LIGHT, 1, 4, 4, CMDF_ACTUATOR|CMDF_ZONE, h_light, "manual override, then AUTO" },
  { CMDV_SET,          CMD_AREA_RING,  "NODE", "NAME", A_NAME, 1, 2, 2, CMDF_MASTER, h_name, NULL },
  { CMDV_SET,          CMD_AREA_RING,  "NODE", "MAC",  A_MAC,  1, 2, 2, CMDF_MASTER, h_mac,  NULL },
  { CMDV_SET,          CMD_AREA_TIME,  "WHEN", NULL,   A_WHEN, 0, 1, 1, 0, h_when, NULL },
  { CMDV_BARE,         CMD_AREA_SYSTEM,"REBOOT", NULL, A_CONF, 0, 1, 1, 0, h_reboot, "restart the node" },
  { CMDV_SET,          CMD_AREA_DEBUG, "POKE", NULL,   A_MODE, 0, 1, 1, CMDF_UNLOCK, h_poke, NULL },
  { CMDV_SET,          CMD_AREA_NET,   "WIFI", NULL,   A_MODE, 0, 1, 1, CMDF_MASTER, h_wifi, NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SESSION,"ECHO", NULL,  A_MODE, 0, 1, 1, CMDF_SESSION, h_echo, NULL },
  { CMDV_GET,          CMD_AREA_SYSTEM,"SILENT", NULL, NULL,   0, 0, 0, 0, h_silent, NULL },
  { CMDV_SET,          CMD_AREA_DEBUG, "TAG", NULL,    A_TAG, 1, 3, 3, 0, h_tag, NULL },
  { CMDV_SET,          CMD_AREA_DEBUG, "TAG", "MAC",   A_TAGMAC, 1, 2, 2, 0, h_tag_mac, NULL },
};
#define TBL_N (int)(sizeof TBL / sizeof TBL[0])
