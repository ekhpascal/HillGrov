#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_mcfg.h"
#include "psvc_rc.h"
#include "psvc_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE master-config read-modify-write (spec Decision 4), over
 * components/mcfg_ops. The web's PUT /api/config?zone=0 calls it with
 * psvc_mcfg_json_fn and a 100 ms budget; the panel's Config editor calls it
 * with psvc_mcfg_fields_fn and 6000 ms. Threading: psvc_mcfg_edit() is
 * [WORKER] -- it can block for lock_ms + mcfg_commit()'s 5 s + the apply's
 * esp_hosted RPCs (P4), so never from the LVGL task nor from a TWDT-subscribed
 * task. psvc_mcfg_get() is [ANY]. */

#define PSVC_LOCK_LEGACY_MS 6000u   /* CLI rows, POST /api/wifi, POST /api/password -- today's budget */
#define PSVC_LOCK_WEB_MS     100u   /* PUT /api/config?zone=0 and GET /api/wifi/scan -- today's budget */
#define PSVC_LOCK_PANEL_MS  6000u   /* the panel worker can wait; the UI shows pending */

typedef int (*psvc_mcfg_fn)(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);   /* 0 proceed / PSVC_EDIT_* */

/* mcfg_ops_edit_ms(lock_ms): snapshot -> fn -> hg_mcfg_validate(m, tz_check,
 * err) -> commit -> apply (wifi_mgr_apply(); time_svc_apply_mcfg(); under the
 * lock, exactly what http_api_cfg.c:244-245 did). err is always written ("" on
 * success). lock -1 -> BUSY; fn 1/2/3 -> BAD_JSON / INVALID_FIELD /
 * VALIDATION with err = the field path; commit -2 -> STORAGE; commit -3 ->
 * VALIDATION with err = "". apply runs only on PSVC_OK. MCFG_F_AP_DEFAULT is
 * NOT touched (D9). what = the per-op log label, e.g. "PUT CONFIG ZONE0". */
psvc_rc_t psvc_mcfg_edit(psvc_mcfg_fn fn, void *ctx, uint32_t lock_ms, const char *what, char *err, size_t errcap);

/* ctx = const char *json. hg_json_merge_mcfg: -1 -> PSVC_EDIT_BAD_JSON,
 * -2 -> PSVC_EDIT_INVALID_FIELD (err "GROUP.KEY"), 0 -> 0. */
int  psvc_mcfg_json_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);

/* ctx = const psvc_fedits_t *. Per edit: a secret row (hg_mcfg_is_secret) with
 * empty text is skipped -- blank means unchanged, so a password cannot be
 * cleared here and an open STA is set by clearing the SSID; hg_field_write()
 * != 0 -> PSVC_EDIT_INVALID_FIELD with err "<HG_MGROUP_NAMES[group]>.<key>". */
int  psvc_mcfg_fields_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);

void psvc_mcfg_get(hg_mcfg_t *out);   /* [ANY] *out = *mcfg_get() */

#ifdef __cplusplus
}
#endif
