#include <stdio.h>
#include <string.h>
#include "alarm_mgr.h"
#include "alarm_mgr_internal.h"
#include "notify.h"

am_event_t  am_ring[AM_EVENTS];
uint32_t     am_total;
am_active_t am_active[AM_ACTIVE_MAX];

static uint32_t (*s_now_s)(void);

/* Active-set vocabulary (brief, verbatim): a line becomes active when its
 * first word after the node is one of these, or starts with a "W_"/"F_"
 * fault-token prefix; it clears on one of CLR_WORDS. The two sets are
 * disjoint, so evaluation order doesn't matter. */
static const char *const ACT_WORDS[] = {
    "DEGRADED", "OFFLINE", "OPEN", "UPDATING", "FAILED",
    "CFG_SYNC_FAILED", "CFG_FORK", "ID_CONFLICT", "SAFE"
};
static const char *const CLR_WORDS[] = {
    "ONLINE", "CLOSED", "DONE", "OK", "CLEARED", "TRIAL", "PASS"
};

void alarm_mgr_init(uint32_t (*now_s)(void)) {
    s_now_s = now_s;
    am_total = 0;
    memset(am_ring, 0, sizeof am_ring);
    memset(am_active, 0, sizeof am_active);
}

static void bcopy_trunc(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int word_eq(const char *w, size_t wlen, const char *lit) {
    size_t l = strlen(lit);
    return wlen == l && memcmp(w, lit, l) == 0;
}

static int is_activate_word(const char *w, size_t wlen) {
    if (wlen >= 2 && (memcmp(w, "W_", 2) == 0 || memcmp(w, "F_", 2) == 0)) return 1;
    for (size_t i = 0; i < sizeof(ACT_WORDS) / sizeof(ACT_WORDS[0]); i++)
        if (word_eq(w, wlen, ACT_WORDS[i])) return 1;
    return 0;
}

static int is_clear_word(const char *w, size_t wlen) {
    for (size_t i = 0; i < sizeof(CLR_WORDS) / sizeof(CLR_WORDS[0]); i++)
        if (word_eq(w, wlen, CLR_WORDS[i])) return 1;
    return 0;
}

/* Upsert the active-set entry for `key`: an already-active key refreshes its
 * text but keeps the original since_s (the start of the active streak, not
 * the latest state change within it); a brand-new key takes the first free
 * slot, or is silently dropped once all AM_ACTIVE_MAX are in use. */
static void active_upsert(const char *key, const char *text, uint32_t now) {
    int free_slot = -1;
    for (int i = 0; i < AM_ACTIVE_MAX; i++) {
        if (!am_active[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (strcmp(am_active[i].key, key) == 0) {
            bcopy_trunc(am_active[i].text, sizeof am_active[i].text, text, strlen(text));
            return;
        }
    }
    if (free_slot < 0) return;
    am_active_t *a = &am_active[free_slot];
    a->used = 1;
    a->since_s = now;
    bcopy_trunc(a->key, sizeof a->key, key, strlen(key));
    bcopy_trunc(a->text, sizeof a->text, text, strlen(text));
}

static void active_clear(const char *key) {
    for (int i = 0; i < AM_ACTIVE_MAX; i++)
        if (am_active[i].used && strcmp(am_active[i].key, key) == 0) { am_active[i].used = 0; return; }
}

/* ntf_sink_fn. Line format: "NOTIFY <TYPE> <node> <rest>\n". Manual bounded
 * split (no strtok): copy into a fixed work buffer first so every scan below
 * is bounded regardless of how long/unterminated the caller's `line` is. */
void alarm_mgr_sink(void *ctx, const char *line) {
    (void)ctx;
    if (!line) return;

    char work[160];
    size_t n = 0;
    while (line[n] != '\0' && n < sizeof(work) - 1) { work[n] = line[n]; n++; }
    work[n] = '\0';
    while (n > 0 && (work[n - 1] == '\n' || work[n - 1] == '\r')) work[--n] = '\0';

    if (strncmp(work, "NOTIFY ", 7) != 0) return;      /* malformed: no prefix */
    const char *text_start = work + 7;                  /* "<TYPE> <node> <rest>" */
    const char *p = text_start;

    const char *type_word = p;
    while (*p && *p != ' ') p++;
    size_t type_len = (size_t)(p - type_word);
    if (type_len == 0 || type_len >= 16) return;         /* malformed: no/too-long type */
    char type_buf[16];
    memcpy(type_buf, type_word, type_len);
    type_buf[type_len] = '\0';
    int type = notify_parse(type_buf);
    if (type < 0 || type >= NTF_COUNT) return;            /* malformed: unknown type */

    if (*p != ' ') return;                                /* malformed: no node */
    p++;
    const char *node_word = p;
    while (*p && *p != ' ') p++;
    size_t node_len = (size_t)(p - node_word);
    if (node_len == 0 || node_len > 3) return;            /* malformed: no/too-long node */
    unsigned node_val = 0;
    for (size_t i = 0; i < node_len; i++) {
        char c = node_word[i];
        if (c < '0' || c > '9') return;                   /* malformed: non-numeric node */
        node_val = node_val * 10 + (unsigned)(c - '0');
    }
    if (node_val > 255) return;                           /* malformed: node out of uint8_t range */
    uint8_t node = (uint8_t)node_val;

    const char *rest = (*p == ' ') ? p + 1 : p;
    const char *w1 = rest, *w1e = w1;
    while (*w1e && *w1e != ' ') w1e++;
    size_t w1len = (size_t)(w1e - w1);

    uint32_t now = s_now_s ? s_now_s() : 0;
    am_event_t *ev = &am_ring[am_total % AM_EVENTS];
    ev->at_s = now;
    ev->type = (uint8_t)type;
    ev->node = node;
    bcopy_trunc(ev->text, sizeof ev->text, text_start, strlen(text_start));
    am_total++;

    if (type != NTF_BOOT && type != NTF_CMD && type != NTF_WIFI) {
        char key[AM_KEY_MAX];
        snprintf(key, sizeof key, "%s %u", notify_type_name(type), (unsigned)node);
        if (is_activate_word(w1, w1len))
            active_upsert(key, ev->text, now);
        else if (is_clear_word(w1, w1len))
            active_clear(key);
    }
}

int alarm_mgr_active_count(void) {
    int c = 0;
    for (int i = 0; i < AM_ACTIVE_MAX; i++) if (am_active[i].used) c++;
    return c;
}

int alarm_mgr_total(void) { return (int)am_total; }
