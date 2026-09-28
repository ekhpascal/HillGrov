#pragma once
/* wdg_keyboard.h -- the one modal keyboard / keypad (glue; LVGL task only). Filters every key with
 * pnl_kb_accepts; OK stays disabled until pnl_text_ok; masked = password mode with an eye toggle that
 * re-masks by itself after 10 s and whenever the overlay closes (Global Constraints, "Secrets"). */
#include <stdint.h>
#include "pcfg_gen.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wdg_kb_done_fn)(void *ctx, int accepted, const char *text);   /* text valid during the call only */
void wdg_keyboard_open(const char *title, pcfg_kb_t kb, const char *initial, uint8_t min_len, uint8_t max_len,
                       int masked, wdg_kb_done_fn done, void *ctx);  /* modal on lv_layer_top(); an open keyboard is
                                                                        closed (without done) first */
void wdg_keyboard_close(void);   /* wipe + close WITHOUT calling done (teardown, idle wipe, row deletion) */
int  wdg_keyboard_is_open(void);

#ifdef __cplusplus
}
#endif
