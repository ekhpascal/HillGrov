#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local civil time for the panel (pure). The master's clock and SET TIME are
 * UTC -- nothing calls setenv("TZ")/tzset -- so the panel's local time is
 * time(NULL) + time_svc_utc_offset(), computed here with Howard Hinnant's
 * days-from-civil algorithm, no libc tz. While the clock is unset every text
 * says so rather than showing a plausible wrong time. */

typedef struct { uint8_t valid; int year, mon /*1..12*/, mday, wday /*0=Sunday*/, hour, min, sec, minute_of_day; } pnl_local_t;

void pnl_local_time(int64_t utc_s, int32_t offset_s, int time_is_set, pnl_local_t *out);   /* valid = time_is_set */
int  pnl_fmt_clock(const pnl_local_t *t, char *out, size_t cap);   /* "14:32" | "--:--" */
int  pnl_fmt_date(const pnl_local_t *t, char *out, size_t cap);    /* "Thursday 18 September" | "Clock not set" */

/* Local wall time -> the master's UTC "SET TIME YYYY-MM-DD HH:MM:00". 0, or -1
 * when the local date/time is invalid or either it or its UTC equivalent falls
 * outside 2020..2099 (the master's own SET TIME range). */
int  pnl_set_time_line(int y, int mo, int d, int h, int mi, int32_t offset_s, char *out, size_t cap);

int64_t pnl_utc_from_civil(int y, int mo, int d, int h, int mi, int s);   /* seconds since 1970-01-01 00:00:00 */
int     pnl_days_in_month(int y, int mo);                                  /* 28..31; 0 for a month outside 1..12 */

#ifdef __cplusplus
}
#endif
