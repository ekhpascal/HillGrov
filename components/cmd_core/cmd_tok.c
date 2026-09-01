#include <stdlib.h>
#include <string.h>
#include "cmd_core.h"

int cmd_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

int cmd_tokenize(char *line, const char *tok[], int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return n;
        if (n == max) return -1;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
}

int cmd_parse_time(const char *s) {
    const char *colon = strchr(s, ':');
    char *end;
    if (!colon) {
        long v = strtol(s, &end, 10);
        if (end == s || *end || v < 0 || v > 1439) return -1;
        return (int)v;
    }
    if (colon == s || strlen(colon + 1) != 2) return -1;
    long h = strtol(s, &end, 10);
    if (end != colon || h < 0 || h > 23) return -1;
    long m = strtol(colon + 1, &end, 10);
    if (*end || m < 0 || m > 59) return -1;
    return (int)(h * 60 + m);
}
