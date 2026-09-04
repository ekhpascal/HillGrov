#include <string.h>
#include "unity.h"
#include "ring_proto.h"

#define LINK_UPSTREAM_ALIVE 0x01

typedef struct { char lines[16][64]; int count; } cap_t;
static cap_t cap;

static void cap_cb(void *ctx, const char *line) {
    cap_t *c = (cap_t *)ctx;
    if (c->count >= 16) return;
    size_t n = strlen(line);
    if (n >= sizeof c->lines[0]) n = sizeof c->lines[0] - 1;
    memcpy(c->lines[c->count], line, n);
    c->lines[c->count][n] = '\0';
    c->count++;
}

void setUp(void) { memset(&cap, 0, sizeof cap); }
void tearDown(void) {}

/* ========== Per-node ladder: ONLINE -> DEGRADED -> OFFLINE -> ONLINE, events on transitions only ========== */

static void test_ladder_online_degraded_offline_recover(void) {
    hg_node_t tab[1] = {0};
    tab[0].used = 1; tab[0].id = 1; tab[0].last_hb_ms = 0;
    ring_status_t st = {0};

    ring_health_eval(tab, 1, 0, 0, &st, cap_cb, &cap);          /* HB at t=0 */
    TEST_ASSERT_EQUAL_INT(NODE_H_ONLINE, tab[0].health);
    TEST_ASSERT_EQUAL_INT(0, cap.count);                        /* first observation: no event */

    ring_health_eval(tab, 1, 5001, 5001, &st, cap_cb, &cap);    /* 5001 ms without HB */
    TEST_ASSERT_EQUAL_INT(NODE_H_DEGRADED, tab[0].health);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 1 DEGRADED", cap.lines[0]);

    ring_health_eval(tab, 1, 5500, 5500, &st, cap_cb, &cap);    /* still DEGRADED: no repeat event */
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    ring_health_eval(tab, 1, 10001, 10001, &st, cap_cb, &cap);  /* 10001 ms without HB */
    TEST_ASSERT_EQUAL_INT(NODE_H_OFFLINE, tab[0].health);
    TEST_ASSERT_EQUAL_INT(2, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 1 OFFLINE", cap.lines[1]);

    ring_health_eval(tab, 1, 10500, 10500, &st, cap_cb, &cap);  /* still OFFLINE: no repeat event */
    TEST_ASSERT_EQUAL_INT(2, cap.count);

    tab[0].last_hb_ms = 15000;                                  /* HB again */
    ring_health_eval(tab, 1, 15000, 15000, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_INT(NODE_H_ONLINE, tab[0].health);
    TEST_ASSERT_EQUAL_INT(3, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 1 ONLINE", cap.lines[2]);
}

/* ========== cmd_timeouts >= 3 forces DEGRADED immediately, independent of HB age ========== */

static void test_cmd_timeouts_force_degraded_immediately(void) {
    hg_node_t tab[1] = {0};
    tab[0].used = 1; tab[0].id = 3; tab[0].last_hb_ms = 0; tab[0].cmd_timeouts = 3;
    ring_status_t st = {0};

    ring_health_eval(tab, 1, 0, 0, &st, cap_cb, &cap);          /* since_hb == 0, but cmd_timeouts == 3 */
    TEST_ASSERT_EQUAL_INT(NODE_H_DEGRADED, tab[0].health);
}

/* ========== UPDATING freezes the ladder; resumes (typically straight to OFFLINE) after the window ========== */

static void test_updating_freeze_then_resumes_offline(void) {
    hg_node_t tab[1] = {0};
    tab[0].used = 1; tab[0].id = 2; tab[0].last_hb_ms = 0; tab[0].updating_until_ms = 20000;
    ring_status_t st = {0};

    ring_health_eval(tab, 1, 0, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_INT(NODE_H_UPDATING, tab[0].health);
    TEST_ASSERT_EQUAL_INT(0, cap.count);

    ring_health_eval(tab, 1, 15000, 15000, &st, cap_cb, &cap);  /* well past 10s since HB, still frozen */
    TEST_ASSERT_EQUAL_INT(NODE_H_UPDATING, tab[0].health);
    TEST_ASSERT_EQUAL_INT(0, cap.count);                        /* no DEGRADED/OFFLINE events during freeze */

    ring_health_eval(tab, 1, 20001, 20001, &st, cap_cb, &cap);  /* window passed; stale last_hb_ms=0 resumes */
    TEST_ASSERT_EQUAL_INT(NODE_H_OFFLINE, tab[0].health);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 2 OFFLINE", cap.lines[0]);
}

/* ========== Empty table: IDLE forever, no events ever ========== */

static void test_empty_table_stays_idle_no_events(void) {
    hg_node_t tab[4] = {0};
    ring_status_t st = {0};

    ring_health_eval(tab, 4, 0, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_INT(RING_ST_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT8(0, st.size);
    TEST_ASSERT_EQUAL_UINT16(0, st.online_mask);

    ring_health_eval(tab, 4, 999999, 0, &st, cap_cb, &cap);     /* huge staleness, still no used nodes */
    TEST_ASSERT_EQUAL_INT(RING_ST_IDLE, st.state);
    TEST_ASSERT_EQUAL_INT(0, cap.count);
}

/* ========== Ring OPEN blame, 3-scenario bench (Z1, Z2 used; M = master) ==========
   hops is measured at the MASTER'S RX (RING_TTL_INIT - ttl), so the zone feeding
   that RX has hops 0 and the first hop after the master's TX has the highest.
   These three cases are the IN-ORDER chain M->Z1->Z2->M (Z1 hops 1, Z2 hops 0);
   their expected strings are unchanged by the hop-order leg naming. */

static void test_ring_open_blame_silent_lowest_id(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .hops = 1, .hops_valid = 1, .last_hb_ms = 0,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_DEGRADED };   /* Z1 silent */
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .hops = 0, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };      /* Z2 hops normal */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);       /* ts_last_returned 6000 ms old */

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire M->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z1 dead or wire M->Z1", cap.lines[0]);
}

static void test_ring_open_blame_upstream_alive_clear(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .hops = 1, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };      /* Z1 alive */
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .hops = 0, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = 0, .health = NODE_H_ONLINE };                        /* Z2 upstream_alive clear */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z1->Z2", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z1->Z2", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0006, st.online_mask);            /* bit1 | bit2, both fresh */
}

static void test_ring_open_blame_masters_own_rx_leg(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .hops = 1, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .hops = 0, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };      /* all HBs alive */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);       /* TIME_SYNC not returning */

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->M", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->M", cap.lines[0]);
}

/* ========== Blame fires once on OK->OPEN; ring stays OPEN until the probe returns (RING CLOSED) ========== */

static void test_ring_stays_open_until_probe_returns_then_closes(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .hops = 1, .hops_valid = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .hops = 0, .hops_valid = 1, .last_hb_ms = 0,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_DEGRADED };    /* Z2 silent */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);       /* OK -> OPEN: one blame event */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    ring_health_eval(tab, 2, 6500, 0, &st, cap_cb, &cap);       /* still stale: stays OPEN, no repeat blame */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    ring_health_eval(tab, 2, 7000, 7000, &st, cap_cb, &cap);    /* probe returns: OPEN -> OK */
    TEST_ASSERT_EQUAL_INT(RING_ST_OK, st.state);
    TEST_ASSERT_EQUAL_INT(2, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING CLOSED", cap.lines[1]);
    TEST_ASSERT_EQUAL_STRING("", st.blame);
}

/* ========== Blame legs follow MEASURED HOP ORDER, not id order ==========
   The 3-board bench enrolled its ids in reverse physical order: the chain is
   M->Z2->Z1->M, so Z2 (first hop after the master's TX) has hops 1 and Z1 (feeds
   the master's RX) has hops 0. Naming legs by id order printed "wire Z1->Z2" for
   the physical M->Z2 leg -- the operator was sent to the wrong cable. */

static void bench_reversed(hg_node_t *tab, uint32_t z1_hb, uint32_t z2_hb,
                            uint8_t z1_link, uint8_t z2_link) {
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .hops = 0, .hops_valid = 1, .last_hb_ms = z1_hb,
                           .link_flags = z1_link, .health = NODE_H_ONLINE };   /* last hop */
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .hops = 1, .hops_valid = 1, .last_hb_ms = z2_hb,
                           .link_flags = z2_link, .health = NODE_H_ONLINE };   /* first hop */
}

static void test_blame_reversed_hops_first_hop_owns_the_master_leg(void) {
    hg_node_t tab[2] = {0};
    bench_reversed(tab, 6000, 0, LINK_UPSTREAM_ALIVE, LINK_UPSTREAM_ALIVE);   /* Z2 (first hop) silent */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire M->Z2", st.blame);   /* was "wire Z1->Z2" */
}

static void test_blame_reversed_hops_last_hop_leg_named_from_upstream(void) {
    hg_node_t tab[2] = {0};
    bench_reversed(tab, 0, 6000, LINK_UPSTREAM_ALIVE, LINK_UPSTREAM_ALIVE);   /* Z1 (last hop) silent */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z2->Z1", st.blame);   /* was "wire M->Z1" */
}

static void test_blame_reversed_hops_upstream_alive_clear_case(void) {
    hg_node_t tab[2] = {0};
    bench_reversed(tab, 6000, 6000, 0, LINK_UPSTREAM_ALIVE);   /* Z1's upstream (= Z2) is dead */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z2->Z1", st.blame);
}

static void test_blame_reversed_hops_masters_rx_leg_is_the_last_hop(void) {
    hg_node_t tab[2] = {0};
    bench_reversed(tab, 6000, 6000, LINK_UPSTREAM_ALIVE, LINK_UPSTREAM_ALIVE);   /* all alive */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);       /* TIME_SYNC not returning */

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);   /* Z1 owns the ->M leg, not Z2 */
}

/* ========== hops never measured (NVS row, never heard): fall back to id order ========== */

static void test_blame_unknown_hops_falls_back_to_id_order(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .last_hb_ms = 0,       /* silent, hops_valid 0 */
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z1->Z2", st.blame);   /* id order: predecessor is Z1 */
}

static void test_blame_unknown_hops_lowest_id_takes_the_master_leg(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .last_hb_ms = 0,       /* silent, hops_valid 0 */
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire M->Z1", st.blame);
}

static void test_blame_unknown_hops_masters_rx_leg_uses_highest_id(void) {
    hg_node_t tab[2] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    tab[1] = (hg_node_t){ .used = 1, .id = 2, .last_hb_ms = 6000,
                           .link_flags = LINK_UPSTREAM_ALIVE, .health = NODE_H_ONLINE };
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, 6000, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->M", st.blame);
}

/* ========== ring_online_mask bit math ========== */

static void test_online_mask_bit_math(void) {
    hg_node_t tab[8] = {0};
    tab[0] = (hg_node_t){ .used = 1, .id = 1, .last_hb_ms = 9000 };   /* fresh: since = 1000 */
    tab[1] = (hg_node_t){ .used = 1, .id = 3, .last_hb_ms = 0 };      /* stale: since = 10000 */
    tab[2] = (hg_node_t){ .used = 1, .id = 5, .last_hb_ms = 9999 };   /* fresh: since = 1 */

    uint16_t mask = ring_online_mask(tab, 8, 10000);
    TEST_ASSERT_EQUAL_HEX16(0x0022, mask);   /* bit1 | bit5 */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ladder_online_degraded_offline_recover);
    RUN_TEST(test_cmd_timeouts_force_degraded_immediately);
    RUN_TEST(test_updating_freeze_then_resumes_offline);
    RUN_TEST(test_empty_table_stays_idle_no_events);
    RUN_TEST(test_ring_open_blame_silent_lowest_id);
    RUN_TEST(test_ring_open_blame_upstream_alive_clear);
    RUN_TEST(test_ring_open_blame_masters_own_rx_leg);
    RUN_TEST(test_ring_stays_open_until_probe_returns_then_closes);
    RUN_TEST(test_blame_reversed_hops_first_hop_owns_the_master_leg);
    RUN_TEST(test_blame_reversed_hops_last_hop_leg_named_from_upstream);
    RUN_TEST(test_blame_reversed_hops_upstream_alive_clear_case);
    RUN_TEST(test_blame_reversed_hops_masters_rx_leg_is_the_last_hop);
    RUN_TEST(test_blame_unknown_hops_falls_back_to_id_order);
    RUN_TEST(test_blame_unknown_hops_lowest_id_takes_the_master_leg);
    RUN_TEST(test_blame_unknown_hops_masters_rx_leg_uses_highest_id);
    RUN_TEST(test_online_mask_bit_math);
    return UNITY_END();
}
