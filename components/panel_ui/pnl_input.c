/* pnl_input.c -- keyboard character classes, text bounds and the MAC rule (pure). */
#include <string.h>
#include "pnl_input.h"

int pnl_kb_accepts(pcfg_kb_t kb, char c) {
    unsigned char u = (unsigned char)c;
    switch (kb) {
    case PCFG_KB_NUMERIC:      return u >= '0' && u <= '9';
    case PCFG_KB_TEXT:         return u >= 0x20 && u <= 0x7E;
    case PCFG_KB_TEXT_NOSPACE: return u >= 0x21 && u <= 0x7E;
    case PCFG_KB_HOSTNAME:     return (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-';
    case PCFG_KB_HEX:          return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F') || u == ':';
    default:                   return 0;
    }
}

int pnl_text_ok(pcfg_kb_t kb, const char *s, uint8_t min_len, uint8_t max_len) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < (size_t)min_len || n > (size_t)max_len) return 0;
    for (size_t i = 0; i < n; i++) if (!pnl_kb_accepts(kb, s[i])) return 0;
    return 1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pnl_mac_parse(const char *s, uint8_t mac[6]) {
    uint8_t out[6];
    if (!s || !mac || strlen(s) != 17) return -1;
    for (int i = 0; i < 6; i++) {
        int hi = hexv(s[3 * i]), lo = hexv(s[3 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        if (i < 5 && s[3 * i + 2] != ':') return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(mac, out, 6);
    return 0;
}
