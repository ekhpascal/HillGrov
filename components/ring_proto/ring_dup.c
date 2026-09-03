#include <string.h>
#include "ring_proto.h"

void ring_dup_init(ring_dup_t *c)
{
    memset(c, 0, sizeof(*c));
}

int ring_dup_check(ring_dup_t *c, uint16_t seq, uint32_t now_ms)
{
    /* Different sequence: new execution */
    if (c->seq != seq) {
        return RING_DUP_EXEC;
    }

    /* Same sequence: check state and timing */
    if (c->state == RING_DUP_ABSORB) {
        /* Still in progress: absorb the duplicate */
        return RING_DUP_ABSORB;
    }

    if (c->state == RING_DUP_REPLAY) {
        /* Execution complete: check if within 3000 ms window */
        if (now_ms - c->t_ms < 3000) {
            return RING_DUP_REPLAY;
        }
        /* Outside window: treat as new execution */
        return RING_DUP_EXEC;
    }

    /* No state set: treat as new (shouldn't normally happen) */
    return RING_DUP_EXEC;
}

void ring_dup_start(ring_dup_t *c, uint16_t seq, uint32_t now_ms)
{
    c->seq = seq;
    c->state = RING_DUP_ABSORB;
    c->status = 0;
    c->t_ms = now_ms;
    memset(c->detail, 0, sizeof(c->detail));
}

void ring_dup_done(ring_dup_t *c, uint16_t seq, uint8_t status, const char *detail)
{
    c->seq = seq;
    c->state = RING_DUP_REPLAY;
    c->status = status;
    if (detail) {
        strncpy(c->detail, detail, sizeof(c->detail) - 1);
        c->detail[sizeof(c->detail) - 1] = '\0';
    }
}
