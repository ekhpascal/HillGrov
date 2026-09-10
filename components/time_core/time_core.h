#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure POSIX-TZ subset parser and UTC-offset calculator: no IDF headers, no
 * dynamic allocation, host-testable. Covers the grammar every IANA zone's
 * POSIX TZ string (as published in tzdata's zone1970.tab-derived "TZ" line)
 * reduces to:
 *
 *   STD offset [DST [offset] , Mm.w.d[/time] , Mm.w.d[/time]]
 *
 * STD/DST names are either 1+ plain letters (CET, EST, ...) or a quoted
 * <...> form (<+03>, letters/digits/+/- between the brackets) -- either way
 * the name itself is never stored, only skipped: nothing downstream needs it.
 * NOT supported: Julian day-of-year rules (Jn / n) and the bare TZ-without-
 * offset shorthand -- tz_parse returns -1 for anything outside the grammar
 * above, including a DST name with no comma-separated rule pair. */

typedef struct {
    int32_t std_off_s;   /* seconds EAST of UTC in standard time (CET = +3600) */
    int32_t dst_off_s;    /* seconds EAST of UTC in DST (only meaningful if has_dst) */
    uint8_t has_dst;
    uint8_t sm, sw, sd;   /* DST START rule: month 1..12, week 1..5 (5 = last), weekday 0..6 (Sun=0) */
    int32_t st;           /* DST START local STANDARD-time-of-day, seconds (default 7200 = 02:00:00) */
    uint8_t em, ew, ed;   /* DST END rule, same encoding */
    int32_t et;           /* DST END local DST-time-of-day, seconds */
} tz_rule_t;

/* Parses a POSIX TZ string into *out. 0 ok, -1 on anything the grammar above
 * doesn't cover (including a plain unparseable string). *out is untouched on
 * failure. */
int tz_parse(const char *posix, tz_rule_t *out);

/* Seconds EAST of UTC in effect at the given UTC instant (CET winter noon =
 * +3600). r must come from a successful tz_parse -- passing NULL returns 0. */
int32_t tz_offset_at(const tz_rule_t *r, uint32_t utc);

/* hg_tz_check_fn (components/hg_mcfg/hg_mcfg.h): 0 ok / -1, mirrors tz_parse
 * exactly (parses into a scratch tz_rule_t and discards it). */
int tz_check(const char *posix);

#ifdef __cplusplus
}
#endif
