#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg.h"
#include "hg_mcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Schema: {"groups":[{"name":..,"scope":0|1|2,"fields":[{"key":..,"type":..,"min":..,"max":..
 *          [,"enums":[...]]},...]},...],"mgroups":[{"name":..,"fields":[{"key":..,"type":..,
 *          "max":..,"secret":bool},...]},...],"hw_readonly":true}. groups: all HG_G_COUNT
 * groups in HG_GROUP_NAMES order (scope: 0 zone, 1 shelf[0..3], 2 aux[0..1]); mgroups: the
 * HG_MGROUP_NAMES groups that have >=1 field (WEB has none, so it never appears).
 * Returns bytes written, or -1 if cap is too small. */
int hg_json_schema(char *out, size_t cap);

/* Zone config document: {"gen":N,
 *   "hw":{"HW":{...},"shelf":[{"HWSHELF":{...},"CAL":{...}}, x HG_MAX_SHELVES]},
 *   "cfg":{"ZONECFG":{...},"shelf":[{"SHELF":{},"LIGHT":{},"WATER":{},"FAN":{},"VIB":{}}, x4],
 *          "aux":[{"AUX":{}}, x HG_MAX_AUX]}}
 * "hw" mirrors the hardware plane (HW/HWSHELF/CAL groups; entirely read-only, see
 * hg_json_merge_cfg). "cfg" mirrors every other, editable group -- including AUX: its field
 * table (MODE/PULSE_S/INTERVAL_MIN/START/END) resolves via hg_field_base() to cfg->aux[idx],
 * not the hg_aux_hw_t wiring struct in hg_zone_hw_t, so it belongs under "cfg" like the
 * shelf-scoped groups, not under "hw". Values: numbers for U8/U16/PIN (PIN NONE -> raw byte
 * 255), true/false for BOOL, "HH:MM" strings for HHMM, enum NAME strings for ENUM, strings
 * for STR16. Returns bytes written, or -1 if cap is too small. */
int hg_json_export_cfg(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint32_t gen, char *out, size_t cap);

/* Merges `json` into `cfg`, applying only the "cfg" section (the ZONECFG, SHELF, LIGHT,
 * WATER, FAN, VIB, AUX field tables). Any key under "hw" is entirely read-only and instead
 * becomes a "hw.<path> readonly" warning, one per JSON leaf found there, however deep.
 * Merging runs on a scratch copy; `*cfg` is left byte-for-byte untouched unless the whole
 * merge (field writes, plus the post-merge hg_cfg_validate) succeeds. Unknown keys anywhere
 * (under "cfg" or "hw") produce warnings rather than failing the merge; `warnings` is a
 * comma-joined list of every warning found (may be "").
 * Return: 0 ok; -1 `json` doesn't parse; -2 first bad value hit while walking "cfg" --
 * `err_path` e.g. "cfg.shelf[1].WATER.TARGET"; -3 the merged result failed hg_cfg_validate --
 * `err_path` is exactly that validator's own path (e.g. "shelf[1].light.off"), not prefixed
 * with "cfg.". On -2/-3 `*cfg` is untouched (only the scratch copy was written). */
int hg_json_merge_cfg(const hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, const char *json,
                      char *err_path, size_t err_cap, char *warnings, size_t warn_cap);

/* Master config document: {"WIFI":{"STA_SSID":..,"STA_PASS":..(omitted unless `secrets`),
 * "AP_SSID":..,"AP_PASS":..(omitted unless `secrets`)},"TIME":{"TZ":..,"NTP":..},
 * "SYS":{"HOSTNAME":..}}. (WEB has no field rows, so it never appears.)
 * Returns bytes written, or -1 if cap is too small. */
int hg_json_export_mcfg(const hg_mcfg_t *m, int secrets, char *out, size_t cap);

/* Merges `json` into `m` on a scratch copy: each field write uses HG_MFIELDS/hg_field_write,
 * then the scratch copy is run through hg_mcfg_validate (using the checker set by
 * hg_json_set_tz_check; NULL, the default, accepts any TZ) before being committed to `*m`.
 * There is no warnings channel here: unknown keys (group or field) are silently ignored.
 * Return: 0 ok; -1 `json` doesn't parse; -2 a field write or the post-merge hg_mcfg_validate
 * rejected the result -- `err_path` = "GROUP.KEY" (e.g. "WIFI.AP_PASS"). On -2 `*m` is
 * untouched. */
int hg_json_merge_mcfg(hg_mcfg_t *m, const char *json, char *err_path, size_t err_cap);

/* Injects the TZ checker hg_json_merge_mcfg's post-merge hg_mcfg_validate call uses; NULL
 * (the default, in effect until this is called) accepts any TZ string. */
void hg_json_set_tz_check(hg_tz_check_fn fn);

#ifdef __cplusplus
}
#endif
