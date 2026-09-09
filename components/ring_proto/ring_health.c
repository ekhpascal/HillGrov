#include <stdio.h>
#include <string.h>
#include "ring_proto.h"

/* hg_hb_t.link_flags b1, copied into hg_node_t.link_flags: a MASTER-SOURCED
 * frame arrived within 6000 ms. Its loss is exactly what makes the zone shout
 * W_LINK_LOST "master silent", and it is the only link bit blame reads.
 *
 * Why the 2026-09-09 wire pulls named the wrong cable: not b0 (b0 was never
 * consulted -- see below), but (1) blame was computed once, at the OK->OPEN
 * edge, which lands one second BEFORE the zones' 6000 ms link timers can
 * report master-silent, so the master-silent branch was always empty and the
 * verdict fell through to weaker ones; and (2) those weaker branches picked
 * the smallest-hops silent node and named its UPSTREAM leg -- the wrong
 * direction, since a cut starves the nodes DOWNSTREAM of it.
 *
 * b0 (upstream_alive) is now stamped by ring_link for every validated arrival,
 * forwards included (ring_link_last_rx_ms), so it finally means "this zone's
 * receiver is getting traffic" rather than repeating b1's stimulus from the
 * consume queue. Blame does not use it yet: once trusted on the bench it could
 * shorten detection of "which node's own upstream leg is cut" -- a node whose
 * b0 goes dark while its neighbours' stay lit is the node just below the
 * break, evidence that arrives 6 s before the 10 s OFFLINE ladder does. */
#define LINK_MASTER_ALIVE 0x02

/* A zone needs to have been up this long before its link flags mean anything:
 * its own 6000 ms master-alive timer has to have had a chance to arm, and a
 * freshly booted zone legitimately reports "no master seen yet". A row the
 * master has never heard from also reads 0 here, so this covers both. */
#define WITNESS_MIN_UPTIME_S 10u

/* Consecutive 1 s health ticks a CHANGED verdict must survive before it is
 * adopted and announced. The OK->OPEN edge is never delayed by it. */
#define BLAME_DWELL_TICKS 3

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
 * is why measured hops win whenever they exist. In practice that fallback is
 * now unreachable from blame -- node_mgr stamps hops_valid with every
 * heartbeat, so an unmeasured row is one that has never been heard, and such a
 * row is neither a U candidate (phantom) nor a witness (boot grace) -- but it
 * keeps the ordering total for any caller that reaches it. */
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

/* U: the most DOWNSTREAM offline node (smallest chain_pos), NULL = none.
 *
 * A row the master has NEVER heard from reads OFFLINE the moment the ladder
 * runs, but it has no place on the chain and no cable of its own to accuse --
 * it is a phantom, not a suspect, and blaming it would point at a segment that
 * does not exist. hops_valid is stamped by every heartbeat, so its absence is
 * exactly "never heard": an id enrolled from NVS whose board is not on the
 * ring. */
static const hg_node_t *most_downstream_offline(const hg_node_t *tab, int n_slots) {
    const hg_node_t *best = NULL;
    uint8_t best_pos = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used || nd->health != NODE_H_OFFLINE) continue;   /* UPDATING is not a fault */
        if (!nd->hops_valid) continue;                             /* phantom: never heard */
        uint8_t pos = chain_pos(tab, n_slots, nd);
        /* equal positions cannot happen physically; order by id to stay deterministic */
        if (!best || pos < best_pos || (pos == best_pos && nd->id < best->id)) { best = nd; best_pos = pos; }
    }
    return best;
}

/* Is this node's report of the link worth anything? It must be alive (an
 * OFFLINE node's link_flags are whatever it last managed to send, i.e. from
 * before the break), not mid-OTA (an UPDATING zone is dark on purpose), and
 * past its boot grace. */
static int is_witness(const hg_node_t *nd) {
    if (!nd->used) return 0;
    if (nd->health != NODE_H_ONLINE && nd->health != NODE_H_DEGRADED) return 0;
    if (nd->hb.uptime_s < WITNESS_MIN_UPTIME_S) return 0;
    return 1;
}

/* Is anything still ALIVE (or merely mid-OTA) between U and the master's RX,
 * i.e. at a smaller chain_pos than U? Such a node cannot testify -- that is
 * why it is not the witness D -- but it is on the cable, so the break may be
 * anywhere from U down to it, and "U dead or wire U->M" would name a leg past
 * a node that might itself be the far end. Phantoms are skipped: they have no
 * place on the chain. */
static int anything_alive_below(const hg_node_t *tab, int n_slots, uint8_t u_pos) {
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!nd->used || !nd->hops_valid) continue;
        if (nd->health == NODE_H_OFFLINE || nd->health == NODE_H_EMPTY) continue;
        if (chain_pos(tab, n_slots, nd) < u_pos) return 1;
    }
    return 0;
}

/* D: the most UPSTREAM witness (largest chain_pos), NULL when the ring holds
 * no witness at all -- in which case the downstream end of the break is the
 * master's own RX leg. Whether this witness hears the master is what decides
 * ripeness, not membership: see ring_blame. */
static const hg_node_t *most_upstream_witness(const hg_node_t *tab, int n_slots) {
    const hg_node_t *best = NULL;
    uint8_t best_pos = 0;
    for (int i = 0; i < n_slots; i++) {
        const hg_node_t *nd = &tab[i];
        if (!is_witness(nd)) continue;
        uint8_t pos = chain_pos(tab, n_slots, nd);
        if (!best || pos > best_pos || (pos == best_pos && nd->id < best->id)) { best = nd; best_pos = pos; }
    }
    return best;
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
 * The pre-fix rule named a leg from hop counts alone, latched at the OK->OPEN
 * edge before any zone could report master-silent, and pointed upstream of the
 * silent node instead of downstream; it was wrong on all three legs of the
 * 2026-09-09 wire pulls. See docs/what_we_learned.md. */
static void ring_blame(const hg_node_t *tab, int n_slots, char *out, size_t outsz) {
    const hg_node_t *u = most_downstream_offline(tab, n_slots);
    const hg_node_t *d = most_upstream_witness(tab, n_slots);

    /* RIPENESS -- three ways the evidence does not (yet) support naming a
     * cable, all answered the same way. Name no cable rather than the wrong
     * one: naming the wrong one is the bug being fixed here. */
    int unripe;
    if (d) {
        /* The most-upstream witness still hears the master: the break lies
         * ABOVE it, among nodes whose heartbeats have stopped but which have
         * not finished falling OFFLINE (10 s), so U is not known yet. Naming
         * the segment down to this witness would accuse a cable that is not
         * even in the path. Wait for the ladder. */
        unripe = (d->link_flags & LINK_MASTER_ALIVE) != 0;
    } else if (u) {
        /* No witness at all, so the segment would end at the master's own RX
         * leg -- which only holds if nothing alive stands between U and that
         * RX. A zone that is mid-OTA or still inside its boot grace is exactly
         * that: present on the cable, unable to say what it hears. */
        unripe = anything_alive_below(tab, n_slots, chain_pos(tab, n_slots, u));
    } else {
        unripe = 1;   /* nothing offline and nobody to ask: nothing to point at */
    }
    if (unripe) {
        snprintf(out, outsz, "no node reports a fault");
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
        st->pending_blame[0] = '\0';   /* same clearing as the CLOSED branch: a table */
        st->pending_ticks = 0;         /* emptied mid-break leaves no half-counted verdict */
        return;
    }

    uint32_t since_ret = now_ms - ts_last_returned_ms;
    if (since_ret >= 5000) {
        /* Blame is re-derived on every tick the ring is open, not latched at
         * the edge. At the 5000 ms mark the evidence does not exist yet: the
         * ladder has only just reached DEGRADED, and a starved zone needs
         * 6000 ms of master silence plus one heartbeat to report the loss --
         * so the first verdict is usually "no node reports a fault" and the
         * segment appears a few seconds later, once the nodes above the cut
         * have gone OFFLINE and the ones below it have said they cannot hear
         * the master.
         *
         * A CHANGE therefore has to hold still before it is believed: the same
         * new verdict on BLAME_DWELL_TICKS consecutive ticks, or it is dropped.
         * That filters the flicker of a recovering ring (a rebooted zone comes
         * back reporting no master, an ex-offline node's first heartbeat lands
         * a tick before its neighbour's) without delaying anything real, since
         * the evidence it waits on is itself seconds old. The OK->OPEN EDGE is
         * never delayed -- the alarm goes out on the tick it is detected, only
         * later corrections dwell.
         * The health ladder above has just run, so ring_blame reads each
         * node's CURRENT health -- no second staleness rule. */
        char cand[sizeof st->blame];
        ring_blame(tab, n_slots, cand, sizeof cand);

        int adopt = 0;
        if (st->state != RING_ST_OPEN) {
            adopt = 1;                                            /* the alarm itself: publish now */
        } else if (strcmp(cand, st->blame) == 0) {
            st->pending_ticks = 0;                                /* re-confirmed: drop any pending change */
        } else if (strcmp(cand, st->pending_blame) == 0) {
            if (++st->pending_ticks >= BLAME_DWELL_TICKS) adopt = 1;
        } else {
            snprintf(st->pending_blame, sizeof st->pending_blame, "%s", cand);
            st->pending_ticks = 1;
        }

        if (adopt) {
            snprintf(st->blame, sizeof st->blame, "%s", cand);
            st->pending_blame[0] = '\0';
            st->pending_ticks = 0;
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
        st->pending_blame[0] = '\0';
        st->pending_ticks = 0;
        st->state = RING_ST_OK;
    }
}
