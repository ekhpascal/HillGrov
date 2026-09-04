#include <string.h>
#include "ring_proto.h"

/* Stop-and-wait pending tracker: a FIFO queue of at most RING_TRK_DEPTH frames awaiting
   ACK_REQ replies, with a single outstanding (in-flight) entry ring-wide. q[0] is always
   the head-of-line entry; it is "in flight" once its attempts counter is nonzero. */

uint32_t ring_ack_timeout_ms(uint8_t ring_size)
{
    uint32_t v = 200u + 40u * (uint32_t)ring_size;
    if (v < 400u) v = 400u;
    if (v > 900u) v = 900u;
    return v;
}

void ring_trk_init(ring_trk_t *t)
{
    memset(t, 0, sizeof(*t));
}

static int is_priority_type(uint8_t type)
{
    return type == RING_T_CMD || type == RING_T_FW_UPDATE;
}

int ring_trk_submit(ring_trk_t *t, const ring_hdr_t *h, const uint8_t *payload, uint32_t now_ms)
{
    (void)now_ms;
    if (t->n >= RING_TRK_DEPTH) return -1;

    ring_trk_slot_t e;
    memset(&e, 0, sizeof(e));
    e.seq = h->seq;
    e.dst = h->dst;
    e.type = h->type;
    int wlen = ring_frame_encode(h, payload, e.wire, sizeof(e.wire));
    if (wlen < 0) return -1;
    e.wire_len = (uint16_t)wlen;

    /* CMD/FW_UPDATE insert after any in-flight entry and after other queued CMD/FW_UPDATE
       entries, but ahead of queued CFG_COMMIT/CFG_GET entries; everything else appends. */
    uint8_t idx = t->n;
    if (is_priority_type(h->type)) {
        uint8_t start = (t->n > 0 && t->q[0].attempts > 0) ? 1 : 0;
        idx = start;
        while (idx < t->n && is_priority_type(t->q[idx].type)) idx++;
    }

    for (uint8_t i = t->n; i > idx; i--) t->q[i] = t->q[i - 1];
    t->q[idx] = e;
    t->n++;
    return 0;
}

static void fill_send(ring_trk_ev_t *ev, const ring_trk_slot_t *e)
{
    ev->kind = RING_TRK_EV_SEND;
    ev->seq = e->seq; ev->dst = e->dst; ev->type = e->type;
    ev->wire = e->wire; ev->wire_len = e->wire_len;
    ev->status = 0; ev->detail[0] = '\0';
    ev->fail_token = NULL;
}

static void fill_fail(ring_trk_ev_t *ev, const ring_trk_slot_t *e, const char *token)
{
    ev->kind = RING_TRK_EV_FAIL;
    ev->seq = e->seq; ev->dst = e->dst; ev->type = e->type;
    ev->wire = NULL; ev->wire_len = 0;
    ev->status = 0; ev->detail[0] = '\0';
    ev->fail_token = token;
}

static void remove_head(ring_trk_t *t)
{
    for (uint8_t i = 1; i < t->n; i++) t->q[i - 1] = t->q[i];
    t->n--;
}

int ring_trk_tick(ring_trk_t *t, uint32_t now_ms, uint8_t ring_size, ring_trk_ev_t *ev)
{
    if (t->n == 0) return 0;
    ring_trk_slot_t *head = &t->q[0];

    if (head->attempts == 0) {
        head->attempts = 1;
        head->last_send_ms = now_ms;
        fill_send(ev, head);
        return 1;
    }

    if (now_ms - head->last_send_ms < ring_ack_timeout_ms(ring_size)) return 0;

    if (head->attempts >= 3) {
        fill_fail(ev, head, "ZONE_TIMEOUT");
        remove_head(t);
        return 1;
    }

    head->attempts++;
    head->last_send_ms = now_ms;
    fill_send(ev, head);
    return 1;
}

int ring_trk_ack(ring_trk_t *t, uint16_t acked_seq, uint8_t status, const char *detail,
                 uint8_t detail_len, ring_trk_ev_t *ev)
{
    if (t->n == 0) return 0;
    ring_trk_slot_t *head = &t->q[0];
    if (head->attempts == 0 || head->seq != acked_seq) return 0;

    ev->kind = RING_TRK_EV_DONE;
    ev->seq = head->seq; ev->dst = head->dst; ev->type = head->type;
    ev->wire = NULL; ev->wire_len = 0;
    ev->status = status;
    uint8_t n = detail_len;
    if (n > sizeof(ev->detail) - 1) n = (uint8_t)(sizeof(ev->detail) - 1);
    if (detail && n) memcpy(ev->detail, detail, n);
    ev->detail[n] = '\0';
    ev->fail_token = NULL;

    remove_head(t);
    return 1;
}

/* Withdraw a QUEUED entry (attempts == 0 -- never put on the wire). An entry
   already in flight cannot be withdrawn: its ACK or timeout is still coming and
   the submitter's own state machine has to see it, so -1 and the queue is left
   exactly as it was. Used by the §4.4 reconciler when a master-originated edit
   supersedes a push whose CFG_COMMIT is still waiting behind an in-flight CMD. */
int ring_trk_cancel(ring_trk_t *t, uint16_t seq)
{
    for (uint8_t i = 0; i < t->n; i++) {
        if (t->q[i].seq != seq) continue;
        if (t->q[i].attempts != 0) return -1;
        for (uint8_t j = (uint8_t)(i + 1); j < t->n; j++) t->q[j - 1] = t->q[j];
        t->n--;
        return 0;
    }
    return -1;
}

int ring_trk_unclaimed(ring_trk_t *t, const ring_hdr_t *returned, ring_trk_ev_t *ev)
{
    if (t->n == 0) return 0;
    ring_trk_slot_t *head = &t->q[0];
    if (head->attempts == 0) return 0;
    if (head->seq != returned->seq || head->dst != returned->dst) return 0;

    fill_fail(ev, head, "ZONE_UNKNOWN");
    remove_head(t);
    return 1;
}
