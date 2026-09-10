#include <string.h>
#include "cJSON.h"
#include "hg_json.h"
#include "hg_json_internal.h"

static const char *type_name(uint8_t t) {
    switch (t) {
    case HG_T_U8:    return "U8";
    case HG_T_U16:   return "U16";
    case HG_T_BOOL:  return "BOOL";
    case HG_T_HHMM:  return "HHMM";
    case HG_T_ENUM:  return "ENUM";
    case HG_T_STR16: return "STR16";
    case HG_T_PIN:   return "PIN";
    case HG_T_STR:   return "STR";
    default:         return "?";
    }
}

static cJSON *enum_names(const char *enums) {
    cJSON *arr = cJSON_CreateArray();
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char name[16];
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, p, n);
        name[n] = '\0';
        cJSON_AddItemToArray(arr, cJSON_CreateString(name));
        if (!bar) break;
        p = bar + 1;
    }
    return arr;
}

static cJSON *zone_field_schema(const hg_field_t *f) {
    cJSON *fo = cJSON_CreateObject();
    cJSON_AddStringToObject(fo, "key", f->key);
    cJSON_AddStringToObject(fo, "type", type_name(f->type));
    cJSON_AddNumberToObject(fo, "min", f->min);
    cJSON_AddNumberToObject(fo, "max", f->max);
    if (f->type == HG_T_ENUM && f->enums)
        cJSON_AddItemToObject(fo, "enums", enum_names(f->enums));
    return fo;
}

static int mgroup_has_fields(int g) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        if (HG_MFIELDS[i].group == g) return 1;
    return 0;
}

static cJSON *mgroup_schema(int g) {
    cJSON *go = cJSON_CreateObject();
    cJSON_AddStringToObject(go, "name", HG_MGROUP_NAMES[g]);
    cJSON *fields = cJSON_CreateArray();
    for (int i = 0; i < HG_MFIELD_COUNT; i++) {
        const hg_field_t *f = &HG_MFIELDS[i];
        if (f->group != g) continue;
        cJSON *fo = cJSON_CreateObject();
        cJSON_AddStringToObject(fo, "key", f->key);
        cJSON_AddStringToObject(fo, "type", type_name(f->type));
        cJSON_AddNumberToObject(fo, "max", f->max);
        cJSON_AddBoolToObject(fo, "secret", hg_mcfg_is_secret(f));
        cJSON_AddItemToArray(fields, fo);
    }
    cJSON_AddItemToObject(go, "fields", fields);
    return go;
}

int hg_json_schema(char *out, size_t cap) {
    cJSON *root = cJSON_CreateObject();

    cJSON *groups = cJSON_CreateArray();
    for (int g = 0; g < HG_G_COUNT; g++) {
        cJSON *go = cJSON_CreateObject();
        cJSON_AddStringToObject(go, "name", HG_GROUP_NAMES[g]);
        cJSON_AddNumberToObject(go, "scope", hg_group_scope((uint8_t)g));
        cJSON *fields = cJSON_CreateArray();
        for (int i = 0; i < HG_FIELD_COUNT; i++)
            if (HG_FIELDS[i].group == g) cJSON_AddItemToArray(fields, zone_field_schema(&HG_FIELDS[i]));
        cJSON_AddItemToObject(go, "fields", fields);
        cJSON_AddItemToArray(groups, go);
    }
    cJSON_AddItemToObject(root, "groups", groups);

    cJSON *mgroups = cJSON_CreateArray();
    for (int g = 0; g < HG_MG_COUNT; g++)
        if (mgroup_has_fields(g)) cJSON_AddItemToArray(mgroups, mgroup_schema(g));
    cJSON_AddItemToObject(root, "mgroups", mgroups);

    cJSON_AddBoolToObject(root, "hw_readonly", 1);

    return hgj_print_and_free(root, out, cap);
}
