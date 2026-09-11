#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "time_core.h"

/* ---- civil-calendar arithmetic (Howard Hinnant's "days_from_civil" /
 * "civil_from_days" -- http://howardhinnant.github.io/date_algorithms.html,
 * proleptic Gregorian, correct for any year in int64_t range). Only the
 * signed 32-bit-safe range our uint32_t utc argument can reach is ever
 * exercised here. ---- */

static int64_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                        /* [0, 399] */
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5u + (unsigned)d - 1u; /* [0,365] */
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;          /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                     /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; /* [0,399] */
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);        /* [0, 365] */
    unsigned mp = (5u * doy + 2u) / 153u;                              /* [0, 11] */
    unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;                    /* [1, 31] */
    unsigned mm = mp + (mp < 10u ? 3u : (unsigned)-9);                 /* [1, 12] */
    yr += (mm <= 2u);
    *y = (int)yr; *m = (int)mm; *d = (int)dd;
}

static int weekday_of_days(int64_t days) {   /* 0=Sunday..6=Saturday; epoch day 0 (1970-01-01) was Thursday */
    int64_t wd = (days + 4) % 7;
    if (wd < 0) wd += 7;
    return (int)wd;
}

/* Floor division by 86400 (divisor always positive here): C's `/` truncates
 * toward zero, which is wrong for a negative dividend (e.g. -1 / 86400 == 0
 * in C, but the floor -- the correct "day containing that second" -- is
 * -1). Needed because utc + std_off_s can go negative for a large negative
 * offset near the 1970 epoch. */
static int64_t floor_div86400(int64_t a) {
    int64_t q = a / 86400;
    if (a % 86400 != 0 && a < 0) q--;
    return q;
}

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m) {
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : dim[m - 1];
}

/* Day-of-month of the w'th weekday `dow` in `month` (w=5 means the LAST such
 * weekday, whether or not a literal 5th occurrence exists -- POSIX's Mm.w.d). */
static int nth_weekday_day(int year, int month, int w, int dow) {
    int wd1 = weekday_of_days(days_from_civil(year, month, 1));
    int first = 1 + ((dow - wd1 + 7) % 7);
    if (w == 5) {
        int dim = days_in_month(year, month);
        return first + 7 * ((dim - first) / 7);
    }
    return first + 7 * (w - 1);
}

/* ---- grammar ---- */

/* Skips a STD/DST name: plain letters, or a quoted <...> form. Returns the
 * position just past it, or NULL if no valid name starts at p. */
static const char *skip_name(const char *p) {
    if (*p == '<') {
        const char *start = p + 1;
        const char *q = start;
        while (*q && *q != '>') q++;
        if (*q != '>' || q == start) return NULL;
        return q + 1;
    }
    if (!isalpha((unsigned char)*p)) return NULL;
    while (isalpha((unsigned char)*p)) p++;
    return p;
}

/* Signed hh[:mm[:ss]] -> total seconds (the sign belongs to the whole value,
 * not just hh). A bare sign or no leading digit is a parse failure, and so
 * is a magnitude outside +-max_h hours -- used for both the STD/DST numeric
 * offsets (POSIX sign: value SUBTRACTED from local time to get UTC; POSIX
 * bounds these to +-24h) and an M-rule's /time-of-day, a plain signed offset
 * from local midnight (POSIX bounds this to +-167h) -- the caller decides
 * which meaning, and which bound, applies. */
static const char *parse_hms(const char *p, int32_t max_h, int32_t *out) {
    int sign = 1;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return NULL;
    char *end;
    long h = strtol(p, &end, 10);
    p = end;
    long m = 0, s = 0;
    if (*p == ':') {
        p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        m = strtol(p, &end, 10); p = end;
        if (*p == ':') {
            p++;
            if (!isdigit((unsigned char)*p)) return NULL;
            s = strtol(p, &end, 10); p = end;
        }
    }
    int32_t total = (int32_t)(sign * (h * 3600 + m * 60 + s));
    if (total > max_h * 3600 || total < -max_h * 3600) return NULL;
    *out = total;
    return p;
}

/* "Mm.w.d[/time]" only -- Julian (Jn / bare n) rules are rejected outright. */
static const char *parse_mrule(const char *p, uint8_t *m, uint8_t *w, uint8_t *d, int32_t *t) {
    if (*p != 'M') return NULL;
    p++;
    char *end;
    long mm = strtol(p, &end, 10);
    if (end == p || mm < 1 || mm > 12 || *end != '.') return NULL;
    p = end + 1;
    long ww = strtol(p, &end, 10);
    if (end == p || ww < 1 || ww > 5 || *end != '.') return NULL;
    p = end + 1;
    long dd = strtol(p, &end, 10);
    if (end == p || dd < 0 || dd > 6) return NULL;
    p = end;
    *m = (uint8_t)mm; *w = (uint8_t)ww; *d = (uint8_t)dd;
    if (*p == '/') {
        p = parse_hms(p + 1, 167, t);
        if (!p) return NULL;
    } else {
        *t = 2 * 3600;   /* POSIX default: 02:00:00 local (time in effect before the transition) */
    }
    return p;
}

int tz_parse(const char *posix, tz_rule_t *out) {
    if (!posix || !out) return -1;
    tz_rule_t r; memset(&r, 0, sizeof r);
    const char *p = posix;

    p = skip_name(p);
    if (!p) return -1;
    int32_t std_num;
    p = parse_hms(p, 24, &std_num);
    if (!p) return -1;
    r.std_off_s = -std_num;   /* POSIX sign: CET-1 -> std_num=-1 -> +3600 east of UTC */

    if (*p == '\0') {
        r.has_dst = 0;
        *out = r;
        return 0;
    }

    p = skip_name(p);          /* DST name */
    if (!p) return -1;
    r.has_dst = 1;
    if (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) {
        int32_t dst_num;
        p = parse_hms(p, 24, &dst_num);
        if (!p) return -1;
        r.dst_off_s = -dst_num;
    } else {
        r.dst_off_s = r.std_off_s + 3600;   /* default: DST is 1h ahead of STD */
    }

    if (*p != ',') return -1;   /* the US-default-rule-omitted shorthand is not supported */
    p = parse_mrule(p + 1, &r.sm, &r.sw, &r.sd, &r.st);
    if (!p || *p != ',') return -1;
    p = parse_mrule(p + 1, &r.em, &r.ew, &r.ed, &r.et);
    if (!p || *p != '\0') return -1;

    *out = r;
    return 0;
}

int32_t tz_offset_at(const tz_rule_t *r, uint32_t utc) {
    if (!r) return 0;
    if (!r->has_dst) return r->std_off_s;

    int y, mo, d;
    /* The Mm.w.d rules must be evaluated against the LOCAL calendar year: a
     * late-December UTC instant can already be Jan 1 local (positive
     * offset), or an early-January UTC instant can still be Dec 31 local
     * (negative offset) -- using the raw UTC day picks the wrong year's
     * transition dates in exactly that window. Shifting by the STD offset
     * (rather than computing which of STD/DST is in effect first, which is
     * exactly the question this year selection exists to answer) is the
     * standard technique and is sufficient: STD and DST differ by at most a
     * couple of hours, nowhere near enough to cross a further day boundary
     * once already shifted onto local time. */
    civil_from_days(floor_div86400((int64_t)utc + r->std_off_s), &y, &mo, &d);

    int64_t start_days = days_from_civil(y, r->sm, nth_weekday_day(y, r->sm, r->sw, r->sd));
    int64_t end_days   = days_from_civil(y, r->em, nth_weekday_day(y, r->em, r->ew, r->ed));
    /* Transition instants are given in the time in effect BEFORE that
     * transition: the START rule's time-of-day is local STANDARD time, the
     * END rule's is local DST time -- so each converts to UTC using the
     * offset that is still active at that instant. */
    int64_t start_utc = start_days * 86400 + r->st - r->std_off_s;
    int64_t end_utc   = end_days   * 86400 + r->et - r->dst_off_s;

    int64_t u = (int64_t)utc;
    int dst_active = (start_utc <= end_utc)
        ? (u >= start_utc && u < end_utc)          /* northern-hemisphere-style: DST inside [start,end) */
        : (u >= start_utc || u < end_utc);         /* southern-hemisphere-style: DST wraps the year end */
    return dst_active ? r->dst_off_s : r->std_off_s;
}

int tz_check(const char *posix) {
    tz_rule_t scratch;
    return tz_parse(posix, &scratch);
}
