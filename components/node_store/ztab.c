#include "node_store.h"
#include "hg_blob.h"
#include <string.h>

int ztab_find_mac(const ztab_t *t, const uint8_t mac[6]) {
    if (!t || !mac) return -1;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (memcmp(t->e[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int ztab_find_id(const ztab_t *t, uint8_t id) {
    if (!t || id == 0) return -1;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (t->e[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* Put mac in the first empty slot holding the given (already-verified free) id.
   -1 = no empty slot (table full). */
static int ztab_place(ztab_t *t, const uint8_t mac[6], uint8_t id) {
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (t->e[i].id != 0) continue;
        memcpy(t->e[i].mac, mac, 6);
        t->e[i].id = id;
        t->e[i].flags = ZTAB_F_ASSIGNED | ZTAB_F_UNCONFIGURED;
        memset(t->e[i].name, 0, 16);
        return i;
    }
    return -1;
}

int ztab_assign(ztab_t *t, const uint8_t mac[6]) {
    if (!t || !mac) return -1;

    /* Check if MAC already exists */
    int slot = ztab_find_mac(t, mac);
    if (slot >= 0) {
        return t->e[slot].id;
    }

    /* Find lowest free id 1..8 */
    for (uint8_t id = 1; id <= HG_MAX_ZONES; id++) {
        if (ztab_find_id(t, id) >= 0) continue;
        return ztab_place(t, mac, id) < 0 ? -1 : (int)id;
    }

    return -1;  /* Table full */
}

int ztab_set_name(ztab_t *t, uint8_t id, const char *name) {
    if (!t || !name) return -1;

    int slot = ztab_find_id(t, id);
    if (slot < 0) return -1;

    size_t len = strlen(name);
    if (len > 15) return -1;  /* Max 15 chars + NUL */

    memset(t->e[slot].name, 0, 16);
    memcpy(t->e[slot].name, name, len);
    return 0;
}

int ztab_clear(ztab_t *t, uint8_t id) {
    if (!t) return -1;

    int slot = ztab_find_id(t, id);
    if (slot < 0) return -1;

    memset(&t->e[slot], 0, sizeof(ztab_ent_t));
    return 0;
}

ztab_en_t ztab_enrol(ztab_t *t, const uint8_t mac[6], uint8_t claimed_id, uint8_t *out_id) {
    if (!t || !mac) return ZTAB_EN_FULL;

    /* Check if MAC is known */
    int mac_slot = ztab_find_mac(t, mac);

    if (mac_slot >= 0) {
        /* MAC is known */
        uint8_t assigned_id = t->e[mac_slot].id;
        if (out_id) *out_id = assigned_id;

        if (claimed_id == assigned_id) {
            return ZTAB_EN_KNOWN;
        } else {
            return ZTAB_EN_STALE;
        }
    }

    /* MAC is unknown */

    if (claimed_id >= 1 && claimed_id <= HG_MAX_ZONES) {
        int id_slot = ztab_find_id(t, claimed_id);
        if (id_slot >= 0) {
            /* This id is taken by someone else */
            if (out_id) *out_id = claimed_id;
            return ZTAB_EN_CONFLICT;
        }
        /* Claimed id is FREE: honour it. Id stability is the intent of spec
           §2.8 and of §4.4's "Master NVS loss self-heals" -- after a wiped
           master table every zone still knows its own id and re-claims it in
           its first heartbeat, so the fleet keeps its numbering (and its
           labels, cabling and operator habits) instead of being renumbered
           lowest-free, which also fired spurious ID_CONFLICTs when the
           renumbering collided with a zone that had not been heard yet.
           Lowest-free is for a zone that claims NOTHING (0xFE, or 0). */
        if (ztab_place(t, mac, claimed_id) < 0) return ZTAB_EN_FULL;
        if (out_id) *out_id = claimed_id;
        return ZTAB_EN_ASSIGNED;
    }

    /* Claims nothing: give it the lowest free id */
    int new_id = ztab_assign(t, mac);
    if (new_id < 0) {
        return ZTAB_EN_FULL;
    }

    if (out_id) *out_id = new_id;
    return ZTAB_EN_ASSIGNED;
}

int ztab_pack(const ztab_t *t, uint32_t gen, uint8_t *out, size_t cap) {
    if (!t || !out || cap < (HG_BLOB_HDR_LEN + sizeof(ztab_t))) return -1;

    size_t ret = hg_blob_wrap(HG_MAGIC_ZTAB, 1, gen, (const void *)t, sizeof(ztab_t), out, cap);
    if (ret == 0) return -1;

    return (int)ret;
}

int ztab_unpack(const uint8_t *in, size_t n, ztab_t *t, uint32_t *gen) {
    if (!in || !t || n < HG_BLOB_HDR_LEN) return -1;

    hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_ZTAB, 1, 1, in, n, (void *)t, sizeof(ztab_t), gen);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) {
        return -1;
    }

    return 0;
}
