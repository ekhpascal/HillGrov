#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hg_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HG_MAGIC_MCFG 0x46434D48u   /* 'HMCF' LE */
#define HG_MCFG_VER      1
#define HG_MCFG_VER_MIN  1

#define MCFG_F_WEB_DEFAULT 0x01     /* web password still the default -> web_auth compares against "hillgrow1" */
#define MCFG_F_AP_DEFAULT  0x02

typedef struct {                    /* 368 B, explicit offsets */
    char    sta_ssid[33];           /* 0   */
    char    sta_pass[65];           /* 33  */
    char    ap_ssid[33];            /* 98  */
    char    ap_pass[65];            /* 131 */
    uint8_t web_salt[16];           /* 196 */
    uint8_t web_hash[32];           /* 212 */
    char    tz[48];                 /* 244 POSIX TZ string */
    char    ntp[48];                /* 292 */
    char    hostname[24];           /* 340 */
    uint8_t flags;                  /* 364 MCFG_F_* */
    uint8_t rsvd[3];                /* 365 */
} hg_mcfg_t;

typedef enum { HG_MG_WIFI = 0, HG_MG_WEB, HG_MG_TIME, HG_MG_SYS, HG_MG_COUNT } hg_mgroup_t;

extern const hg_field_t  HG_MFIELDS[];       /* group = hg_mgroup_t; keys: STA_SSID STA_PASS AP_SSID AP_PASS | (WEB none) | TZ NTP | HOSTNAME */
extern const int         HG_MFIELD_COUNT;
extern const char *const HG_MGROUP_NAMES[HG_MG_COUNT];   /* "WIFI","WEB","TIME","SYS" */

void   hg_mcfg_defaults(hg_mcfg_t *m);       /* ap HillGrow/hillgrow1, tz "CET-1CEST,M3.5.0,M10.5.0/3", ntp "pool.ntp.org", hostname "hillgrow", flags = WEB_DEFAULT|AP_DEFAULT */

typedef int (*hg_tz_check_fn)(const char *tz);   /* 0 ok / -1 (time_core's tz_parse in production) */

/* tzck == NULL accepts any tz string (used before the real checker is wired up). */
int    hg_mcfg_validate(const hg_mcfg_t *m, hg_tz_check_fn tzck, char *err, size_t errlen);  /* 0 ok / -1, err = "GROUP.KEY" */
size_t hg_mcfg_pack(const hg_mcfg_t *m, uint32_t gen, uint8_t *out, size_t cap);              /* envelope; 0 = cap too small */
int    hg_mcfg_unpack(const uint8_t *in, size_t n, hg_mcfg_t *m, uint32_t *gen);               /* 0 ok / -1 (defaults untouched) */
int    hg_mcfg_is_secret(const hg_field_t *f);   /* 1 for STA_PASS / AP_PASS */

#ifdef __cplusplus
}
#endif
