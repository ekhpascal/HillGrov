#include <stdio.h>
#include <string.h>
#include "pnl_zero.h"
#include "pnl_console.h"

/* dst is cleared whole first: a shorter line must not leave the tail of the previous one (a credential, say) behind
 * its terminator. pnl_zero, not memset: C3. */
static void put_clean(char *dst, size_t cap, const char *src) {
    pnl_zero(dst, cap);
    size_t n = src ? strlen(src) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n) memcpy(dst, src, n);
}

static int is_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

static char up(char ch) { return (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch; }

void pnl_con_init(pnl_console_t *c, char (*reply_store)[CMD_RESP_MAX]) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < PNL_CON_LOG; i++) {
        c->log[i].reply = reply_store[i];
        reply_store[i][0] = '\0';
    }
}

int pnl_con_forward(const char *line, uint8_t zone, char *out, size_t cap) {
    if (!line || !out || cap == 0) return -1;
    const size_t line_max = (size_t)(CMD_LINE_MAX - 1);
    size_t limit = cap - 1 < line_max ? cap - 1 : line_max;
    const char *p = line;
    while (*p && is_ws(*p)) p++;
    const char *t0 = p;
    while (*p && !is_ws(*p)) p++;
    size_t l0 = (size_t)(p - t0);
    const char *q = p;
    while (*q && is_ws(*q)) q++;
    const char *t1 = q;
    while (*q && !is_ws(*q)) q++;
    size_t l1 = (size_t)(q - t1);
    int addressed = l1 == 4 && up(t1[0]) == 'Z' && up(t1[1]) == 'O' && up(t1[2]) == 'N' && up(t1[3]) == 'E';
    if (l0 == 0 || addressed) {                 /* no tokens, or already "VERB ZONE n ...": unchanged (app.js) */
        size_t n = strlen(line);
        if (n > limit) return -1;
        memcpy(out, line, n + 1);
        return 0;
    }
    char buf[2 * CMD_LINE_MAX];
    int rc = -1;
    int o = snprintf(buf, sizeof buf, "%.*s ZONE %u", (int)l0, t0, (unsigned)zone);
    if (o < 0 || (size_t)o >= sizeof buf) goto out;
    for (;;) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !is_ws(*p)) p++;
        size_t k = (size_t)(p - s);
        if ((size_t)o + 1 + k >= sizeof buf) goto out;
        buf[o++] = ' ';
        memcpy(buf + o, s, k);
        o += (int)k;
        buf[o] = '\0';
    }
    if ((size_t)o > limit) goto out;
    memcpy(out, buf, (size_t)o + 1);
    rc = 0;
out:
    pnl_zero(buf, sizeof buf);                  /* the line may carry a credential */
    return rc;
}

static int is_trim(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

const char *pnl_con_span(const char *line, size_t *len) {
    if (!line) { *len = 0; return ""; }
    while (*line && is_trim(*line)) line++;
    size_t n = strlen(line);
    while (n > 0 && is_trim(line[n - 1])) n--;
    *len = n;
    return line;
}

int pnl_con_line_ok(const char *line) {
    size_t n;
    (void)pnl_con_span(line, &n);
    return n >= 1 && n <= (size_t)(CMD_LINE_MAX - 1);
}

pnl_con_entry_t *pnl_con_push(pnl_console_t *c, const char *sent) {
    pnl_con_entry_t *e = &c->log[c->head];
    if (e->pending) return NULL;
    put_clean(e->sent, sizeof e->sent, sent);
    pnl_zero(e->reply, CMD_RESP_MAX);           /* the reply this entry held before it came round again */
    e->pending = 1;
    e->used = 1;
    c->head = (c->head + 1) % PNL_CON_LOG;
    if (c->n_log < PNL_CON_LOG) c->n_log++;
    return e;
}

void pnl_con_hist_add(pnl_console_t *c, const char *line) {
    if (c->n_hist == PNL_CON_HIST) {
        memmove(c->hist[0], c->hist[1], (size_t)(PNL_CON_HIST - 1) * CMD_LINE_MAX);
        c->n_hist--;
    }
    put_clean(c->hist[c->n_hist], CMD_LINE_MAX, line);
    c->n_hist++;
    c->hist_pos = c->n_hist;
}

const pnl_con_entry_t *pnl_con_log_at(const pnl_console_t *c, int i) {
    if (i < 0 || i >= c->n_log) return NULL;
    return &c->log[(c->head - c->n_log + i + 2 * PNL_CON_LOG) % PNL_CON_LOG];
}

void pnl_con_reply(pnl_con_entry_t *e, const char *reply) {
    if (!e) return;
    put_clean(e->reply, CMD_RESP_MAX, reply);
    e->pending = 0;
}

const char *pnl_con_hist_prev(pnl_console_t *c) {
    if (c->n_hist == 0) return NULL;
    if (c->hist_pos > 0) c->hist_pos--;
    return c->hist[c->hist_pos];
}

const char *pnl_con_hist_next(pnl_console_t *c) {
    if (c->hist_pos < c->n_hist) c->hist_pos++;
    return c->hist_pos < c->n_hist ? c->hist[c->hist_pos] : "";
}

void pnl_con_wipe(pnl_console_t *c) {
    int pending = 0;
    for (int i = 0; i < PNL_CON_LOG; i++) {
        pnl_con_entry_t *e = &c->log[i];
        if (e->pending) { pending = 1; continue; }   /* the worker owns it until done() */
        pnl_zero(e->sent, sizeof e->sent);
        pnl_zero(e->reply, CMD_RESP_MAX);
        e->used = 0;
    }
    if (!pending) { c->head = 0; c->n_log = 0; }
    pnl_zero(c->hist, sizeof c->hist);
    c->n_hist = 0;
    c->hist_pos = 0;
    pnl_zero(c->draft, sizeof c->draft);
    c->forward = 0;
}
