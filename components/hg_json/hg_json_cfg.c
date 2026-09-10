#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "hg_json.h"
#include "hg_json_internal.h"

/* ---------------- export ---------------- */

static cJSON *field_to_json(const hg_field_t *f, const void *base) {
    char text[32];
    hg_field_read(f, base, text, sizeof text);
    switch (f->type) {
    case HG_T_BOOL:
        return cJSON_CreateBool(text[0] != '0');
    case HG_T_U8: case HG_T_U16: case HG_T_PIN: {
        /* PIN "NONE" (unset) -> the raw HG_NONE byte value; every other numeric type is
         * already a plain decimal string from hg_field_read. */
        long v = (f->type == HG_T_PIN && strcmp(text, "NONE") == 0) ? HG_NONE : strtol(text, NULL, 10);
        return cJSON_CreateNumber((double)v);
    }
    default: /* STR16, STR, ENUM, HHMM all render as JSON strings already in wire form */
        return cJSON_CreateString(text);
    }
}

static cJSON *export_group_obj(uint8_t group, int idx, const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg) {
    cJSON *obj = cJSON_CreateObject();
    const void *base = hg_field_base(group, idx, hw, cfg);
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        if (f->group == group) cJSON_AddItemToObject(obj, f->key, field_to_json(f, base));
    }
    return obj;
}

int hg_json_export_cfg(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint32_t gen, char *out, size_t cap) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "gen", (double)gen);

    cJSON *hwobj = cJSON_CreateObject();
    cJSON_AddItemToObject(hwobj, "HW", export_group_obj(HG_G_HW, -1, hw, cfg));
    cJSON *hwshelf = cJSON_CreateArray();
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToObject(e, "HWSHELF", export_group_obj(HG_G_HWSHELF, i, hw, cfg));
        cJSON_AddItemToObject(e, "CAL", export_group_obj(HG_G_CAL, i, hw, cfg));
        cJSON_AddItemToArray(hwshelf, e);
    }
    cJSON_AddItemToObject(hwobj, "shelf", hwshelf);
    cJSON_AddItemToObject(root, "hw", hwobj);

    cJSON *cfgobj = cJSON_CreateObject();
    cJSON_AddItemToObject(cfgobj, "ZONECFG", export_group_obj(HG_G_ZONECFG, -1, hw, cfg));
    cJSON *shelf = cJSON_CreateArray();
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToObject(e, "SHELF", export_group_obj(HG_G_SHELF, i, hw, cfg));
        cJSON_AddItemToObject(e, "LIGHT", export_group_obj(HG_G_LIGHT, i, hw, cfg));
        cJSON_AddItemToObject(e, "WATER", export_group_obj(HG_G_WATER, i, hw, cfg));
        cJSON_AddItemToObject(e, "FAN", export_group_obj(HG_G_FAN, i, hw, cfg));
        cJSON_AddItemToObject(e, "VIB", export_group_obj(HG_G_VIB, i, hw, cfg));
        cJSON_AddItemToArray(shelf, e);
    }
    cJSON_AddItemToObject(cfgobj, "shelf", shelf);
    cJSON *aux = cJSON_CreateArray();
    for (int i = 0; i < HG_MAX_AUX; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddItemToObject(e, "AUX", export_group_obj(HG_G_AUX, i, hw, cfg));
        cJSON_AddItemToArray(aux, e);
    }
    cJSON_AddItemToObject(cfgobj, "aux", aux);
    cJSON_AddItemToObject(root, "cfg", cfgobj);

    return hgj_print_and_free(root, out, cap);
}

/* ---------------- merge ---------------- */

static const hg_field_t *find_field(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && strcmp(HG_FIELDS[i].key, key) == 0) return &HG_FIELDS[i];
    return NULL;
}

/* Applies every key of `obj` to scratch's (group, shelf|aux) slot; unknown keys warn, a bad
 * value stops and formats err_path, returning -2. shelf/aux: -1 = not indexed. */
static int merge_group_fields(hg_zone_cfg_t *scratch, uint8_t group, int shelf, int aux, cJSON *obj,
                               char *err_path, size_t err_cap, char *warnings, size_t warn_cap, size_t *wpos) {
    void *base = hg_field_base(group, shelf >= 0 ? shelf : aux, NULL, scratch);
    const char *gname = HG_GROUP_NAMES[group];
    for (cJSON *item = obj->child; item; item = item->next) {
        const hg_field_t *f = find_field(group, item->string);
        if (!f) {
            char path[96];
            hgj_path(path, sizeof path, "cfg", shelf, aux, gname, item->string);
            hgj_warn(warnings, warn_cap, wpos, path);
            continue;
        }
        char text[32];
        if (hgj_json_to_text(f, item, text, sizeof text) != 0 || hg_field_write(f, base, text) != 0) {
            if (err_path && err_cap) hgj_path(err_path, err_cap, "cfg", shelf, aux, gname, item->string);
            return -2;
        }
    }
    return 0;
}

typedef struct { const char *name; uint8_t group; } named_group_t;

/* Walks a "shelf" or "aux" JSON array, dispatching each element's known group objects
 * (SHELF/LIGHT/WATER/FAN/VIB, or AUX) to merge_group_fields; anything else warns. */
static int merge_indexed_array(hg_zone_cfg_t *scratch, cJSON *arr, int max_idx, int is_aux,
                                const named_group_t *groups, int ngroups,
                                char *err_path, size_t err_cap, char *warnings, size_t warn_cap, size_t *wpos) {
    int idx = 0;
    for (cJSON *el = arr->child; el && idx < max_idx; el = el->next, idx++) {
        if (!cJSON_IsObject(el)) continue;
        int shelf = is_aux ? -1 : idx, aux = is_aux ? idx : -1;
        for (cJSON *item = el->child; item; item = item->next) {
            const named_group_t *ng = NULL;
            for (int i = 0; i < ngroups; i++)
                if (strcmp(groups[i].name, item->string) == 0) { ng = &groups[i]; break; }
            if (!ng || !cJSON_IsObject(item)) {
                char path[96];
                hgj_path(path, sizeof path, "cfg", shelf, aux, NULL, item->string);
                hgj_warn(warnings, warn_cap, wpos, path);
                continue;
            }
            int rc = merge_group_fields(scratch, ng->group, shelf, aux, item,
                                         err_path, err_cap, warnings, warn_cap, wpos);
            if (rc) return rc;
        }
    }
    return 0;
}

int hg_json_merge_cfg(const hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, const char *json,
                      char *err_path, size_t err_cap, char *warnings, size_t warn_cap) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    if (err_path && err_cap) err_path[0] = '\0';
    if (warnings && warn_cap) warnings[0] = '\0';
    size_t wpos = 0;

    /* "hw" is entirely read-only: every leaf found there is a warning, whether or not it
     * even names a real field -- so this never touches `hw` or looks anything up in it. */
    cJSON *hw_node = cJSON_GetObjectItemCaseSensitive(root, "hw");
    if (hw_node) hgj_warn_readonly_tree(hw_node, "hw", warnings, warn_cap, &wpos);

    hg_zone_cfg_t scratch = *cfg;
    int rc = 0;
    cJSON *cfg_node = cJSON_GetObjectItemCaseSensitive(root, "cfg");
    if (cfg_node && cJSON_IsObject(cfg_node)) {
        static const named_group_t shelf_groups[] = {
            { "SHELF", HG_G_SHELF }, { "LIGHT", HG_G_LIGHT }, { "WATER", HG_G_WATER },
            { "FAN", HG_G_FAN }, { "VIB", HG_G_VIB },
        };
        static const named_group_t aux_groups[] = { { "AUX", HG_G_AUX } };

        for (cJSON *item = cfg_node->child; item && rc == 0; item = item->next) {
            if (strcmp(item->string, "ZONECFG") == 0 && cJSON_IsObject(item)) {
                rc = merge_group_fields(&scratch, HG_G_ZONECFG, -1, -1, item,
                                         err_path, err_cap, warnings, warn_cap, &wpos);
            } else if (strcmp(item->string, "shelf") == 0 && cJSON_IsArray(item)) {
                rc = merge_indexed_array(&scratch, item, HG_MAX_SHELVES, 0, shelf_groups, 5,
                                          err_path, err_cap, warnings, warn_cap, &wpos);
            } else if (strcmp(item->string, "aux") == 0 && cJSON_IsArray(item)) {
                rc = merge_indexed_array(&scratch, item, HG_MAX_AUX, 1, aux_groups, 1,
                                          err_path, err_cap, warnings, warn_cap, &wpos);
            } else {
                char path[96];
                hgj_path(path, sizeof path, "cfg", -1, -1, NULL, item->string);
                hgj_warn(warnings, warn_cap, &wpos, path);
            }
        }
    }

    if (rc == 0) {
        char verr[64];
        if (hg_cfg_validate(&scratch, hw, verr, sizeof verr) != 0) {
            if (err_path && err_cap) snprintf(err_path, err_cap, "%s", verr);
            rc = -3;
        }
    }

    cJSON_Delete(root);
    if (rc != 0) return rc;
    *cfg = scratch;
    return 0;
}
