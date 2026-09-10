#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "hg_json.h"
#include "hg_json_internal.h"

static hg_tz_check_fn g_tzck = NULL;

void hg_json_set_tz_check(hg_tz_check_fn fn) {
    g_tzck = fn;
}

static int mgroup_has_fields(int g) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        if (HG_MFIELDS[i].group == g) return 1;
    return 0;
}

int hg_json_export_mcfg(const hg_mcfg_t *m, int secrets, char *out, size_t cap) {
    cJSON *root = cJSON_CreateObject();
    for (int g = 0; g < HG_MG_COUNT; g++) {
        if (!mgroup_has_fields(g)) continue;
        cJSON *go = cJSON_CreateObject();
        for (int i = 0; i < HG_MFIELD_COUNT; i++) {
            const hg_field_t *f = &HG_MFIELDS[i];
            if (f->group != g) continue;
            if (!secrets && hg_mcfg_is_secret(f)) continue;
            char text[80];
            hg_field_read(f, m, text, sizeof text);
            cJSON_AddStringToObject(go, f->key, text);
        }
        cJSON_AddItemToObject(root, HG_MGROUP_NAMES[g], go);
    }
    return hgj_print_and_free(root, out, cap);
}

int hg_json_merge_mcfg(hg_mcfg_t *m, const char *json, char *err_path, size_t err_cap) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    if (err_path && err_cap) err_path[0] = '\0';

    hg_mcfg_t scratch = *m;
    int rc = 0;
    for (int g = 0; g < HG_MG_COUNT && rc == 0; g++) {
        cJSON *go = cJSON_GetObjectItemCaseSensitive(root, HG_MGROUP_NAMES[g]);
        if (!go || !cJSON_IsObject(go)) continue;
        for (cJSON *item = go->child; item; item = item->next) {
            const hg_field_t *f = NULL;
            for (int i = 0; i < HG_MFIELD_COUNT; i++)
                if (HG_MFIELDS[i].group == g && strcmp(HG_MFIELDS[i].key, item->string) == 0) {
                    f = &HG_MFIELDS[i];
                    break;
                }
            if (!f) continue; /* unknown key: hg_json_merge_mcfg has no warnings channel */
            char text[80];
            if (hgj_json_to_text(f, item, text, sizeof text) != 0 || hg_field_write(f, &scratch, text) != 0) {
                if (err_path && err_cap) snprintf(err_path, err_cap, "%s.%s", HG_MGROUP_NAMES[g], item->string);
                rc = -2;
                break;
            }
        }
    }

    if (rc == 0) {
        char verr[64];
        if (hg_mcfg_validate(&scratch, g_tzck, verr, sizeof verr) != 0) {
            if (err_path && err_cap) snprintf(err_path, err_cap, "%s", verr);
            rc = -2;
        }
    }

    cJSON_Delete(root);
    if (rc != 0) return rc;
    *m = scratch;
    return 0;
}
