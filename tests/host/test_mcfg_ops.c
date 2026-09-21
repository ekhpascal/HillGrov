/* mcfg_ops exists so the HTTP handlers and the panel UI cannot each grow their
   own version of the master-config read-modify-write. The properties worth
   pinning are the ones a second caller would get wrong: that a failed lock is
   reported as a failure rather than silently proceeding unsynchronised, that a
   rejecting edit function does NOT commit, and that the edit sees a private
   copy so a rejected edit cannot leave the live config half-modified. */
#include "unity.h"
#include "mcfg_ops.h"
#include "hg_mcfg.h"
#include "mcfg_store.h"   /* mcfg_get() -- mcfg_ops.h does not re-declare it */
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static int set_hostname(hg_mcfg_t *m, void *ctx) {
    snprintf(m->hostname, sizeof m->hostname, "%s", (const char *)ctx);
    return 0;
}

static int reject(hg_mcfg_t *m, void *ctx) {
    (void)ctx;
    snprintf(m->hostname, sizeof m->hostname, "scribbled");
    return -3;   /* a validation refusal */
}

void test_edit_commits_when_the_edit_function_accepts(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_edit(set_hostname, "greenhouse"));
    TEST_ASSERT_EQUAL_STRING("greenhouse", mcfg_get()->hostname);
}

void test_a_rejecting_edit_does_not_commit_and_leaves_no_trace(void) {
    mcfg_ops_init();
    mcfg_ops_edit(set_hostname, "before");
    TEST_ASSERT_EQUAL_INT(-3, mcfg_ops_edit(reject, NULL));
    /* The edit function scribbled on its copy and then refused. The live config
       must still read "before" -- if mcfg_ops handed out a pointer to the live
       buffer instead of a copy, this reads "scribbled". */
    TEST_ASSERT_EQUAL_STRING("before", mcfg_get()->hostname);
}

void test_lock_is_not_recursive_so_a_second_take_fails_fast(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    TEST_ASSERT_EQUAL_INT(-1, mcfg_ops_lock(10));
    mcfg_ops_unlock();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
}

void test_edit_reports_failure_when_the_lock_is_held(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    /* Must NOT proceed unsynchronised. */
    TEST_ASSERT_EQUAL_INT(-1, mcfg_ops_edit(set_hostname, "racer"));
    mcfg_ops_unlock();
}

void test_init_is_idempotent(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
    mcfg_ops_init();            /* must not destroy or replace a live mutex */
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_edit_commits_when_the_edit_function_accepts);
    RUN_TEST(test_a_rejecting_edit_does_not_commit_and_leaves_no_trace);
    RUN_TEST(test_lock_is_not_recursive_so_a_second_take_fails_fast);
    RUN_TEST(test_edit_reports_failure_when_the_lock_is_held);
    RUN_TEST(test_init_is_idempotent);
    return UNITY_END(); }
