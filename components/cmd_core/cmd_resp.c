#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cmd_core.h"

int cmd_err(char *resp, int len, const char *token) {
    snprintf(resp, (size_t)len, "ERR %s\n", token);
    return -1;
}

int cmd_okf(char *resp, int len, const char *fmt, ...) {
    va_list ap;
    int off = snprintf(resp, (size_t)len, "OK ");
    va_start(ap, fmt);
    off += vsnprintf(resp + off, (size_t)(len - off), fmt, ap);
    va_end(ap);
    if (off < len - 1) { resp[off] = '\n'; resp[off + 1] = '\0'; }
    else { resp[len - 2] = '\n'; resp[len - 1] = '\0'; }
    return 0;
}

int cmd_linef(char *resp, int len, const char *fmt, ...) {
    va_list ap;
    size_t off = strlen(resp);
    if ((int)off >= len - 2) return -1;
    va_start(ap, fmt);
    int w = vsnprintf(resp + off, (size_t)(len - (int)off), fmt, ap);
    va_end(ap);
    off += (size_t)(w > 0 ? w : 0);
    if ((int)off < len - 1) { resp[off] = '\n'; resp[off + 1] = '\0'; return 0; }
    resp[len - 2] = '\n'; resp[len - 1] = '\0';
    return -1;
}
