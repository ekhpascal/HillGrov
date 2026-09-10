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

void alarm_mgr_init(uint32_t (*now_s)(void));
void alarm_mgr_sink(void *ctx, const char *line);   /* ntf_sink_fn: notify_add_sink(alarm_mgr_sink, NULL, NTF_MASK_ALL) */
int  alarm_mgr_active_count(void);
int  alarm_mgr_total(void);                         /* events ever recorded, including ones dropped from the ring */
int  alarm_mgr_json(char *out, size_t cap);          /* {"active":[{"key":..,"text":..,"since_s":..}],"events":[{"at_s":..,"text":..}, newest first, <=AM_EVENTS]} */

#ifdef __cplusplus
}
#endif
