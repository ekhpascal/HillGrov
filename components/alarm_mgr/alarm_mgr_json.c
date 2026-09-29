#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "alarm_mgr.h"
#include "alarm_mgr_internal.h"

int alarm_mgr_json(char *out, size_t cap) {
    /* One entry at a time: each am_copy_*() takes the lock for one ~92 B copy into a stack local, and the entry is
     * formatted after the lock is released, so the lock is never held across cJSON's allocations or its formatting,
     * and no whole am_snapshot_t (~6.6 KB of internal heap on the ESP32) is needed. The events are indexed from ONE
     * am_total read: they are listed newest first as of that read, and a burst of new events while this runs can
     * only cut the tail short (an event overwritten meanwhile ends the list); it never shifts or repeats an entry. */
    cJSON *root = cJSON_CreateObject();

    cJSON *active = cJSON_CreateArray();
    for (int i = 0; i < AM_ACTIVE_MAX; i++) {
        am_active_view_t v;
        if (!am_copy_active(i, &v)) continue;
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "key", v.key);
        cJSON_AddStringToObject(a, "text", v.text);
        cJSON_AddNumberToObject(a, "since_s", (double)v.since_s);
        cJSON_AddItemToArray(active, a);
    }
    cJSON_AddItemToObject(root, "active", active);

    cJSON *events = cJSON_CreateArray();
    const uint32_t total = am_copy_total();
    for (uint32_t i = 0; i < AM_EVENTS; i++) {   /* newest first */
        am_event_t ev;
        if (!am_copy_event(total, i, &ev)) break;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "at_s", (double)ev.at_s);
        cJSON_AddStringToObject(e, "text", ev.text);
        cJSON_AddItemToArray(events, e);
    }
    cJSON_AddItemToObject(root, "events", events);

    cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)cap, 0);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}
