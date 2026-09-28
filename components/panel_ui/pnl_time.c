#include <stdio.h>
#include <string.h>
#include "pnl_time.h"

static const char *const WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MON[12] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                          /* [0, 399] */
    int mp  = m > 2 ? m - 3 : m + 9;                  /* [0, 11], March-based */
    int doy = (153 * mp + 2) / 5 + d - 1;             /* [0, 365] */
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return (int64_t)era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);                                  /* [0, 146096] */
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;    /* [0, 399] */
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                  /* [0, 365] */
    int mp  = (5 * doy + 2) / 153;                                      /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

int pnl_days_in_month(int y, int mo) {
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12) return 0;
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return DIM[mo - 1];
}

int64_t pnl_utc_from_civil(int y, int mo, int d, int h, int mi, int s) {
    return days_from_civil(y, mo, d) * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

void pnl_local_time(int64_t utc_s, int32_t offset_s, int time_is_set, pnl_local_t *out) {
    memset(out, 0, sizeof *out);
    out->valid = time_is_set ? 1 : 0;
    int64_t t = utc_s + offset_s;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);   /* floor division */
    int sod = (int)(t - days * 86400);                              /* [0, 86399] */
    civil_from_days(days, &out->year, &out->mon, &out->mday);
    out->wday = (int)(((days % 7) + 11) % 7);                       /* 1970-01-01 (day 0) was a Thursday */
    out->hour = sod / 3600;
    out->min  = (sod / 60) % 60;
    out->sec  = sod % 60;
    out->minute_of_day = sod / 60;
}

int pnl_fmt_clock(const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid) return snprintf(out, cap, "--:--");
    return snprintf(out, cap, "%02d:%02d", t->hour, t->min);
}

int pnl_fmt_date(const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid || t->wday < 0 || t->wday > 6 || t->mon < 1 || t->mon > 12)
        return snprintf(out, cap, "Clock not set");
    return snprintf(out, cap, "%s %d %s", WDAY[t->wday], t->mday, MON[t->mon - 1]);
}

int pnl_set_time_line(int y, int mo, int d, int h, int mi, int32_t offset_s, char *out, size_t cap) {
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > pnl_days_in_month(y, mo) ||
        h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
    pnl_local_t u;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, 0) - offset_s, 0, 1, &u);
    if (u.year < 2020 || u.year > 2099) return -1;
    snprintf(out, cap, "SET TIME %04d-%02d-%02d %02d:%02d:00", u.year, u.mon, u.mday, u.hour, u.min);
    return 0;
}
