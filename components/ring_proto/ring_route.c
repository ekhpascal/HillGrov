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

    /* Zone rules */
    if (h->src == my_id) {
        return RING_RT_DROP_SELF;
    }

    if (h->dst == my_id) {
        return RING_RT_CONSUME;
    }

    if (h->dst == RING_ID_BCAST) {
        return RING_RT_CONSUME_FWD;
    }

    if (h->dst == RING_ID_UNASSIGNED) {
        if (h->type == RING_T_ASSIGN_ID && h->len >= 7) {
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
