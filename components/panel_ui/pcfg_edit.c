#include <string.h>
#include "pcfg_edit.h"
#include "hg_mcfg.h"
#include "hg_wipe.h"   /* an edit set may hold a pending password: never a plain memset */

static int norm_idx(const pcfg_edits_t *e, uint8_t group, int idx) {
    if (e->table == PCFG_TABLE_MASTER) return -1;
    return hg_group_scope(group) == 0 ? -1 : idx;
}
static int find(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
    for (int i = 0; i < e->n; i++)
        if (e->d[i].f == f && e->d[i].group == group && e->d[i].idx == idx) return i;
    return -1;
}
static int blank_secret(const pcfg_edits_t *e, const psvc_fedit_t *d) {
    return e->table == PCFG_TABLE_MASTER && hg_mcfg_is_secret(d->f) && d->text[0] == '\0';
}

void pcfg_edits_reset(pcfg_edits_t *e, pcfg_table_t t, uint8_t zone) {
    if (!e) return;
    hg_wipe(e, sizeof *e);
    e->table = t;
    e->zone = zone;
}

int pcfg_edits_set(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f, const char *text) {
    if (!e || !f || !text) return -1;
    if (e->table == PCFG_TABLE_ZONE && hg_group_is_hw(group)) return -2;
    size_t len = strlen(text);
    if (len >= PSVC_FEDIT_TEXT_MAX) return -1;
    idx = norm_idx(e, group, idx);
    if (idx < -1 || idx > 127) return -1;
    int i = find(e, group, idx, f);
    if (i < 0) {
        if (e->n >= PCFG_EDIT_MAX) return -1;
        i = e->n++;
        e->d[i].group = group;
        e->d[i].idx = (int8_t)idx;
        e->d[i].f = f;
    }
    hg_wipe(e->d[i].text, sizeof e->d[i].text);
    memcpy(e->d[i].text, text, len);
    return 0;
}

int pcfg_edits_drop(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
    if (!e || !f) return -1;
    int i = find(e, group, norm_idx(e, group, idx), f);
    if (i < 0) return -1;
    memmove(&e->d[i], &e->d[i + 1], (size_t)(e->n - i - 1) * sizeof e->d[0]);
    e->n--;
    hg_wipe(&e->d[e->n], sizeof e->d[0]);
    return 0;
}

const psvc_fedit_t *pcfg_edits_get(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
    if (!e || !f) return NULL;
    int i = find(e, group, norm_idx(e, group, idx), f);
    return i < 0 ? NULL : &e->d[i];
}

int pcfg_edits_export(const pcfg_edits_t *e, psvc_fedit_t *out, int cap) {
    if (!e) return 0;
    int need = 0;
    for (int i = 0; i < e->n; i++) if (!blank_secret(e, &e->d[i])) need++;
    if (need == 0) return 0;
    if (!out || cap < need) return -1;
    int n = 0;
    for (int i = 0; i < e->n; i++) if (!blank_secret(e, &e->d[i])) out[n++] = e->d[i];
    return n;
}

void pcfg_edits_wipe(pcfg_edits_t *e) {
    if (!e) return;
    pcfg_table_t t = e->table;
    uint8_t z = e->zone;
    hg_wipe(e, sizeof *e);
    e->table = t;
    e->zone = z;
}

int pcfg_edits_drop_saved(pcfg_edits_t *e, const psvc_fedit_t *saved, int n) {
    if (!e || !saved) return 0;
    int dropped = 0;
    for (int i = 0; i < n; i++) {
        const psvc_fedit_t *cur = pcfg_edits_get(e, saved[i].group, saved[i].idx, saved[i].f);
        if (cur && strcmp(cur->text, saved[i].text) == 0 &&
            pcfg_edits_drop(e, saved[i].group, saved[i].idx, saved[i].f) == 0) dropped++;
    }
    return dropped;
}
