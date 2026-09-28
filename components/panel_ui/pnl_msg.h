#pragma once
/* pnl_msg.h -- refusal and outcome sentences (pure; printable ASCII only). One refusal vocabulary
 * (psvc_rc_t) -> one sentence per context, so a refusal means the same thing in both faces. */
#include <stddef.h>
#include <stdint.h>
#include "psvc_rc.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PNL_CTX_ZONE_LOAD = 0, PNL_CTX_ZONE_SAVE, PNL_CTX_MCFG_SAVE, PNL_CTX_WIFI_JOIN, PNL_CTX_WIFI_AP, PNL_CTX_SCAN,
               PNL_CTX_TZ, PNL_CTX_PASSWORD, PNL_CTX_FLEET_ZONE, PNL_CTX_FLEET_ALL, PNL_CTX_FLEET_ABORT, PNL_CTX_FW_MASTER,
               PNL_CTX_FW_ZONE, PNL_CTX_COUNT } pnl_msg_ctx_t;
typedef struct { int zone; const char *version, *slot; uint32_t len; } pnl_msg_arg_t;
/* strlen(out) (> 0) / -1 (out NULL or cap 0); clipped to cap; bytes outside 0x20..0x7E become '?' */
int pnl_msg(pnl_msg_ctx_t ctx, psvc_rc_t rc, const pnl_msg_arg_t *arg /* NULL ok */, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
