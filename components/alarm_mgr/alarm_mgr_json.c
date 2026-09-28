#include <string.h>
#include "cJSON.h"
#include "alarm_mgr.h"
#include "alarm_mgr_internal.h"

int alarm_mgr_json(char *out, size_t cap) {
    /* Copied under the lock (inside alarm_mgr_copy), formatted outside it, so
     * the lock is held for microseconds and never across cJSON's allocations.
     * Static because it is ~6.6 KB and there is one caller: /api/alarms on the
     * single httpd task. */
    static am_snapshot_t s;
    alarm_mgr_copy(&s);

    cJSON *root = cJSON_CreateObject();

    cJSON *active = cJSON_CreateArray();
    for (int i = 0; i < s.n_active; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "key", s.active[i].key);
        cJSON_AddStringToObject(a, "text", s.active[i].text);
        cJSON_AddNumberToObject(a, "since_s", (double)s.active[i].since_s);
        cJSON_AddItemToArray(active, a);
    }
    cJSON_AddItemToObject(root, "active", active);

    cJSON *events = cJSON_CreateArray();
    for (int i = 0; i < s.n_events; i++) {   /* already newest first */
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "at_s", (double)s.events[i].at_s);
        cJSON_AddStringToObject(e, "text", s.events[i].text);
        cJSON_AddItemToArray(events, e);
    }
    cJSON_AddItemToObject(root, "events", events);

    cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)cap, 0);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}
