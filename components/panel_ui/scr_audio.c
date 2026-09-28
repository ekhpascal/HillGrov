#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"

/* Spec Scope: "The Audio icon is a placeholder in this sub-project." SP7 owns
 * the player; this screen reserves its place and says so. */
static void audio_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_t *l = pnl_label(page, "Audio playback arrives with SP7 (media and storage: microSD, I2S, PCM5102A). "
                                  "Nothing to set up yet.", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_set_width(l, 760);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

const pnl_screen_ops_t PNL_SCR_AUDIO = { "Audio", audio_build, NULL, NULL, 0 };
