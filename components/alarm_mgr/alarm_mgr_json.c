#include <string.h>
#include "cJSON.h"
#include "alarm_mgr.h"
#include "alarm_mgr_internal.h"

int alarm_mgr_json(char *out, size_t cap) {
    cJSON *root = cJSON_CreateObject();

    cJSON *active = cJSON_CreateArray();
    for (int i = 0; i < AM_ACTIVE_MAX; i++) {
        if (!am_active[i].used) continue;
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "key", am_active[i].key);
        cJSON_AddStringToObject(a, "text", am_active[i].text);
        cJSON_AddNumberToObject(a, "since_s", (double)am_active[i].since_s);
        cJSON_AddItemToArray(active, a);
    }
    cJSON_AddItemToObject(root, "active", active);

    cJSON *events = cJSON_CreateArray();
    uint32_t kept = am_total < AM_EVENTS ? am_total : AM_EVENTS;
    for (uint32_t i = 0; i < kept; i++) {
        uint32_t idx = (am_total - 1 - i) % AM_EVENTS;   /* newest first */
        const am_event_t *ev = &am_ring[idx];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "at_s", (double)ev->at_s);
        cJSON_AddStringToObject(e, "text", ev->text);
        cJSON_AddItemToArray(events, e);
    }
    cJSON_AddItemToObject(root, "events", events);

    cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)cap, 0);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}
