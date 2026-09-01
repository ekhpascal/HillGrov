#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "notify.h"

#define NTF_NEVER 0xFFFFFFFFu   /* sentinel: type/idx has never fired */
#define NTF_IDX_N 8

typedef struct { ntf_sink_fn fn; void *ctx; uint16_t mask; } ntf_sink_t;

static uint32_t (*s_now_fn)(void);
static uint8_t   s_node_id;
static ntf_sink_t s_sinks[NTF_MAX_SINKS];
static int        s_sink_count;
static uint32_t   s_last_ms[NTF_COUNT][NTF_IDX_N];

/* Minimum interval (ms) per type, indexed by ntf_type_t. BOOT's NTF_NEVER makes
   it effectively "once per boot": after the first emit, elapsed time will never
   reach it again within a single boot. SAFE/NODE/WATER/FW are edge-triggered
   (0 = never rate-limited). */
static const uint32_t s_interval_ms[NTF_COUNT] = {
    NTF_NEVER, 1000, 0, 0, 2000, 0, 1000, 60000, 0, 200
};

static const char *const s_type_name[NTF_COUNT] = {
    "BOOT", "ALARM", "SAFE", "NODE", "RING", "WATER", "LIGHT", "SOIL", "FW", "CMD"
};

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

void notify_init(uint32_t (*now_ms)(void), uint8_t node_id) {
    s_now_fn = now_ms;
    s_node_id = node_id;
    s_sink_count = 0;
    memset(s_sinks, 0, sizeof s_sinks);
    for (int t = 0; t < NTF_COUNT; t++)
        for (int i = 0; i < NTF_IDX_N; i++)
            s_last_ms[t][i] = NTF_NEVER;
}

void notify_set_node_id(uint8_t id) { s_node_id = id; }

int notify_add_sink(ntf_sink_fn fn, void *ctx, uint16_t mask) {
    if (s_sink_count >= NTF_MAX_SINKS) return -1;
    int idx = s_sink_count++;
    s_sinks[idx].fn = fn;
    s_sinks[idx].ctx = ctx;
    s_sinks[idx].mask = mask;
    return idx;
}

void notify_set_sink_mask(int sink, uint16_t mask) {
    if (sink < 0 || sink >= s_sink_count) return;
    s_sinks[sink].mask = mask;
}

uint16_t notify_sink_mask(int sink) {
    if (sink < 0 || sink >= s_sink_count) return 0;
    return s_sinks[sink].mask;
}

int notify_parse(const char *name) {
    if (!name) return -1;
    if (ci_eq(name, "ALL")) return NTF_COUNT;
    for (int t = 0; t < NTF_COUNT; t++)
        if (ci_eq(name, s_type_name[t])) return t;
    return -1;
}

const char *notify_type_name(int t) {
    if (t < 0 || t >= NTF_COUNT) return "?";
    return s_type_name[t];
}

void notify_emit(ntf_type_t t, uint8_t idx, const char *fmt, ...) {
    uint8_t i = idx > 7 ? 7 : idx;
    uint32_t now = s_now_fn();
    uint32_t last = s_last_ms[t][i];
    if (last != NTF_NEVER && (now - last) < s_interval_ms[t]) return;
    s_last_ms[t][i] = now;

    char line[NTF_LINE_MAX];
    int n = snprintf(line, sizeof line, "NOTIFY %s %u ", s_type_name[t], (unsigned)s_node_id);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof line) n = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    size_t len = strlen(line);
    if (len >= NTF_LINE_MAX - 1) {          /* payload filled/overran the buffer: force the newline */
        line[NTF_LINE_MAX - 2] = '\n';
        line[NTF_LINE_MAX - 1] = '\0';
    } else {
        line[len] = '\n';
        line[len + 1] = '\0';
    }

    for (int s = 0; s < s_sink_count; s++)
        if (s_sinks[s].mask & NTF_MASK(t))
            s_sinks[s].fn(s_sinks[s].ctx, line);
}
