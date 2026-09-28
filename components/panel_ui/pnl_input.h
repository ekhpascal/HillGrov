#pragma once
/* pnl_input.h -- keyboard character classes, text bounds and the MAC rule (pure). */
#include <stddef.h>
#include <stdint.h>
#include "pcfg_gen.h"
#include "pnl_zero.h"   /* re-exports pnl_zero(): the panel's one secret wipe (volatile stores, never removed as dead
                           stores at -Os). Defined once, in pnl_zero.h (Task 8); every panel_ui file reaches it here. */
#ifdef __cplusplus
extern "C" {
#endif

int pnl_kb_accepts(pcfg_kb_t kb, char c);   /* NUMERIC 0-9; TEXT 0x20..0x7E; TEXT_NOSPACE 0x21..0x7E; HOSTNAME a-z 0-9 '-';
                                               HEX 0-9 a-f A-F ':'; NONE nothing */
int pnl_text_ok(pcfg_kb_t kb, const char *s, uint8_t min_len, uint8_t max_len);   /* 1 / 0 */
int pnl_mac_parse(const char *s, uint8_t mac[6]);   /* app.js:1688 ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ -- 0 / -1 (mac untouched) */

#ifdef __cplusplus
}
#endif
