#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "hg_mcfg.h"
#include "hg_blob.h"

/* ---- layout guard: field-table offsets and total size must track the struct ---- */
_Static_assert(offsetof(hg_mcfg_t, sta_ssid)  == 0,   "sta_ssid offset");
_Static_assert(offsetof(hg_mcfg_t, sta_pass)  == 33,  "sta_pass offset");
_Static_assert(offsetof(hg_mcfg_t, ap_ssid)   == 98,  "ap_ssid offset");
_Static_assert(offsetof(hg_mcfg_t, ap_pass)   == 131, "ap_pass offset");
_Static_assert(offsetof(hg_mcfg_t, web_salt)  == 196, "web_salt offset");
_Static_assert(offsetof(hg_mcfg_t, web_hash)  == 212, "web_hash offset");
_Static_assert(offsetof(hg_mcfg_t, tz)        == 244, "tz offset");
_Static_assert(offsetof(hg_mcfg_t, ntp)       == 292, "ntp offset");
_Static_assert(offsetof(hg_mcfg_t, hostname)  == 340, "hostname offset");
_Static_assert(offsetof(hg_mcfg_t, flags)     == 364, "flags offset");
_Static_assert(sizeof(hg_mcfg_t) == 368, "mcfg layout");

const char *const HG_MGROUP_NAMES[HG_MG_COUNT] = { "WIFI", "WEB", "TIME", "SYS" };

#define MF(g, k, off, t, lo, hi, e) { HG_MG_##g, k, (uint16_t)(off), HG_T_##t, lo, hi, e }
const hg_field_t HG_MFIELDS[] = {
    MF(WIFI, "STA_SSID", 0,   STR, 0, 32, NULL),
    MF(WIFI, "STA_PASS", 33,  STR, 0, 64, NULL),
    MF(WIFI, "AP_SSID",  98,  STR, 0, 32, NULL),
    MF(WIFI, "AP_PASS",  131, STR, 0, 64, NULL),
    MF(TIME, "TZ",       244, STR, 0, 47, NULL),
    MF(TIME, "NTP",      292, STR, 0, 47, NULL),
    MF(SYS,  "HOSTNAME", 340, STR, 0, 23, NULL),
};
#undef MF
const int HG_MFIELD_COUNT = (int)(sizeof HG_MFIELDS / sizeof HG_MFIELDS[0]);

void hg_mcfg_defaults(hg_mcfg_t *m) {
    memset(m, 0, sizeof *m);
    strcpy(m->ap_ssid, "HillGrow");
    strcpy(m->ap_pass, "hillgrow1");
    strcpy(m->tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    strcpy(m->ntp, "pool.ntp.org");
    strcpy(m->hostname, "hillgrow");
    m->flags = MCFG_F_WEB_DEFAULT | MCFG_F_AP_DEFAULT;
}

static int mfail(char *err, size_t errlen, hg_mgroup_t g, const char *key) {
    if (err && errlen) snprintf(err, errlen, "%s.%s", HG_MGROUP_NAMES[g], key);
    return -1;
}

int hg_mcfg_validate(const hg_mcfg_t *m, hg_tz_check_fn tzck, char *err, size_t errlen) {
    size_t n;

    n = strlen(m->sta_ssid);                                    /* empty or 1..32 */
    if (n > 32) return mfail(err, errlen, HG_MG_WIFI, "STA_SSID");

    n = strlen(m->sta_pass);                                    /* empty or 8..63 */
    if (n > 0 && (n < 8 || n > 63)) return mfail(err, errlen, HG_MG_WIFI, "STA_PASS");

    n = strlen(m->ap_ssid);                                     /* 1..32 */
    if (n < 1 || n > 32) return mfail(err, errlen, HG_MG_WIFI, "AP_SSID");

    n = strlen(m->ap_pass);                                     /* 8..63 */
    if (n < 8 || n > 63) return mfail(err, errlen, HG_MG_WIFI, "AP_PASS");

    if (tzck && tzck(m->tz) != 0) return mfail(err, errlen, HG_MG_TIME, "TZ");   /* NULL tzck: accept any TZ */

    n = strlen(m->ntp);                                         /* 1..47, no spaces */
    if (n < 1 || n > 47) return mfail(err, errlen, HG_MG_TIME, "NTP");
    for (size_t i = 0; i < n; i++)
        if (m->ntp[i] == ' ') return mfail(err, errlen, HG_MG_TIME, "NTP");

    n = strlen(m->hostname);                                    /* 1..23 of [a-z0-9-] */
    if (n < 1 || n > 23) return mfail(err, errlen, HG_MG_SYS, "HOSTNAME");
    for (size_t i = 0; i < n; i++) {
        char c = m->hostname[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return mfail(err, errlen, HG_MG_SYS, "HOSTNAME");
    }

    return 0;
}

size_t hg_mcfg_pack(const hg_mcfg_t *m, uint32_t gen, uint8_t *out, size_t cap) {
    return hg_blob_wrap(HG_MAGIC_MCFG, HG_MCFG_VER, gen, m, (uint16_t)sizeof *m, out, cap);
}

int hg_mcfg_unpack(const uint8_t *in, size_t n, hg_mcfg_t *m, uint32_t *gen) {
    hg_mcfg_t tmp;
    hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_MCFG, HG_MCFG_VER, HG_MCFG_VER_MIN,
                                      in, n, &tmp, (uint16_t)sizeof tmp, gen);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) return -1;   /* *m left untouched */
    *m = tmp;
    return 0;
}

int hg_mcfg_is_secret(const hg_field_t *f) {
    return f->group == HG_MG_WIFI &&
           (strcmp(f->key, "STA_PASS") == 0 || strcmp(f->key, "AP_PASS") == 0);
}
