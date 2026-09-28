#include "psvc_fw.h"

static const char *volatile s_kind = "";
static volatile uint32_t    s_pct;

void psvc_fw_progress_set(const char *kind, uint32_t pct) {
    s_pct  = pct;
    s_kind = kind ? kind : "";   /* published last: a reader never sees a kind without a pct */
}

int psvc_fw_progress(const char **kind, uint8_t *pct) {
    const char *k = s_kind;
    uint32_t    p = s_pct;
    if (kind) *kind = k ? k : "";
    if (pct)  *pct  = (uint8_t)(p > 100 ? 100 : p);
    return (k && *k) ? 1 : 0;
}
