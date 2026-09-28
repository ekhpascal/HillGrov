#include <string.h>
#include "unity.h"
#include "hg_image.h"
#include "psvc_rc.h"
#include "psvc_fw.h"

/* The firmware-install core, behind a fake environment, a fake sink that records its calls, and a fake reader that serves
   an image in scripted chunk sizes. The web upload (http_upload.c) and the panel's microSD install both run exactly this
   sequence, so every refusal means the same thing in both faces. */

/* ---- fake environment ---- */
static int f_claim_ok, f_claimed, f_releases, f_fleet_idle, f_wdt_begins, f_wdt_open, f_wdt_token, f_wdt_ends,
           f_wdt_deletes, f_zclaim_ok, f_zclaims, f_zreleases, f_yields;
static uint32_t f_heap;
static int e_claim(void) { if (!f_claim_ok || f_claimed) return 0; f_claimed = 1; return 1; }
static void e_release(void) { f_claimed = 0; f_releases++; }
static int e_fleet_idle(void) { return f_fleet_idle; }
static uint32_t e_heap(void) { return f_heap; }
/* f_wdt_token is what wdt_begin answers: 1 = subscribed here (the core must hand it back so it is deleted), 0 = the task
   was already subscribed by someone else (psvc_fw_env.c: esp_task_wdt_status() == ESP_OK), so nothing may be deleted. */
static int e_wdt_begin(void) { f_wdt_begins++; if (f_wdt_token) f_wdt_open++; return f_wdt_token; }
static void e_wdt_kick(void) {}
static void e_wdt_end(int token) { f_wdt_ends++; if (token) { f_wdt_open--; f_wdt_deletes++; } }
static void e_yield(void) { f_yields++; }
static int e_zclaim(void) { f_zclaims++; return f_zclaim_ok ? 0 : -1; }
static void e_zrelease(void) { f_zreleases++; }
static const psvc_fw_env_t ENV = {
    .claim = e_claim, .release = e_release, .fleet_idle = e_fleet_idle, .heap_free = e_heap,
    .wdt_begin = e_wdt_begin, .wdt_kick = e_wdt_kick, .wdt_end = e_wdt_end, .yield = e_yield,
    .zone_fw_claim = e_zclaim, .zone_fw_release = e_zrelease, .self_chip = HG_CHIP_ESP32P4 };

/* ---- fake sink ---- */
static int k_ready_rc, k_ready_calls, k_begin_rc, k_begins, k_write_rc, k_writes, k_finish_rc, k_finishes, k_cancels;
static size_t k_max, k_written;
static int k_ready(void) { k_ready_calls++; return k_ready_rc; }
static size_t k_maxfn(void) { return k_max; }
static int k_begin(size_t len) { (void)len; k_begins++; return k_begin_rc; }
static int k_write(const void *b, size_t n) { (void)b; k_writes++; k_written += n; return k_write_rc; }
static int k_finish(psvc_fw_result_t *r) {
    k_finishes++;
    if (r) { strcpy(r->slot, "ota_1"); strcpy(r->version, "9.9.9"); r->len = (uint32_t)k_written; }
    return k_finish_rc;
}
static void k_cancel(void) { k_cancels++; }
static const psvc_fw_sink_t SINK = { .ready = k_ready, .max = k_maxfn, .begin = k_begin, .write = k_write,
                                     .finish = k_finish, .cancel = k_cancel };

/* ---- fake reader ---- */
static uint8_t g_img[40960];
static size_t  g_len, r_pos, r_chunk, r_fail_at;
static int     r_fail_rc, r_again, r_saw_kind, r_monotonic, r_last_pct;
static int rd(void *src, void *buf, size_t cap) {
    (void)src;
    const char *kind = NULL;
    uint8_t pct = 0;
    if (psvc_fw_progress(&kind, &pct)) {
        r_saw_kind = 1;
        if ((int)pct < r_last_pct) r_monotonic = 0;
        r_last_pct = pct;
    }
    if (r_again > 0) { r_again--; return PSVC_FW_SRC_AGAIN; }
    if (r_fail_rc && r_pos >= r_fail_at) return r_fail_rc;
    if (r_pos >= g_len) return PSVC_FW_SRC_FAILED;   /* asked past the image: a core regression fails, never hangs */
    size_t n = cap < r_chunk ? cap : r_chunk;
    if (n > g_len - r_pos) n = g_len - r_pos;
    memcpy(buf, g_img + r_pos, n);
    r_pos += n;
    return (int)n;
}

static void mk_image(size_t len, uint16_t chip, const char *proj) {
    g_len = len;
    for (size_t i = 0; i < sizeof g_img; i++) g_img[i] = (uint8_t)(i * 7u);
    g_img[0] = HG_IMG_MAGIC;
    g_img[12] = (uint8_t)chip; g_img[13] = (uint8_t)(chip >> 8);
    g_img[32] = 0x32; g_img[33] = 0x54; g_img[34] = 0xCD; g_img[35] = 0xAB;     /* 0xABCD5432 LE */
    memset(g_img + 48, 0, 32); memcpy(g_img + 48, "1.2.3", 5);
    memset(g_img + 80, 0, 32); memcpy(g_img + 80, proj, strlen(proj));
}

void setUp(void) {
    f_claim_ok = 1; f_claimed = 0; f_releases = 0; f_fleet_idle = 1; f_heap = 200000u;
    f_wdt_begins = 0; f_wdt_open = 0; f_wdt_token = 1; f_wdt_ends = 0; f_wdt_deletes = 0; f_zclaim_ok = 1; f_zclaims = 0; f_zreleases = 0; f_yields = 0;
    k_ready_rc = 0; k_ready_calls = 0; k_begin_rc = 0; k_begins = 0; k_write_rc = 0; k_writes = 0;
    k_finish_rc = 0; k_finishes = 0; k_cancels = 0; k_max = 1u << 20; k_written = 0;
    r_pos = 0; r_chunk = 4096; r_fail_at = 0; r_fail_rc = 0; r_again = 0;
    r_saw_kind = 0; r_monotonic = 1; r_last_pct = -1;
    mk_image(10000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
}
void tearDown(void) {}

static psvc_rc_t install(psvc_fw_kind_t kind, psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    return psvc_fw_install_with(&ENV, &SINK, kind, g_len, rd, NULL, res, st);
}

static void assert_all_released(void) {
    const char *k = NULL;
    uint8_t p = 99;
    TEST_ASSERT_EQUAL_INT(0, f_claimed);
    TEST_ASSERT_EQUAL_INT(0, f_wdt_open);
    TEST_ASSERT_EQUAL_INT(f_zclaims == 0 || !f_zclaim_ok ? 0 : 1, f_zreleases);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
}

static void test_master_ok(void) {
    psvc_fw_result_t res; psvc_fw_stats_t st;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, &res, &st));
    TEST_ASSERT_EQUAL_INT(1, k_begins);
    TEST_ASSERT_EQUAL_size_t(10000, k_written);
    TEST_ASSERT_EQUAL_INT(1, k_finishes);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    TEST_ASSERT_EQUAL_size_t(10000, st.consumed);
    TEST_ASSERT_EQUAL_UINT8(1, st.started);
    TEST_ASSERT_EQUAL_UINT8(0, st.src_failed);
    TEST_ASSERT_EQUAL_STRING("9.9.9", res.version);
    TEST_ASSERT_EQUAL_INT(1, f_wdt_begins);
    TEST_ASSERT_EQUAL_INT(0, f_zclaims);                 /* a master install never touches zone_fw */
    assert_all_released();
}

static void test_len_zero_is_invalid_and_claims_nothing(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fw_install_with(&ENV, &SINK, PSVC_FW_MASTER, 0, rd, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, f_releases);
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
}

static void test_claim_held_is_upload_active_before_ready(void) {
    psvc_fw_stats_t st;
    f_claim_ok = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_UPLOAD_ACTIVE, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
    TEST_ASSERT_EQUAL_INT(0, f_releases);                /* never claimed, never released */
    TEST_ASSERT_EQUAL_UINT8(0, st.started);
}

static void test_fleet_running_is_fleet_active_before_ready(void) {
    f_fleet_idle = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_ACTIVE, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
    TEST_ASSERT_EQUAL_INT(1, f_releases);
    assert_all_released();
}

static void test_ready_codes(void) {
    k_ready_rc = -2;
    TEST_ASSERT_EQUAL_INT(PSVC_E_TRIAL_PENDING, install(PSVC_FW_MASTER, NULL, NULL));
    k_ready_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_SLOT, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    assert_all_released();
}

static void test_low_heap_internal_too_large(void) {
    psvc_fw_stats_t st;
    f_heap = 1000;
    TEST_ASSERT_EQUAL_INT(PSVC_E_LOW_HEAP, install(PSVC_FW_MASTER, NULL, NULL));
    f_heap = 200000u; k_max = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_INTERNAL, install(PSVC_FW_MASTER, NULL, NULL));
    k_max = 5000;
    TEST_ASSERT_EQUAL_INT(PSVC_E_TOO_LARGE, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(0, st.started);
    TEST_ASSERT_EQUAL_INT(0, f_wdt_begins);              /* guards run before the TWDT is touched */
    TEST_ASSERT_EQUAL_size_t(0, r_pos);                  /* and before a single byte is read */
    assert_all_released();
}

static void test_wrong_project_refused_before_any_erase(void) {
    psvc_fw_stats_t st;
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    TEST_ASSERT_EQUAL_UINT8(1, st.started);              /* past the guards: the web drains before answering */
    TEST_ASSERT_EQUAL_UINT8(0, st.src_failed);
    assert_all_released();
}

static void test_wrong_chip_refused(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_MASTER);      /* an ESP32 master image offered to the P4 (D22) */
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
}

static void test_identity_accumulates_across_one_byte_reads(void) {
    mk_image(300, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    r_chunk = 1;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_begins);
    TEST_ASSERT_EQUAL_size_t(300, k_written);
    assert_all_released();
}

static void test_image_shorter_than_the_header(void) {
    mk_image(50, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
}

static void test_stall_after_begin_cancels(void) {
    psvc_fw_stats_t st;
    r_fail_at = 5000; r_fail_rc = PSVC_FW_SRC_STALLED;
    TEST_ASSERT_EQUAL_INT(PSVC_E_STALLED, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(1, st.src_failed);
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_read_failure_before_identity_does_not_cancel(void) {
    psvc_fw_stats_t st;
    r_chunk = 50; r_fail_at = 50; r_fail_rc = PSVC_FW_SRC_FAILED;
    TEST_ASSERT_EQUAL_INT(PSVC_E_RECV_FAILED, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(1, st.src_failed);
    TEST_ASSERT_EQUAL_size_t(50, st.consumed);
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    assert_all_released();
}

static void test_zone_claim_refused_is_zone_fw_busy(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    f_zclaim_ok = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_FW_BUSY, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, f_zreleases);
    assert_all_released();
}

static void test_zone_ok_claims_and_releases_zone_fw(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_zclaims);
    TEST_ASSERT_EQUAL_INT(1, f_zreleases);
    assert_all_released();
}

static void test_zone_image_checked_against_esp32_not_self(void) {
    mk_image(10000, HG_CHIP_ESP32P4, HG_PROJ_ZONE);      /* a "zone" built for the P4 is no zone image */
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_ZONE, NULL, NULL));
}

static void test_write_failure_cancels(void) {
    k_write_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_finish_failure_cancels(void) {
    k_finish_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_begin_failure_does_not_cancel(void) {
    k_begin_rc = -1;                                     /* http_upload.c:270 -- started never set, no cancel */
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    assert_all_released();
}

static void test_progress_runs_and_ends_empty(void) {
    mk_image(40000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, r_saw_kind);
    TEST_ASSERT_EQUAL_INT(1, r_monotonic);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(90, r_last_pct);    /* last sample before the final read */
    assert_all_released();
}

static void test_again_is_retried(void) {
    r_again = 3;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    assert_all_released();
}

static void test_yields_every_eight_blocks(void) {
    mk_image(40000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);    /* 9 full 4 KB writes + 1 partial = 10 */
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(10, k_writes);
    TEST_ASSERT_EQUAL_INT(1, f_yields);
}

/* ---- Task 30 follow-ups: zone_fw is released on every failure after its claim; identity is checked before the claim;
   an already-subscribed task keeps its TWDT subscription ---- */

static void test_zone_begin_failure_releases_zone_fw(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    k_begin_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_zclaims);
    TEST_ASSERT_EQUAL_INT(1, f_zreleases);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);                 /* begin never succeeded: nothing to undo */
    assert_all_released();
}

static void test_zone_write_failure_releases_zone_fw(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    k_write_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_zclaims);
    TEST_ASSERT_EQUAL_INT(1, f_zreleases);
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_zone_finish_failure_releases_zone_fw(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    k_finish_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_zclaims);
    TEST_ASSERT_EQUAL_INT(1, f_zreleases);
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_zone_identity_refused_before_the_zone_claim(void) {
    mk_image(10000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);    /* a master image offered as a zone image */
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, f_zclaims);                 /* identity BEFORE claim: zone_fw is never touched */
    TEST_ASSERT_EQUAL_INT(0, f_zreleases);
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    assert_all_released();
}

static void test_already_subscribed_twdt_is_not_deleted(void) {
    f_wdt_token = 0;                                     /* the calling task was already watched */
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_wdt_begins);
    TEST_ASSERT_EQUAL_INT(1, f_wdt_ends);                /* the token is handed back exactly once... */
    TEST_ASSERT_EQUAL_INT(0, f_wdt_deletes);             /* ...and a 0 token deletes nothing */
    r_pos = 0;                                           /* rewind the reader: it answers AGAIN forever at its end */
    k_write_rc = -1;                                     /* the failure path hands it back the same way */
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, f_wdt_ends);
    TEST_ASSERT_EQUAL_INT(0, f_wdt_deletes);
    assert_all_released();
}

static void test_own_twdt_subscription_is_deleted(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_wdt_ends);
    TEST_ASSERT_EQUAL_INT(1, f_wdt_deletes);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_master_ok);
    RUN_TEST(test_len_zero_is_invalid_and_claims_nothing);
    RUN_TEST(test_claim_held_is_upload_active_before_ready);
    RUN_TEST(test_fleet_running_is_fleet_active_before_ready);
    RUN_TEST(test_ready_codes);
    RUN_TEST(test_low_heap_internal_too_large);
    RUN_TEST(test_wrong_project_refused_before_any_erase);
    RUN_TEST(test_wrong_chip_refused);
    RUN_TEST(test_identity_accumulates_across_one_byte_reads);
    RUN_TEST(test_image_shorter_than_the_header);
    RUN_TEST(test_stall_after_begin_cancels);
    RUN_TEST(test_read_failure_before_identity_does_not_cancel);
    RUN_TEST(test_zone_claim_refused_is_zone_fw_busy);
    RUN_TEST(test_zone_ok_claims_and_releases_zone_fw);
    RUN_TEST(test_zone_image_checked_against_esp32_not_self);
    RUN_TEST(test_write_failure_cancels);
    RUN_TEST(test_finish_failure_cancels);
    RUN_TEST(test_begin_failure_does_not_cancel);
    RUN_TEST(test_progress_runs_and_ends_empty);
    RUN_TEST(test_again_is_retried);
    RUN_TEST(test_yields_every_eight_blocks);
    RUN_TEST(test_zone_begin_failure_releases_zone_fw);
    RUN_TEST(test_zone_write_failure_releases_zone_fw);
    RUN_TEST(test_zone_finish_failure_releases_zone_fw);
    RUN_TEST(test_zone_identity_refused_before_the_zone_claim);
    RUN_TEST(test_already_subscribed_twdt_is_not_deleted);
    RUN_TEST(test_own_twdt_subscription_is_deleted);
    return UNITY_END();
}
