#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_LINE_MAX 192          /* 191 chars + NUL */
#define CLI_HIST_N   10
#define CLI_HIST_LEN 96

typedef enum { CLI_EVT_NONE = 0, CLI_EVT_LINE, CLI_EVT_TOO_LONG } cli_evt_t;

typedef struct {
    char     buf[CLI_LINE_MAX];
    uint16_t len;
    uint8_t  echo, esc, overflow, last_cr, browsing;
    char     hist[CLI_HIST_N][CLI_HIST_LEN];
    uint8_t  hist_count, hist_next;
    int      hist_pos;
    char     saved[CLI_LINE_MAX];
    uint16_t saved_len;
    uint32_t last_ms, noise, stale;
} cli_line_t;

void        cli_line_init(cli_line_t *l, int echo);
void        cli_line_set_echo(cli_line_t *l, int echo);          /* echo=1 human mode, 0 machine mode */
cli_evt_t   cli_line_feed(cli_line_t *l, uint8_t b, uint32_t now_ms,
                          char *echo_out, size_t echo_cap, size_t *echo_len);
const char *cli_line_take(cli_line_t *l);                        /* valid after CLI_EVT_LINE; clears the buffer */
const char *cli_line_peek(const cli_line_t *l);                  /* current edit buffer, NUL-terminated */
uint32_t    cli_line_noise(const cli_line_t *l);
uint32_t    cli_line_stale(const cli_line_t *l);

#ifdef __cplusplus
}
#endif
