#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t major, minor, patch; } app_version_t;

/* Strict "M.m.p", each 0..255, no leading/trailing junk. 0 ok, -1 bad. */
int app_version_parse(const char *s, app_version_t *out);
/* -1 a<b, 0 equal, +1 a>b */
int app_version_cmp(const app_version_t *a, const app_version_t *b);

#ifdef __cplusplus
}
#endif
