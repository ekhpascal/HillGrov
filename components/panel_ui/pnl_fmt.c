#include <stdio.h>
#include <string.h>
#include "pnl_time.h"
#include "pnl_fmt.h"

/* Copies at most n bytes of src (stopping at NUL) into dst[cap], replacing
 * anything outside printable ASCII with '?'. */
static void ascii_copy(char *dst, size_t cap, const char *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[o] = '\0';
}

void pnl_zone_name(const hg_node_t *n, char out[17]) {
    if (n->name[0]) ascii_copy(out, 17, n->name, sizeof n->name);
    else snprintf(out, 17, "Z%u", (unsigned)n->id);
}

int pnl_fmt_age(uint32_t now_s, uint32_t stamp_s, char *out, size_t cap) {
    unsigned s = (unsigned)(now_s >= stamp_s ? now_s - stamp_s : 0u);
    if (s < 60u)    return snprintf(out, cap, "%us ago", s);
    if (s < 3600u)  return snprintf(out, cap, "%um %us ago", s / 60u, s % 60u);
    if (s < 86400u) return snprintf(out, cap, "%uh %um ago", s / 3600u, (s % 3600u) / 60u);
    return snprintf(out, cap, "%ud %uh ago", s / 86400u, (s % 86400u) / 3600u);
}

int pnl_fmt_sta(const wifi_status_t *w, char *out, size_t cap) {
    if (w->sta_up) {
        char ip[16], ssid[33];
        ascii_copy(ip, sizeof ip, w->sta_ip, sizeof w->sta_ip);
        ascii_copy(ssid, sizeof ssid, w->sta_ssid, sizeof w->sta_ssid);
        return snprintf(out, cap, "%s | %s | %d dBm", ip, ssid, (int)w->rssi);
    }
    char reason[24];
    ascii_copy(reason, sizeof reason, w->sta_reason, sizeof w->sta_reason);
    return snprintf(out, cap, "STA down: %s", reason[0] ? reason : "--");
}

int pnl_fmt_ap(const wifi_status_t *w, char *out, size_t cap) {
    char ssid[33], ip[16];
    ascii_copy(ssid, sizeof ssid, w->ap_ssid, sizeof w->ap_ssid);
    ascii_copy(ip, sizeof ip, w->ap_ip, sizeof w->ap_ip);
    return snprintf(out, cap, "%s | %u client(s) | %s", ssid, (unsigned)w->ap_clients, ip);
}

int pnl_fmt_master_time(const char *utc19, const char *src, int32_t offset_s, int is_set, char *out, size_t cap) {
    char s[8];
    ascii_copy(s, sizeof s, src ? src : "", 8);
    if (!is_set) return snprintf(out, cap, "Clock not set (%s)", s[0] ? s : "NONE");
    int y, mo, d, h, mi, se;
    if (!utc19 || sscanf(utc19, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) {
        char raw[20];
        ascii_copy(raw, sizeof raw, utc19 ? utc19 : "", 19);
        return snprintf(out, cap, "%s %s", raw, s);
    }
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, se), offset_s, 1, &t);
    int32_t a = offset_s < 0 ? -offset_s : offset_s;
    return snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d (UTC%c%02d:%02d) %s",
                    t.year, t.mon, t.mday, t.hour, t.min, t.sec,
                    offset_s < 0 ? '-' : '+', (int)(a / 3600), (int)((a % 3600) / 60), s);
}

void pnl_node_readings(const hg_node_t *n, pnl_readings_t *out) {
    memset(out, 0, sizeof *out);
    int ns = n->hb.n_shelves > 4 ? 4 : n->hb.n_shelves;
    if (ns <= 0) return;
    int soil = 0, light = 0, pump = 0, any = 0;
    for (int i = 0; i < ns; i++) {
        const hg_hb_shelf_t *s = &n->hb.shelf[i];
        if (s->pct_a || s->pct_b || s->white || s->red || s->pump_today_s) any = 1;
        soil  += s->pct_a + s->pct_b;
        light += s->white + s->red;
        pump  += s->pump_today_s;
    }
    out->any = (uint8_t)any;
    out->soil_pct  = (soil + ns) / (2 * ns);    /* Math.round(sum / (2 * ns)), half up */
    out->light_pct = (light + ns) / (2 * ns);
    out->pump_s    = pump;
}

int pnl_fmt_reading(const pnl_readings_t *r, int which, char *out, size_t cap) {
    if (!r || !r->any) return snprintf(out, cap, "--");
    switch (which) {
    case 0:  return snprintf(out, cap, "%d%%", r->soil_pct);
    case 1:  return snprintf(out, cap, "%d%%", r->light_pct);
    case 2:  return snprintf(out, cap, "%ds", r->pump_s);
    default: return snprintf(out, cap, "--");
    }
}
