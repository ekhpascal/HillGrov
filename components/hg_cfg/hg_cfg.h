#pragma once
#include "hg_cfg_types.h"
#ifdef __cplusplus
extern "C" {
#endif

void hg_defaults_hw(hg_zone_hw_t *hw);
void hg_defaults_cfg(hg_zone_cfg_t *cfg);

typedef enum { HG_G_ZONECFG = 0, HG_G_SHELF, HG_G_LIGHT, HG_G_WATER, HG_G_FAN, HG_G_VIB,
               HG_G_AUX, HG_G_HW, HG_G_HWSHELF, HG_G_CAL, HG_G_COUNT } hg_group_t;
typedef enum { HG_T_U8, HG_T_U16, HG_T_BOOL, HG_T_HHMM, HG_T_ENUM, HG_T_STR16, HG_T_PIN, HG_T_STR } hg_ftype_t;
/* HG_T_STR: NUL-terminated text, any byte; f->max = capacity - 1 (in-struct buffer is max+1 bytes) */
typedef struct { uint8_t group; const char *key; uint16_t offset; uint8_t type;
                 int32_t min, max; const char *enums; } hg_field_t;
extern const hg_field_t HG_FIELDS[];
extern const int        HG_FIELD_COUNT;
extern const char *const HG_GROUP_NAMES[HG_G_COUNT];   /* "ZONECFG".."CAL" */
int  hg_group_find(const char *name);                  /* case-insensitive; -1 unknown */
int  hg_group_scope(uint8_t group);                    /* 0 zone-scoped, 1 shelf-scoped (idx 0..3), 2 aux-scoped (idx 0..1) */
/* rc: 0 OK, -1 BAD_ARGS, -2 OUT_OF_RANGE, -3 INVALID_FIELD (unknown key), -4 bad index */
int  hg_field_set_text(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, uint8_t group, int idx, const char *key, const char *val);
int  hg_field_get_text(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint8_t group, int idx, const char *key, char *out, size_t cap);
/* Generic field access: base points at the struct the field row's offset is relative to
 * (any struct, not just hg_zone_hw_t/hg_zone_cfg_t) -- callers resolve group -> base pointer
 * themselves. rc: 0 OK, -1 BAD_ARGS, -2 OUT_OF_RANGE. hg_field_set_text/get_text are
 * hg_group_find()+base-resolution wrappers around these. */
int  hg_field_write(const hg_field_t *f, void *base, const char *text);
int  hg_field_read(const hg_field_t *f, const void *base, char *out, size_t cap);
/* Same group -> base resolution hg_field_set_text/get_text use internally, exposed for
 * callers (hg_json) that walk HG_FIELDS directly instead of going through the text-based
 * key wrappers. idx is ignored for zone-scoped groups (scope 0). NULL for an out-of-range
 * shelf/aux idx. The returned pointer is never actually const even though hw/cfg are taken
 * as const here (mirrors hg_field_read's const base) -- callers writing through it (merge,
 * via hg_field_write) pass their own non-const cfg/scratch object. */
void *hg_field_base(uint8_t group, int idx, const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg);
int  hg_hhmm_parse(const char *s);                     /* "HH:MM" or integer minutes; 0..1439, -1 bad */
void hg_hhmm_format(int minutes, char out[6]);
int  hg_hw_validate(const hg_zone_hw_t *hw, char *err, size_t errlen);                       /* 0 ok / -1, err = field path */
int  hg_cfg_validate(const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
