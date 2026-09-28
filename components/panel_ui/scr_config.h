#pragma once
/* scr_config.h -- the Config destination and the generated-editor frame both editors share (glue; LVGL task). */
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "hg_cfg.h"
#include "pcfg_gen.h"
#include "pcfg_edit.h"
#include "pnl_poll.h"
#include "wdg_field.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef const char *(*cfg_value_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap);
typedef struct { pcfg_table_t table; uint8_t zone; cfg_value_fn value; void *ctx;
                 const hg_zone_hw_t *hw_or_null; pcfg_edits_t *edits;
                 wdg_reveal_fn reveal; } cfg_view_t;          /* reveal: SECRET rows only; NULL for zones */
int  cfg_render_group(lv_obj_t *list, const cfg_view_t *v, uint8_t group, int idx);  /* clears list, one wdg_field per row */
void cfg_show_error(const cfg_view_t *v, const char *path, const char *code);
     /* pcfg_locate -> switch tab/index -> wdg_field_set_error; no row -> banner "<code>: <path>".
        A scoped group located with idx -1 (no "shelf[N]." / "aux[N]." in the path) is never used as an index:
        the tab switches, no row is outlined, and the banner says the index was not reported. While the keyboard is
        open nothing re-renders: the refusal is the banner alone. */
void cfg_set_status(const char *text, int is_error);
void cfg_set_title(const char *text);
typedef void (*cfg_save_fn)(void);
lv_obj_t *cfg_frame_build(lv_obj_t *body, const cfg_view_t *v, cfg_save_fn on_save);
     /* tabs (the table's groups that have rows, in table order; ten zone tabs in two rows), shelf/aux index selector,
        Save, row list; renders the current tab; keeps tab + index across rebuilds of the same (table, zone) */
void cfg_frame_rerender(void);           /* re-render the current tab/index from the view (after a reload or a save) */
void cfg_frame_set_saving(int saving);   /* Save -> "Saving..." + disabled / back to "Save" */

/* ADDITION (controller ruling C14): the save state both editors share, and its ONE reconciliation. A save job's
 * done() may land after the screen was rebuilt (THE RULE: it must not touch the new widgets), so the outcome is kept
 * here and the Save button is brought back in step by cfg_save_sync. */
#define CFG_SAVE_MSG_MAX 160
typedef struct { uint8_t saving, ui_saving, kept_err; char kept[CFG_SAVE_MSG_MAX]; } cfg_save_t;
void cfg_save_begin(cfg_save_t *s);   /* the job was queued: Save -> "Saving...", status "Saving..." */
void cfg_save_end(cfg_save_t *s, int live, const char *msg, int is_err);
     /* in done(): live -> Save + status now; stale -> msg kept for the next frame (touches no widget) */
void cfg_save_sync(cfg_save_t *s, int frame_rebuilt);
     /* after every frame build (frame_rebuilt 1) and on every update (0): the Save button follows s->saving, and a
        kept outcome is shown once the save is over */
void cfg_save_forget(cfg_save_t *s);  /* drop a kept outcome (idle wipe) */

void cfg_zone_open(lv_obj_t *body, uint8_t zone);
void cfg_zone_close(void);
void cfg_zone_update(const pnl_snap_t *s);
void cfg_zone_wipe_all(void);   /* every zone's edit set + loaded doc (Task 27 calls it) */
void cfg_master_open(lv_obj_t *body);          /* Task 21 */
void cfg_master_close(void);                   /* Task 21 */
void cfg_master_update(const pnl_snap_t *s);   /* Task 21 */
void cfg_master_wipe(void);                    /* Task 21 */

#ifdef __cplusplus
}
#endif
