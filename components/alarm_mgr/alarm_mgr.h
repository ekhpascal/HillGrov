#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* alarm_mgr -- a pure (host-tested) NOTIFY sink: keeps a bounded ring of every
 * NOTIFY line ever seen plus a derived "active" set (currently-abnormal
 * nodes/rings/etc, keyed by "<TYPE> <node>"), and exports both as JSON for the
 * master's web UI. Fed exclusively through alarm_mgr_sink(), registered as an
 * ntf_sink_fn with notify_add_sink(). No IDF headers; time comes from the
 * injected now_s clock so this links and runs unmodified on the host. */

#define AM_EVENTS 64

typedef struct {
    uint32_t at_s;
    uint8_t  type;      /* ntf_type_t */
    uint8_t  node;
    char     text[72];  /* "<TYPE> <node> <rest>", i.e. the line minus "NOTIFY " and '\n' */
} am_event_t;

#define AM_ACTIVE_MAX 16
#define AM_KEY_MAX    16   /* "<TYPE> <node>": longest type name (ALARM/WATER/LIGHT) is 5 + ' ' + up to 3 digits */

/* A consistent copy of everything /api/alarms exports, for a reader that wants
 * structs (the panel). active[] is in the same order the JSON lists it;
 * events[] newest first. ~6.6 KB: keep it off the stack -- heap-allocate it
 * for the length of one use, or put it in PSRAM. alarm_mgr_json does not use
 * it: it copies one entry per lock hold (alarm_mgr_internal.h). */
typedef struct { char key[AM_KEY_MAX]; char text[72]; uint32_t since_s; } am_active_view_t;
typedef struct {
    am_active_view_t active[AM_ACTIVE_MAX]; int n_active;
    am_event_t       events[AM_EVENTS];     int n_events;   /* newest first, <= AM_EVENTS */
    uint32_t         total;
} am_snapshot_t;

/* The sink runs inline in whichever task emits a NOTIFY (sometimes inside
 * another component's lock), and readers run on other tasks. Inject a lock:
 * NULL,NULL (the default) = none, which is what the host tests and a
 * single-task build use. The lock is held for copies and mutations only
 * (microseconds) -- never around formatting -- so a spinlock (portMUX) is the
 * right kind. Set it once, right after alarm_mgr_init(), before any sink can
 * run; alarm_mgr_init() does not reset it. */
void alarm_mgr_set_lock(void (*lock)(void), void (*unlock)(void));

void alarm_mgr_copy(am_snapshot_t *out);   /* [ANY] */

void alarm_mgr_init(uint32_t (*now_s)(void));
void alarm_mgr_sink(void *ctx, const char *line);   /* ntf_sink_fn: notify_add_sink(alarm_mgr_sink, NULL, NTF_MASK_ALL) */
int  alarm_mgr_active_count(void);
int  alarm_mgr_total(void);                         /* events ever recorded, including ones dropped from the ring */
int  alarm_mgr_json(char *out, size_t cap);          /* {"active":[{"key":..,"text":..,"since_s":..}],"events":[{"at_s":..,"text":..}, newest first, <=AM_EVENTS]} -- each entry is copied under the lock, then formatted outside it */

#ifdef __cplusplus
}
#endif
