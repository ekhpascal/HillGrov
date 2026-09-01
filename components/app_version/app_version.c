#include "app_version.h"

static int parse_part(const char **p, uint8_t *out) {
    const char *s = *p;
    if (*s < '0' || *s > '9') return -1;
    unsigned v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        if (v > 255u || ++n > 3) return -1;
        s++;
    }
    *out = (uint8_t)v;
    *p = s;
    return 0;
}

int app_version_parse(const char *s, app_version_t *out) {
    if (!s || !out) return -1;
    app_version_t v;
    if (parse_part(&s, &v.major) != 0 || *s++ != '.') return -1;
    if (parse_part(&s, &v.minor) != 0 || *s++ != '.') return -1;
    if (parse_part(&s, &v.patch) != 0 || *s != '\0') return -1;
    *out = v;
    return 0;
}

int app_version_cmp(const app_version_t *a, const app_version_t *b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}
