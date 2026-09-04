#include <stdio.h>
#include "ring_proto.h"

#define LINK_UPSTREAM_ALIVE 0x01   /* hg_hb_t.link_flags b0, copied into hg_node_t.link_flags */

static const char *health_name(node_health_t h) {
    switch (h) {
        case NODE_H_ONLINE:   return "ONLINE";
        case NODE_H_DEGRADED: return "DEGRADED";
        case NODE_H_OFFLINE:  return "OFFLINE";
        case NODE_H_UPDATING: return "UPDATING";
        default:              return "EMPTY";
    }
}

/* Greatest used id strictly below k, or 0 (master) if none. */
static uint8_t prev_used_id(const hg_node_t *tab, int n_slots, uint8_t k) {
    uint8_t best = 0;
    for (int i = 0; i < n_slots; i++) {
        if (tab[i].used && tab[i].id < k && tab[i].id > best) best = tab[i].id;
    }
    return best;
}

static uint8_t highest_used_id(const hg_node_t *tab, int n_slots) {
    uint8_t best = 0;
    for (int i = 0; i < n_slots; i++) {
        if (tab[i].used && tab[i].id > best) best = tab[i].id;
    }
    return best;
}

/* The upstream neighbour of node k -- the node whose TX feeds k's RX -- named
 * from MEASURED hop order, because id order is not physical order (ids come
 * from enrolment, which for simultaneously-enrolling boards is arbitrary) and
 * the blame line exists to tell the operator which cable to touch.
 *
 * hops is stamped at the MASTER'S RX as RING_TTL_INIT - ttl, so it counts the
 * forwards a heartbeat took to get there: the zone that feeds the master's RX
 * arrives undecremented (hops 0) and the FIRST hop after the master's TX has
 * the HIGHEST hops. Walking upstream therefore walks hops UP -- the upstream
 * neighbour of a node with hops h is the node with hops h+1, and the master
 * itself when no such node exists (the max-hops node owns the M->Z leg).
 * hops survives silence (last known value, kept in RAM), so a dead node is
 * still placed correctly; id order is the fallback only for a node whose hops
 * have never been measured (hops_valid 0: enrolled from NVS, never heard). */
static uint8_t upstream_id(const hg_node_t *tab, int n_slots, const hg_node_t *k) {
    if (!k->hops_valid) return prev_used_id(tab, n_slots, k->id);
    uint8_t best = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used || !nd->hops_valid || nd->hops != (uint8_t)(k->hops + 1)) continue;
        if (best == 0 || nd->id < best) best = nd->id;   /* ties can't happen physically; be deterministic */
    }
    return best;   /* 0 = nothing upstream of the first hop but the master */
}

/* The node whose TX feeds the master's RX: the smallest measured hops, or the
 * highest used id when no hops have ever been measured (same fallback rule). */
static uint8_t last_hop_id(const hg_node_t *tab, int n_slots) {
    const hg_node_t *best = NULL;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used || !nd->hops_valid) continue;
        if (!best || nd->hops < best->hops || (nd->hops == best->hops && nd->id < best->id)) best = nd;
    }
    return best ? best->id : highest_used_id(tab, n_slots);
}

static void format_wire_blame(uint8_t k, uint8_t prev, char *out, size_t outsz) {
    if (prev == 0) snprintf(out, outsz, "Z%u dead or wire M->Z%u", (unsigned)k, (unsigned)k);
    else           snprintf(out, outsz, "Z%u dead or wire Z%u->Z%u", (unsigned)k, (unsigned)prev, (unsigned)k);
}

/* Priority order (spec §2.7): (1) smallest-hops FRESH node whose upstream_alive bit is clear;
   (2) else lowest-id SILENT (stale HB) used node; (3) else every zone is alive and reporting
   its upstream alive -> the break is the master's own RX leg, blamed on the last hop (the
   smallest-hops node). The suspect LEG is always named by measured hop order (upstream_id). */
static void ring_blame(const hg_node_t *tab, int n_slots, uint32_t now_ms, char *out, size_t outsz) {
    const hg_node_t *cand_a = NULL;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        if (now_ms - nd->last_hb_ms >= 5000) continue;             /* stale: link_flags not trustworthy */
        if (nd->link_flags & LINK_UPSTREAM_ALIVE) continue;
        if (!cand_a || nd->hops < cand_a->hops) cand_a = nd;
    }
    if (cand_a) {
        format_wire_blame(cand_a->id, upstream_id(tab, n_slots, cand_a), out, outsz);
        return;
    }

    const hg_node_t *cand_b = NULL;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        if (now_ms - nd->last_hb_ms < 5000) continue;
        if (!cand_b || nd->id < cand_b->id) cand_b = nd;
    }
    if (cand_b) {
        format_wire_blame(cand_b->id, upstream_id(tab, n_slots, cand_b), out, outsz);
        return;
    }

    uint8_t last = last_hop_id(tab, n_slots);
    snprintf(out, outsz, "Z%u dead or wire Z%u->M", (unsigned)last, (unsigned)last);
}

uint16_t ring_online_mask(const hg_node_t *tab, int n_slots, uint32_t now_ms) {
    uint16_t mask = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (nd->used && (now_ms - nd->last_hb_ms) < 5000) mask |= (uint16_t)(1u << nd->id);
    }
    return mask;
}

void ring_health_eval(hg_node_t *tab, int n_slots, uint32_t now_ms,
                      uint32_t ts_last_returned_ms, ring_status_t *st,
                      ring_health_ev_cb cb, void *ctx) {
    if (!tab || !st) return;

    uint8_t used_count = 0;
    for (int i = 0; i < n_slots; i++) {
        hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        used_count++;

        node_health_t prev = nd->health;
        node_health_t nh;

        if (now_ms < nd->updating_until_ms) {
            nh = NODE_H_UPDATING;                                  /* frozen: no HB alarms while updating */
        } else {
            uint32_t since_hb = now_ms - nd->last_hb_ms;
            if (since_hb >= 10000)                                  nh = NODE_H_OFFLINE;
            else if (since_hb >= 5000 || nd->cmd_timeouts >= 3)      nh = NODE_H_DEGRADED;
            else                                                    nh = NODE_H_ONLINE;
        }

        if (nh != prev) {
            nd->health = nh;
            /* first-ever observation (prev == EMPTY) is not a notify-worthy transition */
            if (cb && prev != NODE_H_EMPTY) {
                char line[32];
                snprintf(line, sizeof line, "NODE %u %s", (unsigned)nd->id, health_name(nh));
                cb(ctx, line);
            }
        }
    }

    st->size = used_count;
    st->online_mask = ring_online_mask(tab, n_slots, now_ms);

    if (used_count == 0) {
        st->state = RING_ST_IDLE;
        st->blame[0] = '\0';
        return;
    }

    uint32_t since_ret = now_ms - ts_last_returned_ms;
    if (since_ret >= 5000) {
        if (st->state != RING_ST_OPEN) {
            ring_blame(tab, n_slots, now_ms, st->blame, sizeof st->blame);
            if (cb) {
                char line[10 + sizeof st->blame];
                snprintf(line, sizeof line, "RING OPEN %s", st->blame);
                cb(ctx, line);
            }
        }
        st->state = RING_ST_OPEN;
    } else {
        if (st->state == RING_ST_OPEN) {
            if (cb) cb(ctx, "RING CLOSED");
            st->blame[0] = '\0';
        }
        st->state = RING_ST_OK;
    }
}
