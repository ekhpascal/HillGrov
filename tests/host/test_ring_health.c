#include <string.h>
#include "unity.h"
#include "ring_proto.h"

/* hg_node_t.link_flags is the zone's own heartbeat field, copied verbatim:
 *   b0 upstream_alive -- ANY frame arrived from the upstream leg within 6 s
 *   b1 master_alive   -- a MASTER-SOURCED frame within 6 s; losing this is
 *                        what makes the zone shout W_LINK_LOST "master silent"
 * Blame reads b1 only (bench ruling 2026-09-09): b0 stayed SET on every node
 * whose upstream leg had just been pulled, so it cannot see a break. */
#define LINK_UPSTREAM_ALIVE  0x01
#define LINK_MASTER_ALIVE    0x02
#define LINK_OK              (LINK_UPSTREAM_ALIVE | LINK_MASTER_ALIVE)
#define LINK_SILENT          LINK_UPSTREAM_ALIVE   /* alive leg, no master frames */

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

/* ================================ Ring OPEN blame ================================
   Blame names the segment between U and D (spec 2.7, bench ruling 2026-09-09):
     U = the most DOWNSTREAM offline node (smallest hops), the master if none
     D = the most UPSTREAM alive node reporting master-silent (largest hops),
         the master if none
   because a cut on the leg into node N starves N and everything downstream of
   master frames (they stay ONLINE -- their heartbeats keep reaching the
   master's RX) and takes everything upstream of the cut OFFLINE (heartbeats
   cannot get past it, though those nodes still hear the master).

   hops is measured at the MASTER'S RX (RING_TTL_INIT - ttl): the zone feeding
   that RX has hops 0 and the FIRST hop after the master's TX has the highest.

   The scenarios below share one clock: health comes from the HB age, so each
   row is built with the age that produces the health it needs, and its .health
   field is pre-loaded with that same value so no NODE transition event fires
   and cap.count counts ring lines only. */

#define NOW        20000u
#define HB_FRESH   19000u   /*  1 s old -> ONLINE   */
#define HB_DEGR    13000u   /*  7 s old -> DEGRADED */
#define HB_DEAD     5000u   /* 15 s old -> OFFLINE  */

static hg_node_t row(uint8_t id, uint8_t hops, uint8_t hops_valid, uint32_t hb, uint8_t link) {
    hg_node_t n = {0};
    n.used = 1; n.id = id; n.hops = hops; n.hops_valid = hops_valid;
    n.last_hb_ms = hb; n.link_flags = link;
    uint32_t age = NOW - hb;
    n.health = age >= 10000 ? NODE_H_OFFLINE : age >= 5000 ? NODE_H_DEGRADED : NODE_H_ONLINE;
    return n;
}

/* ---- The 3-board bench, whose ids enrolled in REVERSE physical order:
        M -> Z2 (first hop, hops 1) -> Z1 (last hop, hops 0) -> M ---- */

static void test_blame_bench_cut_master_to_first_hop(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_SILENT);    /* both still deliver heartbeats... */
    tab[1] = row(2, 1, 1, HB_FRESH, LINK_SILENT);    /* ...and both lost the master */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("wire M->Z2", st.blame);            /* was "Z1 dead or wire Z1->M" */
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN wire M->Z2", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0006, st.online_mask);
}

/* The Z2->Z1 cut and a dead Z2 (held in reset) present the SAME table -- an
   offline first hop with a master-silent last hop -- which is why the text
   offers both readings. This is the bench's hold-COM24-in-reset case. */
static void test_blame_bench_first_hop_offline_downstream_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_SILENT);    /* alive, starved of master frames */
    tab[1] = row(2, 1, 1, HB_DEAD,  LINK_OK);        /* offline; last flags still saw the master */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);   /* was "Z2 dead or wire M->Z2" */
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->Z1", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0002, st.online_mask);
}

/* The Z1->M cut and a dead Z1 also coincide: everything offline, nobody
   master-silent. This is the bench's hold-COM25-in-reset case. */
static void test_blame_bench_last_hop_offline_nobody_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_DEAD, LINK_OK);
    tab[1] = row(2, 1, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);    /* was "Z1 dead or wire Z2->Z1" */
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z1 dead or wire Z1->M", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, st.online_mask);
}

/* ---- The same three cuts on an IN-ORDER chain M -> Z1 (hops 1) -> Z2 (hops 0) -> M ---- */

static void test_blame_inorder_cut_master_to_first_hop(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, 1, HB_FRESH, LINK_SILENT);
    tab[1] = row(2, 0, 1, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("wire M->Z1", st.blame);
}

static void test_blame_inorder_first_hop_offline_downstream_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, 1, HB_DEAD,  LINK_OK);
    tab[1] = row(2, 0, 1, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z2", st.blame);
}

static void test_blame_inorder_last_hop_offline_nobody_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, 1, HB_DEAD, LINK_OK);
    tab[1] = row(2, 0, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->M", st.blame);
}

/* ---- A dead node in the middle of a 3-node chain names ITSELF as U, because
        it is offline while its downstream neighbour is only master-silent; a
        cut ABOVE it instead names the node above. The pair proves U is the
        most-downstream offline node and not merely "an offline node". ---- */

static void test_blame_dead_middle_node_names_itself(void) {
    hg_node_t tab[3];
    tab[0] = row(1, 2, 1, HB_DEAD,  LINK_OK);        /* upstream of the dead node: offline */
    tab[1] = row(2, 1, 1, HB_DEAD,  LINK_OK);        /* the dead node */
    tab[2] = row(3, 0, 1, HB_FRESH, LINK_SILENT);    /* downstream: alive, master-silent */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 3, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
}

static void test_blame_cut_above_the_middle_node(void) {
    hg_node_t tab[3];
    tab[0] = row(1, 2, 1, HB_DEAD,  LINK_OK);        /* only the top hop is offline */
    tab[1] = row(2, 1, 1, HB_FRESH, LINK_SILENT);    /* alive but starved: the cut is above it */
    tab[2] = row(3, 0, 1, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 3, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z2", st.blame);
}

/* ---- 4-node chain, cut in the middle ---- */

static void test_blame_four_node_middle_cut(void) {
    hg_node_t tab[4];
    tab[0] = row(1, 3, 1, HB_DEAD,  LINK_OK);        /* M -> Z1 -> Z2 -| Z3 -> Z4 -> M */
    tab[1] = row(2, 2, 1, HB_DEAD,  LINK_OK);
    tab[2] = row(3, 1, 1, HB_FRESH, LINK_SILENT);
    tab[3] = row(4, 0, 1, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
    TEST_ASSERT_EQUAL_HEX16(0x0018, st.online_mask);             /* bit3 | bit4 */
}

/* Same physical chain, ids enrolled scrambled: M -> Z4 -> Z1 -| Z3 -> Z2 -> M.
   Only the measured hops can name this segment; id order would print nonsense. */
static void test_blame_four_node_middle_cut_scrambled_ids(void) {
    hg_node_t tab[4];
    tab[0] = row(4, 3, 1, HB_DEAD,  LINK_OK);
    tab[1] = row(1, 2, 1, HB_DEAD,  LINK_OK);
    tab[2] = row(3, 1, 1, HB_FRESH, LINK_SILENT);
    tab[3] = row(2, 0, 1, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z3", st.blame);
}

/* ---- An OFFLINE node's link_flags are whatever it last managed to send, i.e.
        pre-cut, so they never make it D ---- */

static void test_blame_offline_nodes_stale_flags_are_not_trusted(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_DEAD, LINK_SILENT);     /* stale "master silent" from before it went quiet */
    tab[1] = row(2, 1, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);  /* D is the master, not the offline Z1 */
}

/* ---- DEGRADED still counts as alive: its heartbeats are late, not gone ---- */

static void test_blame_degraded_node_can_be_the_silent_end(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_DEGR, LINK_SILENT);
    tab[1] = row(2, 1, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(NODE_H_DEGRADED, tab[0].health);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
}

/* ---- UPDATING zones are ignored at both ends (fleet OTA breaks the ring on purpose) ---- */

static void test_blame_updating_zone_is_ignored(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_OK);
    tab[1] = row(2, 1, 1, HB_DEAD,  LINK_SILENT);    /* would be U (offline) and D (silent)... */
    tab[1].updating_until_ms = NOW + 10000;          /* ...but it is mid-OTA */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(NODE_H_UPDATING, tab[1].health);
    TEST_ASSERT_EQUAL_STRING("ring open (no node reports a fault)", st.blame);
}

/* ---- Ring open with every node happy: the master's own RX leg, or a zone
        that forwards without saying so. Name no cable rather than a wrong one. ---- */

static void test_blame_no_node_reports_a_fault(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_OK);
    tab[1] = row(2, 1, 1, HB_FRESH, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* TIME_SYNC not returning */

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("ring open (no node reports a fault)", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN ring open (no node reports a fault)", cap.lines[0]);
}

/* ---- hops never measured (NVS row, never heard): fall back to id order,
        where the lowest used id is the first hop after the master's TX ---- */

static void test_blame_unknown_hops_falls_back_to_id_order(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 0, HB_DEAD,  LINK_OK);        /* hops_valid 0 on both rows */
    tab[1] = row(2, 0, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z2", st.blame);
}

static void test_blame_unknown_hops_master_leg_when_nothing_is_offline(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 0, HB_FRESH, LINK_SILENT);
    tab[1] = row(2, 0, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("wire M->Z1", st.blame);            /* lowest id = first hop */
}

/* ========== Blame fires once on OK->OPEN; ring stays OPEN until the probe returns (RING CLOSED) ========== */

static void test_ring_stays_open_until_probe_returns_then_closes(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_SILENT);
    tab[1] = row(2, 1, 1, HB_DEAD,  LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* OK -> OPEN: one blame event */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    ring_health_eval(tab, 2, NOW + 500, 0, &st, cap_cb, &cap);   /* still stale: stays OPEN, no repeat blame */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    ring_health_eval(tab, 2, NOW + 1000, NOW + 1000, &st, cap_cb, &cap);   /* probe returns: OPEN -> OK */
    TEST_ASSERT_EQUAL_INT(RING_ST_OK, st.state);
    TEST_ASSERT_EQUAL_INT(2, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING CLOSED", cap.lines[1]);
    TEST_ASSERT_EQUAL_STRING("", st.blame);
}

/* ========== ...but the verdict itself is re-derived every tick, because at the
   5 s mark the evidence does not exist yet: nothing is OFFLINE (10 s) and no
   starved zone has had its 6 s of master silence plus a heartbeat to report
   the loss. This is the bench's "hold a board in reset" timeline. ========== */

static void test_blame_is_refined_while_the_ring_stays_open(void) {
    /* The whole timeline of holding Z2 (the first hop) in reset, as the master
       sees it. Z1 keeps delivering heartbeats throughout -- its leg to the
       master's RX is intact -- so only Z2's heartbeats stop. */
    hg_node_t tab[2];

    /* t ~ 5 s: the ring probe has not returned, but Z2 is merely late (DEGRADED)
       and Z1 has not yet had 6 s of master silence to report anything. */
    tab[0] = row(1, 0, 1, HB_FRESH, LINK_OK);
    tab[1] = row(2, 1, 1, HB_DEGR,  LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING("ring open (no node reports a fault)", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN ring open (no node reports a fault)", cap.lines[0]);

    /* t ~ 7 s: Z1 now reports master-silent. U/D taken literally would answer
       "wire M->Z1" -- a cable that is not even in the path -- because nothing
       has reached OFFLINE yet. Z2's missing heartbeats say that reading is not
       ripe, so the verdict is withheld and nothing is re-notified. */
    tab[0].link_flags = LINK_SILENT;

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("ring open (no node reports a fault)", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    /* t ~ 10 s: Z2 crosses the OFFLINE line and the segment is now provable. */
    tab[1].last_hb_ms = HB_DEAD;

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(3, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 2 OFFLINE", cap.lines[1]);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->Z1", cap.lines[2]);

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);   /* verdict unchanged: silent */
    TEST_ASSERT_EQUAL_INT(3, cap.count);
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
    RUN_TEST(test_blame_bench_cut_master_to_first_hop);
    RUN_TEST(test_blame_bench_first_hop_offline_downstream_silent);
    RUN_TEST(test_blame_bench_last_hop_offline_nobody_silent);
    RUN_TEST(test_blame_inorder_cut_master_to_first_hop);
    RUN_TEST(test_blame_inorder_first_hop_offline_downstream_silent);
    RUN_TEST(test_blame_inorder_last_hop_offline_nobody_silent);
    RUN_TEST(test_blame_dead_middle_node_names_itself);
    RUN_TEST(test_blame_cut_above_the_middle_node);
    RUN_TEST(test_blame_four_node_middle_cut);
    RUN_TEST(test_blame_four_node_middle_cut_scrambled_ids);
    RUN_TEST(test_blame_offline_nodes_stale_flags_are_not_trusted);
    RUN_TEST(test_blame_degraded_node_can_be_the_silent_end);
    RUN_TEST(test_blame_updating_zone_is_ignored);
    RUN_TEST(test_blame_no_node_reports_a_fault);
    RUN_TEST(test_blame_unknown_hops_falls_back_to_id_order);
    RUN_TEST(test_blame_unknown_hops_master_leg_when_nothing_is_offline);
    RUN_TEST(test_ring_stays_open_until_probe_returns_then_closes);
    RUN_TEST(test_blame_is_refined_while_the_ring_stays_open);
    RUN_TEST(test_online_mask_bit_math);
    return UNITY_END();
}
