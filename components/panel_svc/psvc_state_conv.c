#include <stdio.h>
#include <string.h>
#include "psvc_state.h"

/* Pure (host-tested): no IDF headers. */

void psvc_parse_time_noted(const char *noted, char time_out[20], char src_out[8]) {
    time_out[0] = '\0';
    src_out[0] = '\0';
    if (!noted) return;
    size_t tlen = strlen(noted);
    size_t tcopy = tlen < 19 ? tlen : 19;   /* bounded memcpy, not snprintf -- h_state's format-truncation lesson */
    memcpy(time_out, noted, tcopy);
    time_out[tcopy] = '\0';
    if (tlen >= 19) (void)sscanf(noted + 19, " %7s", src_out);
}

void psvc_parse_fw_info(const char *info, char slot[16], char state[16], char other[16]) {
    slot[0] = state[0] = other[0] = '\0';
    if (!info) return;
    (void)sscanf(info, "%*s %15s %15s %15s", slot, state, other);
}

void psvc_state_to_snap(const psvc_state_t *s, snap_master_t *m) {
    memset(m, 0, sizeof *m);
    m->version     = s->version;
    m->uptime_s    = s->uptime_s;
    m->heap_min_kb = s->heap_min_kb;
    memcpy(m->time, s->time, sizeof m->time);
    m->time[sizeof m->time - 1] = '\0';
    m->time_src = s->time_src;
    m->sta.up = s->wifi.sta_up;
    snprintf(m->sta.ip, sizeof m->sta.ip, "%s", s->wifi.sta_ip);
    snprintf(m->sta.ssid, sizeof m->sta.ssid, "%s", s->wifi.sta_ssid);
    m->sta.rssi = s->wifi.rssi;
    snprintf(m->sta.reason, sizeof m->sta.reason, "%s", s->wifi.sta_reason);
    snprintf(m->ap.ssid, sizeof m->ap.ssid, "%s", s->wifi.ap_ssid);
    m->ap.clients = s->wifi.ap_clients;
    snprintf(m->ap.ip, sizeof m->ap.ip, "%s", s->wifi.ap_ip);
    m->fw.slot        = s->fw_slot;
    m->fw.state       = s->fw_state;
    m->fw.other       = s->fw_other;
    m->fw.upload_kind = s->upload_kind;
    m->fw.upload_pct  = s->upload_pct;
    m->fleet_line     = s->fleet_line;
    m->alarms_active  = s->alarms_active;
    m->alarms_total   = s->alarms_total;
    m->web_default    = s->web_default;
    m->ap_default     = s->ap_default;
    m->cmd_quarantined = s->web_cmd_quarantined;
}
