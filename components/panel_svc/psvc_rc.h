#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE refusal vocabulary both faces speak (spec "Error handling": a
 * refusal must mean the same thing on the web and on the panel). Each value's
 * token is exactly the error code the web already sends -- http_srv_error()
 * codes and CLI ERR tokens -- so psvc_rc_token() is also what the web maps
 * back onto HTTP. Pure: no IDF headers. Append new values before
 * PSVC_RC_COUNT and add the token in psvc_rc.c (a static assert ties the two). */
typedef enum {
    PSVC_OK = 0,
    PSVC_E_BUSY, PSVC_E_INVALID, PSVC_E_BAD_JSON, PSVC_E_INVALID_FIELD, PSVC_E_VALIDATION,
    PSVC_E_NO_CACHE, PSVC_E_ZONE_UNKNOWN, PSVC_E_ZONE_NOT_ONLINE, PSVC_E_STORAGE, PSVC_E_INTERNAL,
    PSVC_E_FLEET_BUSY, PSVC_E_FLEET_REJECTED, PSVC_E_NOT_ACTIVE,
    PSVC_E_UPLOAD_ACTIVE, PSVC_E_FLEET_ACTIVE, PSVC_E_TRIAL_PENDING, PSVC_E_NO_SLOT, PSVC_E_LOW_HEAP,
    PSVC_E_TOO_LARGE, PSVC_E_IMAGE_MISMATCH, PSVC_E_WRITE_FAILED, PSVC_E_STALLED, PSVC_E_RECV_FAILED,
    PSVC_E_ZONE_FW_BUSY,
    PSVC_RC_COUNT
} psvc_rc_t;

/* "OK", "BUSY", ... "ZONE_FW_BUSY": the enum name minus "PSVC_E_" ("OK" for
 * PSVC_OK). "INTERNAL" for anything outside 0..PSVC_RC_COUNT-1. */
const char *psvc_rc_token(psvc_rc_t rc);

/* The net_ops_t convention the CLI rows and POST /api/wifi answer with
 * (master_cmds.h:46-60): OK 0; INVALID/VALIDATION/INVALID_FIELD/BAD_JSON/
 * ZONE_UNKNOWN -1 ("the value is wrong"); BUSY/STORAGE -2 ("could not be
 * stored, retry" -- lock contention stays -2 there, D10); anything else -3. */
int psvc_rc_to_net_legacy(psvc_rc_t rc);

/* The node_mgr fleet sequencer's return codes (node_mgr_fw_zone/all/abort) in this vocabulary, the rule
 * http_fleet.c has always answered with, now shared so both faces say the same thing. */
psvc_rc_t psvc_rc_from_fleet(int rc, int is_abort);   /* 0 OK; abort: -1 NOT_ACTIVE; start: -2 FLEET_BUSY, -1 FLEET_REJECTED
                                                          (every other non-zero start rc is FLEET_REJECTED, every other
                                                          non-zero abort rc NOT_ACTIVE -- http_fleet.c's rule) */

#ifdef __cplusplus
}
#endif
