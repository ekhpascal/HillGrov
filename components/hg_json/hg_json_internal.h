#pragma once
/* Shared helpers for hg_json_schema.c / hg_json_cfg.c / hg_json_mcfg.c. `static inline` so
 * each translation unit that doesn't need a given helper (e.g. hg_json_schema.c needs none
 * of these; hg_json_mcfg.c never uses the warnings/readonly-tree pair) doesn't trip
 * -Wunused-function -- only non-inline `static` functions get that warning. */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "hg_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "<root>[.shelf[N]|.aux[N]][.group][.key]" -- shelf/aux: -1 = not indexed, at most one of
 * the two may be >= 0. group/key: NULL to omit. Used both for merge's own -2 bad-value path
 * and for "cfg.<...>" unknown-key warnings (hg_cfg_validate's own -3 path is untouched by
 * this -- it comes back verbatim from hg_cfg_validate/hg_mcfg_validate). */
static inline void hgj_path(char *out, size_t cap, const char *root, int shelf, int aux,
                             const char *group, const char *key) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "%s", root);
    if (n < 0) n = 0;
    if (shelf >= 0) n += snprintf(buf + n, sizeof buf - (size_t)n, ".shelf[%d]", shelf);
    else if (aux >= 0) n += snprintf(buf + n, sizeof buf - (size_t)n, ".aux[%d]", aux);
    if (group) n += snprintf(buf + n, sizeof buf - (size_t)n, ".%s", group);
    if (key)   snprintf(buf + n, sizeof buf - (size_t)n, ".%s", key);
    snprintf(out, cap, "%s", buf);
}

/* Comma-joined warning accumulator; *pos tracks the write position across calls. Silently
 * drops a warning that doesn't fit rather than truncating one mid-string. */
static inline void hgj_warn(char *warnings, size_t warn_cap, size_t *pos, const char *text) {
    if (!warnings || warn_cap == 0) return;
    size_t tl = strlen(text);
    size_t need = tl + (*pos > 0 ? 1 : 0);
    if (*pos + need >= warn_cap) return;
    if (*pos > 0) warnings[(*pos)++] = ',';
    memcpy(warnings + *pos, text, tl);
    *pos += tl;
    warnings[*pos] = '\0';
}

/* Every leaf found under `node` becomes one "<prefix...> readonly" warning; `prefix` is the
 * path already built for `node` itself (e.g. "hw"). Used for the entire "hw" branch of a
 * merge document, which is read-only regardless of whether a given key is even a real field. */
static inline void hgj_warn_readonly_tree(const cJSON *node, const char *prefix,
                                           char *warnings, size_t warn_cap, size_t *pos) {
    if (cJSON_IsObject(node)) {
        for (cJSON *c = node->child; c; c = c->next) {
            char np[96];
            snprintf(np, sizeof np, "%s.%s", prefix, c->string);
            hgj_warn_readonly_tree(c, np, warnings, warn_cap, pos);
        }
    } else if (cJSON_IsArray(node)) {
        int i = 0;
        for (cJSON *c = node->child; c; c = c->next, i++) {
            char np[96];
            snprintf(np, sizeof np, "%s[%d]", prefix, i);
            hgj_warn_readonly_tree(c, np, warnings, warn_cap, pos);
        }
    } else {
        char msg[112];
        snprintf(msg, sizeof msg, "%s readonly", prefix);
        hgj_warn(warnings, warn_cap, pos, msg);
    }
}

/* JSON leaf -> the CLI text form hg_field_write expects, per f->type. Returns -1 (leaving
 * `buf` untouched) if the JSON value's kind doesn't match what f->type requires -- numbers
 * for U8/U16/PIN, true/false for BOOL, strings for HHMM/ENUM/STR16/STR (a number given for
 * an ENUM is rejected here, same as a string given for a U8/U16). */
static inline int hgj_json_to_text(const hg_field_t *f, const cJSON *item, char *buf, size_t cap) {
    switch (f->type) {
    case HG_T_STR16: case HG_T_STR: case HG_T_ENUM: case HG_T_HHMM:
        if (!cJSON_IsString(item)) return -1;
        snprintf(buf, cap, "%s", item->valuestring);
        return 0;
    case HG_T_BOOL:
        if (!cJSON_IsBool(item)) return -1;
        snprintf(buf, cap, "%d", cJSON_IsTrue(item) ? 1 : 0);
        return 0;
    case HG_T_U8: case HG_T_U16: case HG_T_PIN:
        if (!cJSON_IsNumber(item)) return -1;
        snprintf(buf, cap, "%d", (int)item->valuedouble);
        return 0;
    default:
        return -1;
    }
}

/* Prints `root` compactly into `out`/`cap` and always frees it. Returns the byte count
 * (strlen of what was printed) or -1 if `cap` was too small. */
static inline int hgj_print_and_free(cJSON *root, char *out, size_t cap) {
    cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)cap, 0);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}

#ifdef __cplusplus
}
#endif
