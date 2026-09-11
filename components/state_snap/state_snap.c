#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include "state_snap.h"

#define SS_SCRATCH 1024   /* every w() call carries at most this many bytes */

/* ---- tiny bounds-checked JSON builders, all sharing the same (buf, cap, *off)
 * shape: each returns 0 on success, -1 if the segment would not fit -- and
 * is safe to call again after a -1 (it re-checks *off against cap and bails
 * without writing out of bounds; the caller is expected to stop on -1). */

static int raw(char *buf, size_t cap, size_t *off, const char *s) {
    size_t n = strlen(s);
    if (*off + n >= cap) { *off += n; return -1; }
    memcpy(buf + *off, s, n);
    *off += n;
    return 0;
}

static int fmt(char *buf, size_t cap, size_t *off, const char *format, ...) {
    if (*off >= cap) return -1;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf + *off, cap - *off, format, ap);
    va_end(ap);
    if (n < 0) return -1;
    *off += (size_t)n;
    return (*off < cap) ? 0 : -1;
}

/* Quoted, escaped JSON string: '"' -> \", '\\' -> \\, control chars (<0x20)
 * -> \u00XX, everything else verbatim (operator-supplied names are the
 * only free-text input; §task-10 brief). */
static int jstr(char *buf, size_t cap, size_t *off, const char *s) {
    if (*off >= cap) return -1;
    buf[(*off)++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char rep[8];
        int  rl;
        if (c == '"' || c == '\\') { rep[0] = '\\'; rep[1] = (char)c; rl = 2; }
        else if (c < 0x20)         { rl = snprintf(rep, sizeof rep, "\\u%04x", c); }
        else                       { rep[0] = (char)c; rl = 1; }
        if (*off + (size_t)rl >= cap) { *off += (size_t)rl; return -1; }
        memcpy(buf + *off, rep, (size_t)rl);
        *off += (size_t)rl;
    }
    if (*off >= cap) return -1;
    buf[(*off)++] = '"';
    return (*off < cap) ? 0 : -1;
}

static const char *health_name(node_health_t h) {   /* mirrors master_cmds.c's health_name() */
    switch (h) {
    case NODE_H_ONLINE:   return "ONLINE";
    case NODE_H_DEGRADED: return "DEGRADED";
    case NODE_H_OFFLINE:  return "OFFLINE";
    case NODE_H_UPDATING: return "UPDATING";
    default:              return "EMPTY";
    }
}

static int ss_master(char *buf, size_t cap, size_t *off, const snap_master_t *m) {
    if (raw(buf, cap, off, "{")) return -1;
    if (raw(buf, cap, off, "\"version\":") || jstr(buf, cap, off, m->version ? m->version : "")) return -1;
    if (fmt(buf, cap, off, ",\"uptime_s\":%u,\"heap_min_kb\":%u,",
            (unsigned)m->uptime_s, (unsigned)m->heap_min_kb)) return -1;
    if (raw(buf, cap, off, "\"time\":") || jstr(buf, cap, off, m->time)) return -1;
    if (raw(buf, cap, off, ",\"time_src\":") || jstr(buf, cap, off, m->time_src ? m->time_src : "")) return -1;
    if (raw(buf, cap, off, ",\"wifi\":{\"sta\":{")) return -1;
    if (fmt(buf, cap, off, "\"up\":%s,", m->sta.up ? "true" : "false")) return -1;
    if (raw(buf, cap, off, "\"ip\":") || jstr(buf, cap, off, m->sta.ip)) return -1;
    if (raw(buf, cap, off, ",\"ssid\":") || jstr(buf, cap, off, m->sta.ssid)) return -1;
    if (fmt(buf, cap, off, ",\"rssi\":%d,", (int)m->sta.rssi)) return -1;
    if (raw(buf, cap, off, "\"reason\":") || jstr(buf, cap, off, m->sta.reason)) return -1;
    if (raw(buf, cap, off, "},\"ap\":{")) return -1;
    if (raw(buf, cap, off, "\"ssid\":") || jstr(buf, cap, off, m->ap.ssid)) return -1;
    if (fmt(buf, cap, off, ",\"clients\":%u,", (unsigned)m->ap.clients)) return -1;
    if (raw(buf, cap, off, "\"ip\":") || jstr(buf, cap, off, m->ap.ip)) return -1;
    if (raw(buf, cap, off, "}},\"fw\":{")) return -1;
    if (raw(buf, cap, off, "\"slot\":") || jstr(buf, cap, off, m->fw.slot ? m->fw.slot : "")) return -1;
    if (raw(buf, cap, off, ",\"state\":") || jstr(buf, cap, off, m->fw.state ? m->fw.state : "")) return -1;
    if (raw(buf, cap, off, ",\"other\":") || jstr(buf, cap, off, m->fw.other ? m->fw.other : "")) return -1;
    if (raw(buf, cap, off, ",\"upload_kind\":") || jstr(buf, cap, off, m->fw.upload_kind ? m->fw.upload_kind : "")) return -1;
    if (fmt(buf, cap, off, ",\"upload_pct\":%u},", (unsigned)m->fw.upload_pct)) return -1;
    if (raw(buf, cap, off, "\"fleet\":") || jstr(buf, cap, off, m->fleet_line ? m->fleet_line : "")) return -1;
    if (fmt(buf, cap, off, ",\"alarms\":{\"active\":%d,\"total\":%d},",
            m->alarms_active, m->alarms_total)) return -1;
    if (fmt(buf, cap, off, "\"defaults\":{\"web\":%s,\"ap\":%s}}",
            m->web_default ? "true" : "false", m->ap_default ? "true" : "false")) return -1;
    return 0;
}

static int ss_node(char *buf, size_t cap, size_t *off, const hg_node_t *nd, uint32_t now_ms,
                    const uint8_t *cfg_sync_failed) {
    char mac[18];
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
             nd->mac[0], nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
    char fw[16];
    snprintf(fw, sizeof fw, "%u.%u.%u", nd->hb.fw_maj, nd->hb.fw_min, nd->hb.fw_patch);
    char faults[20];
    snprintf(faults, sizeof faults, "0x%" PRIx64, (uint64_t)nd->hb.active_faults);

    int stale  = (nd->health == NODE_H_OFFLINE);
    int failed = cfg_sync_failed && nd->id >= 1 && nd->id <= HG_MAX_ZONES && cfg_sync_failed[nd->id - 1];
    /* Plain unsigned subtraction (ring_health.c's own convention), NOT a
     * "now_ms >= last_hb_ms" guard: last_hb_ms/now_ms are millisecond
     * counters that wrap at 2^32 (~49.7 days), and modular arithmetic
     * already gives the right answer across that wrap -- a guard that
     * falls back to 0 instead would make a long-dead node (last heard
     * before the wrap) look "just heard" the moment now_ms wraps past it.
     * A never-heard row (last_hb_ms == 0, e.g. loaded fresh from NVS) has
     * no real "last heard" instant to measure from, so this reports the
     * plain result (now_ms/1000) rather than a sentinel -- see
     * test_hb_age_s_never_heard_row_is_plain_now_ms. */
    uint32_t age = (uint32_t)(now_ms - nd->last_hb_ms) / 1000;

    if (raw(buf, cap, off, "{")) return -1;
    if (fmt(buf, cap, off, "\"id\":%u,", (unsigned)nd->id)) return -1;
    if (raw(buf, cap, off, "\"name\":") || jstr(buf, cap, off, nd->name)) return -1;
    if (raw(buf, cap, off, ",\"mac\":") || jstr(buf, cap, off, mac)) return -1;
    if (raw(buf, cap, off, ",\"health\":") || jstr(buf, cap, off, health_name(nd->health))) return -1;
    if (raw(buf, cap, off, ",\"fw\":") || jstr(buf, cap, off, fw)) return -1;
    if (fmt(buf, cap, off, ",\"gen\":%u,\"hops\":%u,\"link\":%u,\"link_stale\":%s,",
            (unsigned)nd->hb.cfg_gen, (unsigned)nd->hops, (unsigned)nd->link_flags,
            stale ? "true" : "false")) return -1;
    if (raw(buf, cap, off, "\"cfg_sync\":") || jstr(buf, cap, off, failed ? "FAILED" : "OK")) return -1;
    if (fmt(buf, cap, off, ",\"hb_age_s\":%u,\"uptime_s\":%u,\"heap_kb\":%u,\"reset\":%u,",
            (unsigned)age, (unsigned)nd->hb.uptime_s, (unsigned)nd->hb.min_free_heap_kb,
            (unsigned)nd->hb.reset_reason)) return -1;
    if (raw(buf, cap, off, "\"faults\":") || jstr(buf, cap, off, faults)) return -1;
    if (fmt(buf, cap, off, ",\"mode\":%u,\"shelves\":[", (unsigned)nd->hb.mode)) return -1;

    int ns = nd->hb.n_shelves;   /* uint8_t, so never negative -- only the upper bound needs clamping */
    if (ns > 4) ns = 4;
    for (int i = 0; i < ns; i++) {
        const hg_hb_shelf_t *s = &nd->hb.shelf[i];
        if (fmt(buf, cap, off, "%s{\"pct_a\":%u,\"pct_b\":%u,\"white\":%u,\"red\":%u,\"out\":%u,\"pump_s\":%u}",
                i ? "," : "", (unsigned)s->pct_a, (unsigned)s->pct_b, (unsigned)s->white, (unsigned)s->red,
                (unsigned)s->out_flags, (unsigned)s->pump_today_s)) return -1;
    }
    if (raw(buf, cap, off, "]}")) return -1;
    return 0;
}

static int ss_ring(char *buf, size_t cap, size_t *off, const ring_status_t *rs) {
    const char *state = rs->state == RING_ST_OPEN ? "OPEN" : rs->state == RING_ST_OK ? "OK" : "IDLE";
    if (raw(buf, cap, off, "{")) return -1;
    if (raw(buf, cap, off, "\"state\":") || jstr(buf, cap, off, state)) return -1;
    if (fmt(buf, cap, off, ",\"size\":%u,\"online\":%u,",
            (unsigned)rs->size, (unsigned)rs->online_mask)) return -1;
    if (raw(buf, cap, off, "\"blame\":") || jstr(buf, cap, off, rs->blame)) return -1;
    if (raw(buf, cap, off, "}")) return -1;
    return 0;
}

int state_snap_write(const snap_master_t *m, const hg_node_t *tab, int n_slots, const ring_status_t *rs,
                     const uint8_t *cfg_sync_failed, uint32_t now_ms, snap_write_fn w, void *ctx) {
    char buf[SS_SCRATCH];
    size_t off = 0;

    if (raw(buf, sizeof buf, &off, "{\"master\":")) return -1;
    if (ss_master(buf, sizeof buf, &off, m)) return -1;
    if (raw(buf, sizeof buf, &off, ",\"nodes\":[")) return -1;
    if (w(ctx, buf, off) != 0) return -1;

    int first = 1;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        off = 0;
        if (!first && raw(buf, sizeof buf, &off, ",")) return -1;
        if (ss_node(buf, sizeof buf, &off, nd, now_ms, cfg_sync_failed)) return -1;
        if (w(ctx, buf, off) != 0) return -1;
        first = 0;
    }

    off = 0;
    if (raw(buf, sizeof buf, &off, "],\"ring\":")) return -1;
    if (ss_ring(buf, sizeof buf, &off, rs)) return -1;
    if (raw(buf, sizeof buf, &off, "}")) return -1;
    if (w(ctx, buf, off) != 0) return -1;

    return 0;
}
