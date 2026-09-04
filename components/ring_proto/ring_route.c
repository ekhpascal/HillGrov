#include <string.h>
#include "ring_proto.h"

ring_rt_t ring_route(int is_master, uint8_t my_id, const uint8_t my_mac[6],
                     const ring_hdr_t *h, const uint8_t *payload)
{
    if (is_master) {
        /* Master rules: src==MASTER -> DROP_SELF; everything else CONSUME; never forward */
        if (h->src == RING_ID_MASTER) {
            return RING_RT_DROP_SELF;
        }
        return RING_RT_CONSUME;
    }

    /* Zone rules. An unassigned node's my_id (0xFE) is "no identity yet", not
     * an address: matching src/dst against it makes the FIRST unassigned hop
     * swallow every other unassigned node's traffic in both directions -- it
     * consumes ASSIGN_IDs meant for boards further down the ring, and drops
     * their 0xFE-sourced heartbeats as if they were its own. So an unassigned
     * node identifies its own frames by MAC only (the dst==0xFE block below),
     * and loop protection for 0xFE-sourced frames rests on the master being
     * the ring sink (it consumes everything and never forwards) plus the TTL. */
    int assigned = my_id != RING_ID_UNASSIGNED;

    if (assigned && h->src == my_id) {
        return RING_RT_DROP_SELF;
    }

    if (assigned && h->dst == my_id) {
        return RING_RT_CONSUME;
    }

    if (h->dst == RING_ID_BCAST) {
        return RING_RT_CONSUME_FWD;
    }

    if (h->dst == RING_ID_UNASSIGNED) {
        if (h->type == RING_T_ASSIGN_ID && h->len >= HG_ASSIGN_LEN) {
            if (memcmp(payload, my_mac, 6) == 0) {
                return RING_RT_CONSUME;
            }
        }
        return RING_RT_FORWARD;
    }

    /* else: check ttl */
    if (h->ttl <= 1) {
        return RING_RT_DROP;
    }

    return RING_RT_FORWARD;
}
