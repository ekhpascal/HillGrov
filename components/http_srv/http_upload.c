#include "http_srv_internal.h"

/* Task 13 replaces this: the real master/zone firmware upload tracks a
 * kind ("" | "master" | "zone") and a percent complete while POST
 * /api/fw/master or /api/fw/zone is streaming a body, for /api/state's
 * master.fw.upload_kind/upload_pct fields. Until then, no upload is ever in
 * flight. */
int http_upload_progress(const char **kind, uint8_t *pct) {
    if (kind) *kind = "";
    if (pct)  *pct = 0;
    return 0;
}
