#pragma once
/* pcfg_pres.h -- the panel's presentation table (pure): label, unit, coarse step, scale and text
 * bounds per (table, group, key). A row missing here still renders with generator defaults; the
 * test_pcfg_gen coverage case fails until it gets its entry. All text is printable ASCII. */
#include <stdint.h>
#include "pcfg_gen.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t table, group; const char *key;
    const char *label, *unit;   /* ASCII */
    int32_t big_step;           /* 0 = default rule (10 when max-min > 100, else 0) */
    int16_t scale_div;          /* 0 = 1 */
    uint8_t min_len, max_len;   /* text; max_len 0 = row max */
    uint8_t keyboard;           /* pcfg_kb_t for text rows */
    uint8_t fmt_hex;            /* PCA_ADDR, PCF_ADDR, PCF_ACTLOW */
    uint8_t readonly;           /* extra override (none by default -- D11) */
    const char *zero_text;      /* display text for raw 0 (LIGHT.DLI "off"); NULL = none */
} pcfg_pres_t;
extern const pcfg_pres_t PCFG_PRES[];
extern const int         PCFG_PRES_COUNT;
const pcfg_pres_t *pcfg_pres_find(pcfg_table_t t, uint8_t group, const char *key);   /* NULL -> generator defaults */

#ifdef __cplusplus
}
#endif
