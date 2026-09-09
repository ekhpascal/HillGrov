#include <string.h>
#include "unity.h"
#include "ring_proto.h"

/* hg_node_t.link_flags is the zone's own heartbeat field, copied verbatim:
 *   b0 upstream_alive -- ANY validated frame reached this zone's link layer
 *                        within 6 s, forwards included
 *   b1 master_alive   -- a MASTER-SOURCED frame within 6 s; losing this is
 *                        what makes the zone shout W_LINK_LOST "master silent"
 * Blame reads b1 only. b0 is carried in these tables purely to keep them
 * physically truthful (a node whose upstream leg is cut receives nothing at
 * all, so it reports neither bit) and to pin that no verdict depends on it. */
#define LINK_UPSTREAM_ALIVE  0x01
#define LINK_MASTER_ALIVE    0x02
#define LINK_OK              (LINK_UPSTREAM_ALIVE | LINK_MASTER_ALIVE)
#define LINK_SILENT          LINK_UPSTREAM_ALIVE   /* frames arrive, none of them the master's */
#define LINK_DARK            0x00                  /* nothing arrives at all */

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
   Blame names the segment between U and D (spec 2.7, bench rulings 2026-09-09):
     D = the most UPSTREAM node that is a witness -- alive (ONLINE/DEGRADED,
         not UPDATING) and up for >= 10 s -- i.e. the largest chain_pos; the
         master when there is no such node. If that witness still reports
         master_alive the evidence is UNRIPE (the break is above it and the
         nodes up there have not finished falling OFFLINE), so no cable is
         named at all.
     U = the most DOWNSTREAM OFFLINE node that has been heard at least once
         (smallest chain_pos); the master when there is none.
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

#define NO_FAULT   "no node reports a fault"

static hg_node_t row(uint8_t id, uint8_t hops, uint32_t hb, uint8_t link) {
    hg_node_t n = {0};
    n.used = 1; n.id = id; n.hops = hops; n.hops_valid = 1;
    n.last_hb_ms = hb; n.link_flags = link;
    n.hb.uptime_s = 600;             /* a long-running zone: a trustworthy witness */
    uint32_t age = NOW - hb;
    n.health = age >= 10000 ? NODE_H_OFFLINE : age >= 5000 ? NODE_H_DEGRADED : NODE_H_ONLINE;
    return n;
}

/* An enrolled NVS row the master has never heard from: no hops measurement, no
   heartbeat ever, so it reads OFFLINE the moment the ladder runs. */
static hg_node_t phantom(uint8_t id) {
    hg_node_t n = {0};
    n.used = 1; n.id = id; n.health = NODE_H_OFFLINE;
    return n;
}

/* ---- The 3-board bench, whose ids enrolled in REVERSE physical order:
        M -> Z2 (first hop, hops 1) -> Z1 (last hop, hops 0) -> M ---- */

static void test_blame_bench_cut_master_to_first_hop(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_SILENT);   /* still fed by Z2's own frames */
    tab[1] = row(2, 1, HB_FRESH, LINK_DARK);     /* nothing reaches Z2 any more  */
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
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);     /* alive, but nothing arrives */
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);       /* offline; last flags still saw the master */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);   /* was "Z2 dead or wire M->Z2" */
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->Z1", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0002, st.online_mask);
}

/* The Z1->M cut and a dead Z1 also coincide: everything offline, nobody left
   to witness anything. This is the bench's hold-COM25-in-reset case. */
static void test_blame_bench_last_hop_offline_nobody_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_DEAD, LINK_OK);
    tab[1] = row(2, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);    /* was "Z1 dead or wire Z2->Z1" */
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z1 dead or wire Z1->M", cap.lines[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, st.online_mask);
}

/* ---- The same three cuts on an IN-ORDER chain M -> Z1 (hops 1) -> Z2 (hops 0) -> M ---- */

static void test_blame_inorder_cut_master_to_first_hop(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, HB_FRESH, LINK_DARK);
    tab[1] = row(2, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("wire M->Z1", st.blame);
}

static void test_blame_inorder_first_hop_offline_downstream_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, HB_DEAD,  LINK_OK);
    tab[1] = row(2, 0, HB_FRESH, LINK_DARK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z2", st.blame);
}

static void test_blame_inorder_last_hop_offline_nobody_silent(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 1, HB_DEAD, LINK_OK);
    tab[1] = row(2, 0, HB_DEAD, LINK_OK);
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
    tab[0] = row(1, 2, HB_DEAD,  LINK_OK);       /* upstream of the dead node: offline */
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);       /* the dead node */
    tab[2] = row(3, 0, HB_FRESH, LINK_DARK);     /* downstream: alive, hears nothing */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 3, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
}

static void test_blame_cut_above_the_middle_node(void) {
    hg_node_t tab[3];
    tab[0] = row(1, 2, HB_DEAD,  LINK_OK);       /* only the top hop is offline */
    tab[1] = row(2, 1, HB_FRESH, LINK_DARK);     /* alive but starved: the cut is above it */
    tab[2] = row(3, 0, HB_FRESH, LINK_SILENT);   /* still fed by Z2's frames */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 3, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z2", st.blame);
}

/* ---- 4-node chain, cut in the middle ---- */

static void test_blame_four_node_middle_cut(void) {
    hg_node_t tab[4];
    tab[0] = row(1, 3, HB_DEAD,  LINK_OK);       /* M -> Z1 -> Z2 -| Z3 -> Z4 -> M */
    tab[1] = row(2, 2, HB_DEAD,  LINK_OK);
    tab[2] = row(3, 1, HB_FRESH, LINK_DARK);
    tab[3] = row(4, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
    TEST_ASSERT_EQUAL_HEX16(0x0018, st.online_mask);             /* bit3 | bit4 */
}

/* Same physical chain, ids enrolled scrambled: M -> Z4 -> Z1 -| Z3 -> Z2 -> M.
   Only the measured hops can name this segment; id order would print nonsense. */
static void test_blame_four_node_middle_cut_scrambled_ids(void) {
    hg_node_t tab[4];
    tab[0] = row(4, 3, HB_DEAD,  LINK_OK);
    tab[1] = row(1, 2, HB_DEAD,  LINK_OK);
    tab[2] = row(3, 1, HB_FRESH, LINK_DARK);
    tab[3] = row(2, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->Z3", st.blame);
}

/* Two breaks at once: the one nearest the master's RX is named. Fix it and the
   next one surfaces. M -> Z1 -| Z2 -> Z3 -| Z4 -> M: Z1 is blocked by the upper
   cut, Z2 and Z3 by the lower one, and only Z4 still delivers heartbeats.
   Z4 hears NOTHING (LINK_DARK) even though Z3 is alive and transmitting -- the
   named segment is the cable between them, so nothing Z3 sends can arrive.
   Pinned as documentation of the ordering. */
static void test_blame_two_cuts_names_the_downstream_break(void) {
    hg_node_t tab[4];
    tab[0] = row(1, 3, HB_DEAD,  LINK_OK);       /* still hears the master, cannot be heard */
    tab[1] = row(2, 2, HB_DEAD,  LINK_DARK);
    tab[2] = row(3, 1, HB_DEAD,  LINK_SILENT);   /* fed by Z2, which is alive above it */
    tab[3] = row(4, 0, HB_FRESH, LINK_DARK);     /* the only witness, and it hears nothing */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z3 dead or wire Z3->Z4", st.blame);
}

/* Same rule, breaks one node higher: M -> Z1 -| Z2 -| Z3 -> Z4 -> M. Now Z3 is
   the top witness (it hears nothing) and Z4 below it is fed by Z3's forwarding,
   so Z4 reports b0 set with b1 clear -- the one arrangement in which a node
   below the break is NOT dark. The named segment is again the downstream one. */
static void test_blame_two_cuts_witness_below_the_break_is_fed_by_forwarding(void) {
    hg_node_t tab[4];
    tab[0] = row(1, 3, HB_DEAD,  LINK_OK);       /* hears the master, blocked by the upper cut */
    tab[1] = row(2, 2, HB_DEAD,  LINK_DARK);     /* blocked by the lower cut, hears nothing */
    tab[2] = row(3, 1, HB_FRESH, LINK_DARK);     /* alive: its heartbeats go down to the master */
    tab[3] = row(4, 0, HB_FRESH, LINK_SILENT);   /* Z3 forwards into it, none of it the master's */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
    TEST_ASSERT_EQUAL_HEX16(0x0018, st.online_mask);
}

/* ---- An OFFLINE node's link_flags are whatever it last managed to send, i.e.
        pre-break, so it is never a witness ---- */

static void test_blame_offline_nodes_stale_flags_are_not_trusted(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_DEAD, LINK_SILENT);    /* stale "master silent" from before it went quiet */
    tab[1] = row(2, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);  /* D is the master, not the offline Z1 */
}

/* ---- DEGRADED still counts as alive: its heartbeats are late, not gone ---- */

static void test_blame_degraded_node_can_be_the_silent_end(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_DEGR, LINK_DARK);
    tab[1] = row(2, 1, HB_DEAD, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(NODE_H_DEGRADED, tab[0].health);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
}

/* ---- UPDATING zones witness nothing (fleet OTA breaks the ring on purpose) ---- */

static void test_blame_updating_zone_is_ignored(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_OK);
    tab[1] = row(2, 1, HB_DEAD,  LINK_DARK);     /* would be U (offline) and the top witness... */
    tab[1].updating_until_ms = NOW + 10000;      /* ...but it is mid-OTA */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(NODE_H_UPDATING, tab[1].health);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);                /* Z1, the only witness, hears the master */
}

static void test_blame_no_witness_at_all_names_nothing(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);
    tab[1] = row(2, 1, HB_FRESH, LINK_DARK);
    tab[0].updating_until_ms = NOW + 10000;      /* both mid-OTA: nobody alive to ask, */
    tab[1].updating_until_ms = NOW + 10000;      /* and nobody offline to blame        */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
}

/* ---- Ring open with every node hearing the master: the master's own RX leg,
        or a zone that forwards without saying so. Name no cable. ---- */

static void test_blame_no_node_reports_a_fault(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_OK);
    tab[1] = row(2, 1, HB_FRESH, LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* TIME_SYNC not returning */

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN " NO_FAULT, cap.lines[0]);
}

/* ---- Ripeness: while the most-upstream witness still hears the master, the
        break is above it and the nodes up there have not finished falling
        OFFLINE. Naming a cable then would name one that is not in the path. ---- */

static void test_blame_unripe_until_the_top_hop_falls_offline(void) {
    hg_node_t tab[4];                            /* M -> Z1 -> Z2 -| Z3 -> Z4 -> M */
    tab[0] = row(1, 3, HB_DEAD,  LINK_OK);       /* first to cross OFFLINE */
    tab[1] = row(2, 2, HB_DEGR,  LINK_OK);       /* still only late, still hears the master */
    tab[2] = row(3, 1, HB_FRESH, LINK_DARK);
    tab[3] = row(4, 0, HB_FRESH, LINK_SILENT);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);         /* OPEN edge: publishes at once */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);                /* not "wire M->Z3" */
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    tab[1].last_hb_ms = HB_DEAD;                                 /* Z2 crosses OFFLINE */

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);         /* tick 1 of the new verdict */
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
    TEST_ASSERT_EQUAL_INT(2, cap.count);                         /* the NODE 2 OFFLINE line */
    TEST_ASSERT_EQUAL_STRING("NODE 2 OFFLINE", cap.lines[1]);

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);         /* tick 2: still not adopted */
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
    TEST_ASSERT_EQUAL_INT(2, cap.count);

    ring_health_eval(tab, 4, NOW, 0, &st, cap_cb, &cap);         /* tick 3: dwell satisfied */
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z3", st.blame);
    TEST_ASSERT_EQUAL_INT(3, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->Z3", cap.lines[2]);
}

/* The same timeline on the 2-board bench, pinned from the 2026-09-09 capture
   (blame_round1_capture.txt, 34.5 -> 41.6 s: COM24 held in reset). Z1 keeps
   delivering heartbeats throughout -- its leg to the master's RX is intact --
   so only Z2's stop. */
static void test_blame_bench_hold_timeline(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_OK);       /* 5 s in: Z1 has not noticed yet */
    tab[1] = row(2, 1, HB_DEGR,  LINK_OK);       /* Z2 late; its stale flags still say master-alive */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* the 5 s alarm */
    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
    TEST_ASSERT_EQUAL_STRING("RING OPEN " NO_FAULT, cap.lines[0]);

    tab[1].last_hb_ms = HB_DEAD;                                 /* 10 s in: Z2 goes OFFLINE... */
    tab[0].link_flags = LINK_DARK;                               /* ...and Z1 reports the silence */

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(3, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 2 OFFLINE", cap.lines[1]);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z2 dead or wire Z2->Z1", cap.lines[2]);
}

/* ---- Dwell: a verdict that flickers for a tick or two and reverts is never
        adopted; three consecutive ticks of the same new verdict are. ---- */

static void test_blame_dwell_ignores_a_short_flicker(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    /* Z1's heartbeat momentarily carries master_alive again (a frame slipped
       through, or the flag was sampled early): the verdict would go unripe. */
    tab[0].link_flags = LINK_OK;
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* two ticks is still not enough */
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    tab[0].link_flags = LINK_DARK;                               /* reverted: pending dropped */
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
    TEST_ASSERT_EQUAL_INT(1, cap.count);

    tab[0].link_flags = LINK_OK;                                 /* now it sticks: 3 ticks */
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
    TEST_ASSERT_EQUAL_INT(2, cap.count);
    TEST_ASSERT_EQUAL_STRING("RING OPEN " NO_FAULT, cap.lines[1]);
}

/* ---- Boot grace: a zone that has been up for less than 10 s has not had time
        for its own 6 s link timers to mean anything, so it witnesses nothing.
        This is the bench transient seen when a held board is released: it comes
        back reporting "no master yet" and briefly accused its own feed. ---- */

static void test_blame_boot_grace_zone_is_not_a_witness(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_OK);       /* the settled zone hears the master */
    tab[1] = row(2, 1, HB_FRESH, LINK_DARK);     /* just rebooted: nothing heard yet */
    tab[1].hb.uptime_s = 3;
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);                /* not "wire M->Z2" */

    tab[1].hb.uptime_s = 30;                                     /* past the grace window */
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    TEST_ASSERT_EQUAL_STRING("wire M->Z2", st.blame);            /* now it is evidence */
}

/* ---- The master's own RX leg can only close the segment when NOTHING alive
        stands between U and that RX. A zone that is mid-OTA, or still inside
        its boot grace, cannot testify -- but it is on the cable, so the break
        may be anywhere from U down to it, and "U dead or wire U->M" would jump
        over it. ---- */

static void test_blame_updating_zone_below_u_withholds_the_master_leg(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);     /* the last hop, alive but mid-OTA */
    tab[0].updating_until_ms = NOW + 10000;
    tab[0].health = NODE_H_UPDATING;             /* what the ladder will compute: no event */
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);       /* U: the first hop is offline */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(NODE_H_UPDATING, tab[0].health);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);                /* not "Z2 dead or wire Z2->M" */

    /* The OTA window ends and Z1 turns out to be silent too: now nothing alive
       is left between Z2 and the master's RX, so the segment reaches the master
       -- and U moves down to Z1, the most-downstream offline node. */
    tab[0].updating_until_ms = 0;
    tab[0].last_hb_ms = HB_DEAD;
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);         /* dwell */

    TEST_ASSERT_EQUAL_STRING("Z1 dead or wire Z1->M", st.blame);
    TEST_ASSERT_EQUAL_INT(3, cap.count);
    TEST_ASSERT_EQUAL_STRING("NODE 1 OFFLINE", cap.lines[1]);
    TEST_ASSERT_EQUAL_STRING("RING OPEN Z1 dead or wire Z1->M", cap.lines[2]);
}

static void test_blame_boot_grace_zone_below_u_withholds_the_master_leg(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);     /* heartbeats arriving, but only just booted */
    tab[0].hb.uptime_s = 1;
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);       /* U */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);                /* not "Z2 dead or wire Z2->M" */

    tab[0].hb.uptime_s = 30;                                     /* now it can testify */
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);
    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("Z2 dead or wire Z2->Z1", st.blame);
}

/* ---- A row the master has never heard from is not a suspect: it has no hops
        measurement and no heartbeat, so it cannot be placed on the chain ---- */

static void test_blame_never_heard_row_is_not_u(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_SILENT);   /* the one real zone, feeding the master's RX */
    tab[1] = phantom(2);                         /* enrolled from NVS, never seen */
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_STRING("wire M->Z1", st.blame);            /* never "Z2 dead or ..." */
    TEST_ASSERT_EQUAL_HEX16(0x0002, st.online_mask);
}

static void test_blame_all_rows_never_heard_names_nothing(void) {
    hg_node_t tab[2];
    tab[0] = phantom(1);
    tab[1] = phantom(2);
    ring_status_t st = { .state = RING_ST_OK };

    ring_health_eval(tab, 2, NOW, 0, &st, cap_cb, &cap);

    TEST_ASSERT_EQUAL_INT(RING_ST_OPEN, st.state);
    TEST_ASSERT_EQUAL_STRING(NO_FAULT, st.blame);
}

/* ========== Blame fires once on OK->OPEN; ring stays OPEN until the probe returns (RING CLOSED) ========== */

static void test_ring_stays_open_until_probe_returns_then_closes(void) {
    hg_node_t tab[2];
    tab[0] = row(1, 0, HB_FRESH, LINK_DARK);
    tab[1] = row(2, 1, HB_DEAD,  LINK_OK);
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
    RUN_TEST(test_blame_two_cuts_names_the_downstream_break);
    RUN_TEST(test_blame_two_cuts_witness_below_the_break_is_fed_by_forwarding);
    RUN_TEST(test_blame_offline_nodes_stale_flags_are_not_trusted);
    RUN_TEST(test_blame_degraded_node_can_be_the_silent_end);
    RUN_TEST(test_blame_updating_zone_is_ignored);
    RUN_TEST(test_blame_no_witness_at_all_names_nothing);
    RUN_TEST(test_blame_no_node_reports_a_fault);
    RUN_TEST(test_blame_unripe_until_the_top_hop_falls_offline);
    RUN_TEST(test_blame_bench_hold_timeline);
    RUN_TEST(test_blame_dwell_ignores_a_short_flicker);
    RUN_TEST(test_blame_boot_grace_zone_is_not_a_witness);
    RUN_TEST(test_blame_updating_zone_below_u_withholds_the_master_leg);
    RUN_TEST(test_blame_boot_grace_zone_below_u_withholds_the_master_leg);
    RUN_TEST(test_blame_never_heard_row_is_not_u);
    RUN_TEST(test_blame_all_rows_never_heard_names_nothing);
    RUN_TEST(test_ring_stays_open_until_probe_returns_then_closes);
    RUN_TEST(test_online_mask_bit_math);
    return UNITY_END();
}
