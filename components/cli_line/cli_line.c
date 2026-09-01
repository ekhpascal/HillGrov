#include <string.h>
#include "cli_line.h"

#define STALE_MS 30000u

void cli_line_init(cli_line_t *l, int echo) {
    memset(l, 0, sizeof *l);
    l->echo = (uint8_t)(echo ? 1 : 0);
    l->hist_pos = -1;
}

void cli_line_set_echo(cli_line_t *l, int echo) {
    l->echo = (uint8_t)(echo ? 1 : 0);
}

/* Echo is suppressed entirely in machine mode; excess bytes beyond echo_cap are dropped silently. */
static void echo_put(const cli_line_t *l, char *echo_out, size_t echo_cap, size_t *echo_len,
                      const char *s, size_t n) {
    if (!l->echo) return;
    size_t room = (echo_cap > *echo_len) ? (echo_cap - *echo_len) : 0;
    size_t cpy = (n < room) ? n : room;
    if (cpy) memcpy(echo_out + *echo_len, s, cpy);
    *echo_len += cpy;
}

static void hist_push(cli_line_t *l) {
    if (l->buf[0] == '\0') return;                        /* skip empty line */
    if (l->hist_count > 0) {
        uint8_t newest = (uint8_t)((l->hist_next + CLI_HIST_N - 1) % CLI_HIST_N);
        if (strcmp(l->hist[newest], l->buf) == 0) return;  /* skip duplicate of newest */
    }
    size_t n = strlen(l->buf);
    if (n > CLI_HIST_LEN - 1) n = CLI_HIST_LEN - 1;
    memcpy(l->hist[l->hist_next], l->buf, n);
    l->hist[l->hist_next][n] = '\0';
    l->hist_next = (uint8_t)((l->hist_next + 1) % CLI_HIST_N);
    if (l->hist_count < CLI_HIST_N) l->hist_count++;
}

/* pos: 0 = newest entry ... hist_count-1 = oldest entry */
static uint8_t hist_ring_index(const cli_line_t *l, int pos) {
    int idx = ((int)l->hist_next - 1 - pos) % CLI_HIST_N;
    if (idx < 0) idx += CLI_HIST_N;
    return (uint8_t)idx;
}

static void hist_load(cli_line_t *l, int pos) {
    const char *e = l->hist[hist_ring_index(l, pos)];
    size_t n = strlen(e);
    memcpy(l->buf, e, n);
    l->buf[n] = '\0';
    l->len = (uint16_t)n;
}

static void hist_echo(const cli_line_t *l, char *echo_out, size_t echo_cap, size_t *echo_len) {
    echo_put(l, echo_out, echo_cap, echo_len, "\r\x1b[2K", 4);
    echo_put(l, echo_out, echo_cap, echo_len, l->buf, l->len);
}

static void hist_up(cli_line_t *l, char *echo_out, size_t echo_cap, size_t *echo_len) {
    if (!l->browsing) {
        if (l->hist_count == 0) return;                    /* nothing to browse */
        memcpy(l->saved, l->buf, l->len);
        l->saved[l->len] = '\0';
        l->saved_len = l->len;
        l->browsing = 1;
        l->hist_pos = 0;
    } else if (l->hist_pos < (int)l->hist_count - 1) {
        l->hist_pos++;                                      /* clamped: stays at oldest */
    }
    hist_load(l, l->hist_pos);
    hist_echo(l, echo_out, echo_cap, echo_len);
}

static void hist_down(cli_line_t *l, char *echo_out, size_t echo_cap, size_t *echo_len) {
    if (!l->browsing) return;
    l->hist_pos--;
    if (l->hist_pos < 0) {                                  /* past newest -> restore saved partial */
        memcpy(l->buf, l->saved, l->saved_len);
        l->buf[l->saved_len] = '\0';
        l->len = l->saved_len;
        l->browsing = 0;
    } else {
        hist_load(l, l->hist_pos);
    }
    hist_echo(l, echo_out, echo_cap, echo_len);
}

cli_evt_t cli_line_feed(cli_line_t *l, uint8_t b, uint32_t now_ms,
                         char *echo_out, size_t echo_cap, size_t *echo_len) {
    *echo_len = 0;

    /* Machine mode: a partial line idle past STALE_MS is discarded before this byte is processed. */
    if (!l->echo && l->len > 0 && now_ms - l->last_ms > STALE_MS) {
        l->len = 0;
        l->buf[0] = '\0';
        l->stale++;
    }
    l->last_ms = now_ms;

    /* ESC [ A/B history navigation is human-mode only; in machine mode ESC bytes fall through to noise. */
    if (l->echo) {
        if (b == 0x1B) { l->esc = 1; return CLI_EVT_NONE; }
        if (l->esc == 1) {
            if (b == '[') l->esc = 2;
            else { l->esc = 0; l->noise++; }
            return CLI_EVT_NONE;
        }
        if (l->esc == 2) {
            l->esc = 0;
            if (b == 'A') hist_up(l, echo_out, echo_cap, echo_len);
            else if (b == 'B') hist_down(l, echo_out, echo_cap, echo_len);
            return CLI_EVT_NONE;
        }
    }

    if (b == '\r' || b == '\n') {
        if (b == '\n' && l->last_cr) { l->last_cr = 0; return CLI_EVT_NONE; }  /* LF after CR swallowed */
        l->last_cr = (uint8_t)(b == '\r');
        l->browsing = 0;
        if (l->overflow) {
            l->overflow = 0;
            l->len = 0;
            l->buf[0] = '\0';
            echo_put(l, echo_out, echo_cap, echo_len, "\r\n", 2);
            return CLI_EVT_TOO_LONG;
        }
        l->buf[l->len] = '\0';
        hist_push(l);
        echo_put(l, echo_out, echo_cap, echo_len, "\r\n", 2);
        return CLI_EVT_LINE;
    }

    if (b == 0x03) {                                        /* Ctrl-C: clear the in-progress line */
        l->len = 0;
        l->buf[0] = '\0';
        l->overflow = 0;
        l->browsing = 0;
        l->hist_pos = -1;
        l->last_cr = 0;
        echo_put(l, echo_out, echo_cap, echo_len, "^C\r\n", 4);
        return CLI_EVT_NONE;
    }

    if (b == 0x08 || b == 0x7F) {                           /* backspace / DEL */
        if (l->len > 0) {
            l->len--;
            l->buf[l->len] = '\0';
            l->overflow = 0;
            echo_put(l, echo_out, echo_cap, echo_len, "\b \b", 3);
        }
        return CLI_EVT_NONE;
    }

    if (b >= 0x20 && b <= 0x7E) {                           /* printable */
        l->browsing = 0;
        if (l->len == CLI_LINE_MAX - 1) {
            l->overflow = 1;
        } else {
            l->buf[l->len++] = (char)b;
            l->buf[l->len] = '\0';
            char c = (char)b;
            echo_put(l, echo_out, echo_cap, echo_len, &c, 1);
        }
        return CLI_EVT_NONE;
    }

    l->noise++;                                             /* 0x00/0xFF/other control bytes */
    return CLI_EVT_NONE;
}

const char *cli_line_take(cli_line_t *l) {
    l->buf[l->len] = '\0';
    l->len = 0;
    l->browsing = 0;
    l->overflow = 0;
    return l->buf;
}

const char *cli_line_peek(const cli_line_t *l) { return l->buf; }

uint32_t cli_line_noise(const cli_line_t *l) { return l->noise; }
uint32_t cli_line_stale(const cli_line_t *l) { return l->stale; }
