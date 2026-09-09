#include <stdio.h>
#include <string.h>
#include "ring_proto.h"

/* hg_hb_t.link_flags b1, copied into hg_node_t.link_flags: a MASTER-SOURCED
 * frame arrived within 6000 ms. Its loss is exactly what makes the zone shout
 * W_LINK_LOST "master silent", and it is the only link bit blame trusts.
 *
 * b0 (upstream_alive, "any frame from the upstream leg") looks like the
 * natural break detector and was what the first implementation used, but in
 * all three 2026-09-09 wire pulls it stayed SET on the node whose upstream leg
 * had just been cut (why is not established -- the zone sets it from any frame
 * its receiver hands up), so the rule built on it never fired at all and blame
 * fell through to weaker branches that named the wrong cable. b1 is the flag
 * that demonstrably goes dark when master frames stop arriving. */
#define LINK_MASTER_ALIVE 0x02

static const char *health_name(node_health_t h) {
    switch (h) {
        case NODE_H_ONLINE:   return "ONLINE";
        case NODE_H_DEGRADED: return "DEGRADED";
        case NODE_H_OFFLINE:  return "OFFLINE";
        case NODE_H_UPDATING: return "UPDATING";
        default:              return "EMPTY";
    }
}

/* Position along the chain counted from the MASTER'S RX: 0 = the node feeding
 * that RX, larger = further upstream, toward the master's TX.
 *
 * node_mgr stamps hops = RING_TTL_INIT - ttl at the master's RX on every
 * heartbeat, so hops IS that position: the zone feeding the RX arrives
 * undecremented (0) and the FIRST hop after the master's TX has the highest.
 * It is kept in RAM and survives silence (last known value), so a node that
 * has just gone quiet is still placed correctly.
 *
 * A row that has never been heard from (hops_valid 0: enrolled from NVS) has
 * no measurement -- its hops field reads 0, indistinguishable from the real
 * last hop -- so its position is derived from id order instead, lowest used id
 * = first hop after the master's TX. That is the same fallback the leg naming
 * used before hops were measured, and it is only a fallback: enrolment order
 * is not physical order (boards enrolling together get arbitrary ids), which
 * is why measured hops win whenever they exist. */
static uint8_t chain_pos(const hg_node_t *tab, int n_slots, const hg_node_t *k) {
    if (k->hops_valid) return k->hops;
    uint8_t used = 0, lower = 0;
    for (int i = 0; i < n_slots; i++) {
        if (!tab[i].used) continue;
        used++;
        if (tab[i].id < k->id) lower++;
    }
    return used ? (uint8_t)(used - 1 - lower) : 0;
}

/* U: the most DOWNSTREAM offline node (smallest chain_pos), NULL = none. */
static const hg_node_t *most_downstream_offline(const hg_node_t *tab, int n_slots) {
    const hg_node_t *best = NULL;
    uint8_t best_pos = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used || nd->health != NODE_H_OFFLINE) continue;   /* UPDATING is not a fault */
        uint8_t pos = chain_pos(tab, n_slots, nd);
        /* equal positions cannot happen physically; order by id to stay deterministic */
        if (!best || pos < best_pos || (pos == best_pos && nd->id < best->id)) { best = nd; best_pos = pos; }
    }
    return best;
}

/* D: the most UPSTREAM node that is alive yet reports master-silent (largest
 * chain_pos), NULL = none. Only ONLINE/DEGRADED nodes qualify: an OFFLINE
 * node's link_flags are whatever it last managed to send, i.e. from before
 * the break, and an UPDATING node is expected to be dark. */
static const hg_node_t *most_upstream_master_silent(const hg_node_t *tab, int n_slots) {
    const hg_node_t *best = NULL;
    uint8_t best_pos = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        if (nd->health != NODE_H_ONLINE && nd->health != NODE_H_DEGRADED) continue;
        if (nd->link_flags & LINK_MASTER_ALIVE) continue;
        uint8_t pos = chain_pos(tab, n_slots, nd);
        if (!best || pos > best_pos || (pos == best_pos && nd->id < best->id)) { best = nd; best_pos = pos; }
    }
    return best;
}

/* A zone whose heartbeats have STOPPED arriving but which has not yet crossed
 * the 10000 ms OFFLINE line -- i.e. DEGRADED by heartbeat age, not by command
 * timeouts. It is a U in the making: 5 s from now it will be offline. */
static int hb_missing_unsettled(const hg_node_t *tab, int n_slots, uint32_t now_ms) {
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used) continue;
        if (nd->health == NODE_H_OFFLINE || nd->health == NODE_H_UPDATING) continue;
        if ((uint32_t)(now_ms - nd->last_hb_ms) >= 5000) return 1;
    }
    return 0;
}

/* Blame the segment between U and D (spec §2.7, bench ruling 2026-09-09).
 *
 * Physics of a single cut on the leg into node N: every node from N DOWNSTREAM
 * (toward the master's RX) stops hearing the master while staying alive -- its
 * heartbeats still reach the master, they only travel downstream -- and every
 * node UPSTREAM of the cut (between the master's TX and it) goes OFFLINE,
 * because its heartbeats cannot get past the cut, while it still hears the
 * master. So the break sits between the most-downstream OFFLINE node and the
 * most-upstream master-silent live node; the master itself takes the open end
 * when one of those sets is empty.
 *
 * A dead node is a cut on BOTH of its legs: it is offline AND everything
 * downstream of it is master-silent, so it comes out as U itself -- hence
 * "U dead or wire U->D" whenever U is a node. When U is the master no node is
 * offline, so nothing can be dead and the text names the cable alone.
 *
 * Naming a leg from hop counts and upstream_alive bits (the pre-fix rule) put
 * the operator on the wrong cable in all three of the 2026-09-09 wire pulls;
 * see docs/what_we_learned.md. */
static void ring_blame(const hg_node_t *tab, int n_slots, uint32_t now_ms, char *out, size_t outsz) {
    const hg_node_t *u = most_downstream_offline(tab, n_slots);
    const hg_node_t *d = most_upstream_master_silent(tab, n_slots);

    /* No U means the master's own TX leg is the upstream end of the break --
     * which asserts that every zone above D is still delivering heartbeats. A
     * zone whose heartbeats have just stopped falsifies that assertion and is
     * about to become U itself, so the cable question stays unanswered for the
     * few seconds the ladder needs. Nothing to report at all lands here too:
     * every zone alive and hearing the master, i.e. the master's own RX leg or
     * a zone that forwards without saying so. Either way, name no cable rather
     * than the wrong one -- naming the wrong one is the bug being fixed. */
    if (!u && (!d || hb_missing_unsettled(tab, n_slots, now_ms))) {
        snprintf(out, outsz, "ring open (no node reports a fault)");
        return;
    }

    char dn[8];
    if (d) snprintf(dn, sizeof dn, "Z%u", (unsigned)d->id);
    else   snprintf(dn, sizeof dn, "M");

    if (!u) snprintf(out, outsz, "wire M->%s", dn);
    else    snprintf(out, outsz, "Z%u dead or wire Z%u->%s", (unsigned)u->id, (unsigned)u->id, dn);
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
        /* Blame is re-derived on every tick the ring is open, not latched at
         * the edge. At the 5000 ms mark the evidence does not exist yet: the
         * ladder has only just reached DEGRADED, and a starved zone needs
         * 6000 ms of master silence plus one heartbeat to report the loss --
         * so the first verdict is usually "no node reports a fault", and the
         * segment appears a few seconds later when the nodes above the cut go
         * OFFLINE (10 s) and the ones below it say they cannot hear the
         * master. A CHANGED verdict is worth a new line; an unchanged one is
         * not, so a stable break still notifies exactly once.
         * The health ladder above has just run, so ring_blame reads each
         * node's CURRENT health -- no second staleness rule. */
        char blame[sizeof st->blame];
        ring_blame(tab, n_slots, now_ms, blame, sizeof blame);
        if (st->state != RING_ST_OPEN || strcmp(blame, st->blame) != 0) {
            memcpy(st->blame, blame, sizeof blame);
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
