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
        /* Execution complete: within 3000 ms of COMPLETION (inclusive) -- see
           ring_dup_done for why the window cannot run from the start instead */
        if (now_ms - c->t_ms <= 3000) {
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

void ring_dup_done(ring_dup_t *c, uint16_t seq, uint8_t status, const char *detail, uint32_t now_ms)
{
    /* Only update if this completion matches the cached sequence;
       stale completions (e.g. from a slow handler after cache eviction) are ignored */
    if (c->seq != seq) {
        return;
    }
    c->state = RING_DUP_REPLAY;
    /* The 3000 ms replay window runs from COMPLETION, not from start. A command
       can take up to cmd_task's 3500 ms abandon timeout, and the zone checks the
       cache with the clock read when it DEQUEUED the frame: a retransmit queued
       at +520 ms and dequeued after that abandon would have seen "> 3000 ms since
       start" and executed the command a second time -- spec 2.6's at-most-once
       violated on exactly the slow, already-unhappy path. */
    c->t_ms = now_ms;
    c->status = status;
    if (detail) {
        strncpy(c->detail, detail, sizeof(c->detail) - 1);
        c->detail[sizeof(c->detail) - 1] = '\0';
    }
}
