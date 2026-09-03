#include "unity.h"
#include "node_store.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_assign_fills_1_to_8_in_order(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};
    uint8_t mac3[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x03};

    int id1 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id1);
    TEST_ASSERT_EQUAL_UINT8(ZTAB_F_ASSIGNED | ZTAB_F_UNCONFIGURED, t.e[0].flags);

    int id2 = ztab_assign(&t, mac2);
    TEST_ASSERT_EQUAL_INT(2, id2);
    TEST_ASSERT_EQUAL_UINT8(ZTAB_F_ASSIGNED | ZTAB_F_UNCONFIGURED, t.e[1].flags);

    int id3 = ztab_assign(&t, mac3);
    TEST_ASSERT_EQUAL_INT(3, id3);
    TEST_ASSERT_EQUAL_UINT8(ZTAB_F_ASSIGNED | ZTAB_F_UNCONFIGURED, t.e[2].flags);
}

static void test_assign_skips_occupied_ids(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};
    uint8_t mac3[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x03};

    int id1 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id1);

    int id2 = ztab_assign(&t, mac2);
    TEST_ASSERT_EQUAL_INT(2, id2);

    /* Clear id 1 */
    ztab_clear(&t, 1);

    /* Assign new MAC should get id 1 (lowest-free) */
    int id3 = ztab_assign(&t, mac3);
    TEST_ASSERT_EQUAL_INT(1, id3);
}

static void test_find_mac(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac_notfound[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    ztab_assign(&t, mac1);

    int slot = ztab_find_mac(&t, mac1);
    TEST_ASSERT_EQUAL_INT(0, slot);

    int not_found = ztab_find_mac(&t, mac_notfound);
    TEST_ASSERT_EQUAL_INT(-1, not_found);
}

static void test_find_id(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};

    int id = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id);

    int slot = ztab_find_id(&t, 1);
    TEST_ASSERT_EQUAL_INT(0, slot);

    int not_found = ztab_find_id(&t, 99);
    TEST_ASSERT_EQUAL_INT(-1, not_found);
}

static void test_reassign_same_mac_returns_existing_id(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};

    int id1 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id1);

    int id2 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id2);
}

static void test_assign_table_full_returns_minus_one(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t macs[8][6];
    for (int i = 0; i < 8; i++) {
        memset(macs[i], i + 1, 6);
        int id = ztab_assign(&t, macs[i]);
        TEST_ASSERT_EQUAL_INT(i + 1, id);
    }

    uint8_t mac_new[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int id_full = ztab_assign(&t, mac_new);
    TEST_ASSERT_EQUAL_INT(-1, id_full);
}

static void test_set_name_15_chars_ok(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);

    const char *name15 = "123456789012345";  /* 15 chars */
    int ret = ztab_set_name(&t, id, name15);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING(name15, t.e[0].name);
}

static void test_set_name_16_chars_rejected(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);

    const char *name16 = "1234567890123456";  /* 16 chars, too long */
    int ret = ztab_set_name(&t, id, name16);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_set_name_unknown_id_rejected(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    const char *name = "test";
    int ret = ztab_set_name(&t, 99, name);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_clear_entry(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id);

    ztab_clear(&t, 1);

    /* Entry should be zeroed */
    uint8_t zero_entry[24];
    memset(zero_entry, 0, 24);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero_entry, (uint8_t *)&t.e[0], 24);
}

static void test_enrol_known_mac_right_id_returns_known(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id);

    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac1, 1, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_KNOWN, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);
}

static void test_enrol_known_mac_wrong_id_returns_stale(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id);

    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac1, 2, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_STALE, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);  /* Returns its assigned id */
}

static void test_enrol_known_mac_0xfe_returns_stale(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id);

    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac1, 0xFE, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_STALE, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);
}

static void test_enrol_unknown_mac_free_slot_returns_assigned(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};

    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac1, 0, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_ASSIGNED, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);
    TEST_ASSERT_EQUAL_UINT8(ZTAB_F_ASSIGNED | ZTAB_F_UNCONFIGURED, t.e[0].flags);
}

static void test_enrol_unknown_mac_table_full_returns_full(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    /* Fill table */
    for (int i = 0; i < 8; i++) {
        uint8_t mac[6];
        memset(mac, i + 1, 6);
        ztab_assign(&t, mac);
    }

    uint8_t mac_new[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac_new, 0, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_FULL, verdict);
}

static void test_enrol_conflict_claimed_id_owned_by_different_mac(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};

    int id1 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id1);

    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac2, 1, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_CONFLICT, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);

    /* Table should be unchanged - mac2 should NOT be in the table */
    int mac2_slot = ztab_find_mac(&t, mac2);
    TEST_ASSERT_EQUAL_INT(-1, mac2_slot);
}

static void test_pack_unpack_roundtrip(void) {
    ztab_t t_in, t_out;
    memset(&t_in, 0, sizeof(t_in));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};

    ztab_assign(&t_in, mac1);
    ztab_set_name(&t_in, 1, "Zone1");

    ztab_assign(&t_in, mac2);
    ztab_set_name(&t_in, 2, "Zone2");

    uint32_t gen = 42;
    uint8_t buf[256];
    int pack_ret = ztab_pack(&t_in, gen, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, pack_ret);

    uint32_t gen_out = 0;
    int unpack_ret = ztab_unpack(buf, pack_ret, &t_out, &gen_out);
    TEST_ASSERT_EQUAL_INT(0, unpack_ret);
    TEST_ASSERT_EQUAL_UINT32(gen, gen_out);

    /* Compare tables byte-for-byte */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((uint8_t *)&t_in, (uint8_t *)&t_out, sizeof(ztab_t));
}

static void test_pack_returns_correct_size(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    ztab_assign(&t, mac1);

    uint8_t buf[256];
    int pack_ret = ztab_pack(&t, 0, buf, sizeof(buf));
    /* Should be HG_BLOB_HDR_LEN (16) + sizeof(ztab_t) (192) = 208 */
    TEST_ASSERT_EQUAL_INT(16 + 192, pack_ret);
}

static void test_unpack_flipped_byte_rejects(void) {
    ztab_t t_in, t_out;
    memset(&t_in, 0, sizeof(t_in));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    ztab_assign(&t_in, mac1);

    uint8_t buf[256];
    int pack_ret = ztab_pack(&t_in, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, pack_ret);

    /* Flip a byte in the middle */
    buf[100] ^= 0xFF;

    uint32_t gen_out = 0;
    int unpack_ret = ztab_unpack(buf, pack_ret, &t_out, &gen_out);
    TEST_ASSERT_EQUAL_INT(-1, unpack_ret);
}

static void test_unpack_wrong_magic_rejects(void) {
    ztab_t t_in, t_out;
    memset(&t_in, 0, sizeof(t_in));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    ztab_assign(&t_in, mac1);

    uint8_t buf[256];
    int pack_ret = ztab_pack(&t_in, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, pack_ret);

    /* Flip magic bytes */
    buf[0] ^= 0xFF;

    uint32_t gen_out = 0;
    int unpack_ret = ztab_unpack(buf, pack_ret, &t_out, &gen_out);
    TEST_ASSERT_EQUAL_INT(-1, unpack_ret);
}

static void test_unpack_short_buffer_rejects(void) {
    ztab_t t_out;

    uint8_t buf[8];  /* Too short */
    memset(buf, 0xFF, sizeof(buf));

    uint32_t gen_out = 0;
    int unpack_ret = ztab_unpack(buf, sizeof(buf), &t_out, &gen_out);
    TEST_ASSERT_EQUAL_INT(-1, unpack_ret);
}

static void test_set_name_nul_padding(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    int id = ztab_assign(&t, mac1);

    const char *name = "abc";  /* 3 chars */
    int ret = ztab_set_name(&t, id, name);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify first 3 bytes are "abc" */
    TEST_ASSERT_EQUAL_UINT8('a', t.e[0].name[0]);
    TEST_ASSERT_EQUAL_UINT8('b', t.e[0].name[1]);
    TEST_ASSERT_EQUAL_UINT8('c', t.e[0].name[2]);

    /* Verify bytes 3..15 are NUL */
    uint8_t expected_padding[13];
    memset(expected_padding, 0, 13);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_padding, (uint8_t *)&t.e[0].name[3], 13);
}

static void test_enrol_stale_vs_conflict_precedence(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac_a[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac_b[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};

    /* Assign MAC A to id 1 */
    int id_a = ztab_assign(&t, mac_a);
    TEST_ASSERT_EQUAL_INT(1, id_a);

    /* Assign MAC B to id 2 */
    int id_b = ztab_assign(&t, mac_b);
    TEST_ASSERT_EQUAL_INT(2, id_b);

    /* MAC A claims id 2 (which is owned by B) */
    /* This must return STALE (known MAC with wrong id), not CONFLICT */
    /* Precedence: known-MAC check comes before claimed-id check */
    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac_a, 2, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_STALE, verdict);
    TEST_ASSERT_EQUAL_UINT8(1, out_id);  /* Returns A's assigned id */

    /* Verify table is unchanged - B still owns 2 */
    int b_slot = ztab_find_id(&t, 2);
    TEST_ASSERT_NOT_EQUAL(-1, b_slot);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mac_b, t.e[b_slot].mac, 6);
}

static void test_enrol_claiming_free_id_gets_lowest_free(void) {
    ztab_t t;
    memset(&t, 0, sizeof(t));

    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    uint8_t mac_new[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xFF};

    /* Assign first MAC to id 1 */
    int id1 = ztab_assign(&t, mac1);
    TEST_ASSERT_EQUAL_INT(1, id1);

    /* Unknown MAC claims id 5 (which is free), but ids 2,3,4 are also free */
    /* Must get lowest-free (id 2), not the claimed id */
    uint8_t out_id = 0;
    ztab_en_t verdict = ztab_enrol(&t, mac_new, 5, &out_id);
    TEST_ASSERT_EQUAL_INT(ZTAB_EN_ASSIGNED, verdict);
    TEST_ASSERT_EQUAL_UINT8(2, out_id);  /* Gets lowest-free, not claimed id 5 */

    /* Verify MAC is assigned to id 2 */
    int new_slot = ztab_find_mac(&t, mac_new);
    TEST_ASSERT_EQUAL_INT(1, new_slot);  /* Should be in slot 1 (id 2) */
    TEST_ASSERT_EQUAL_UINT8(2, t.e[new_slot].id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_assign_fills_1_to_8_in_order);
    RUN_TEST(test_assign_skips_occupied_ids);
    RUN_TEST(test_find_mac);
    RUN_TEST(test_find_id);
    RUN_TEST(test_reassign_same_mac_returns_existing_id);
    RUN_TEST(test_assign_table_full_returns_minus_one);
    RUN_TEST(test_set_name_15_chars_ok);
    RUN_TEST(test_set_name_16_chars_rejected);
    RUN_TEST(test_set_name_unknown_id_rejected);
    RUN_TEST(test_clear_entry);
    RUN_TEST(test_enrol_known_mac_right_id_returns_known);
    RUN_TEST(test_enrol_known_mac_wrong_id_returns_stale);
    RUN_TEST(test_enrol_known_mac_0xfe_returns_stale);
    RUN_TEST(test_enrol_unknown_mac_free_slot_returns_assigned);
    RUN_TEST(test_enrol_unknown_mac_table_full_returns_full);
    RUN_TEST(test_enrol_conflict_claimed_id_owned_by_different_mac);
    RUN_TEST(test_pack_unpack_roundtrip);
    RUN_TEST(test_pack_returns_correct_size);
    RUN_TEST(test_unpack_flipped_byte_rejects);
    RUN_TEST(test_unpack_wrong_magic_rejects);
    RUN_TEST(test_unpack_short_buffer_rejects);
    RUN_TEST(test_set_name_nul_padding);
    RUN_TEST(test_enrol_stale_vs_conflict_precedence);
    RUN_TEST(test_enrol_claiming_free_id_gets_lowest_free);
    return UNITY_END();
}
