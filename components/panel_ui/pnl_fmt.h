#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ring_proto.h"   /* hg_node_t */
#include "wifi_mgr.h"     /* wifi_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The web's words and numbers, for the panel (pure; ASCII only -- any byte
 * outside 0x20..0x7E, e.g. a UTF-8 SSID, becomes '?', because the built-in
 * Montserrat has no glyph for it). Every function writes a NUL-terminated
 * string and returns snprintf's count (or 0). */

void pnl_zone_name(const hg_node_t *n, char out[17]);                      /* name, or "Z<id>" when empty (app.js:788) */
int  pnl_fmt_age(uint32_t now_s, uint32_t stamp_s, char *out, size_t cap); /* the web's fmtAge + " ago": "12s ago",
                                                                              "4m 3s ago", "3h 5m ago", "2d 4h ago";
                                                                              a stamp newer than now clamps to "0s ago" */
int  pnl_fmt_sta(const wifi_status_t *w, char *out, size_t cap);           /* "192.168.1.5 | house | -61 dBm" | "STA down: <reason|-->" */
int  pnl_fmt_ap(const wifi_status_t *w, char *out, size_t cap);            /* "HillGrow | 2 client(s) | 192.168.7.7" */

/* The master's UTC "YYYY-MM-DD HH:MM:SS" shown as LOCAL time and labelled:
 * "2026-09-18 14:32:05 (UTC+02:00) NTP". !is_set -> "Clock not set (<src>)".
 * An unparseable utc19 is shown as-is: "<utc19> <src>". The web shows UTC
 * unlabelled (D28); that fix is out of scope. */
int  pnl_fmt_master_time(const char *utc19, const char *src, int32_t offset_s, int is_set, char *out, size_t cap);

typedef struct { int soil_pct, light_pct, pump_s; uint8_t any; } pnl_readings_t;
void pnl_node_readings(const hg_node_t *n, pnl_readings_t *out);           /* app.js:603-617 shelfTotals over min(n_shelves,4) */
int  pnl_fmt_reading(const pnl_readings_t *r, int which /*0 soil 1 light 2 pump*/, char *out, size_t cap);  /* "41%" "12s" "--" */

/* Roller options: n numbers from `from`, each zero-padded to `width` digits, one per line ("00\n01\n..\n23" for
 * (0, 24, 2); "2024\n2025.." for years). Stops after the last whole number that fits cap; returns the length. */
size_t pnl_fmt_roller_opts(char *out, size_t cap, int from, int n, int width);
/* Strips trailing '\n' / '\r' in place: a CLI reply's line end, before it goes under a label. NULL-safe. */
void   pnl_fmt_trim_eol(char *s);

#ifdef __cplusplus
}
#endif
