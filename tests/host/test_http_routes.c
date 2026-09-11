#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "http_routes.h"

/* Task 11 Step 1: the route table + matcher is the product's security
 * boundary -- every request either matches a row (and then the row's auth bit
 * decides whether the cookie is checked) or is a 404. These tests pin the
 * table's shape so a later task cannot add an /api/ route that silently
 * defaults to auth 0. */

void setUp(void) {}
void tearDown(void) {}

static void test_table_length_matches_route_count(void) {
    /* One row per route_id_t, in id order -- http_srv.c registers HTTP_ROUTES[i]
     * and dispatches on .id, so a missing or duplicated row would leave a
     * handler unreachable (or, worse, registered under the wrong auth bit). */
    TEST_ASSERT_EQUAL_INT(RT_COUNT, HTTP_ROUTES_N);
    for (int i = 0; i < HTTP_ROUTES_N; i++) {
        TEST_ASSERT_EQUAL_INT(i, (int)HTTP_ROUTES[i].id);
        TEST_ASSERT_NOT_NULL(HTTP_ROUTES[i].method);
        TEST_ASSERT_NOT_NULL(HTTP_ROUTES[i].path);
        TEST_ASSERT_EQUAL_INT('/', HTTP_ROUTES[i].path[0]);
    }
}

static void test_query_string_is_stripped_before_matching(void) {
    int auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_STATE, http_route_find("GET", "/api/state?x=1", &auth));
    TEST_ASSERT_EQUAL_INT(1, auth);
    auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_CONFIG_GET, http_route_find("GET", "/api/config?zone=2", &auth));
    TEST_ASSERT_EQUAL_INT(1, auth);
    /* a bare '?' with nothing after it is still the same path */
    TEST_ASSERT_EQUAL_INT(RT_STATE, http_route_find("GET", "/api/state?", NULL));
}

static void test_fragment_is_stripped_before_matching(void) {
    /* A conforming client never puts a fragment on the wire, so this is
     * defence in depth: whatever arrives, the path is what precedes the first
     * '?' OR '#', and never leaks into the comparison. */
    TEST_ASSERT_EQUAL_INT(RT_STATE, http_route_find("GET", "/api/state#frag", NULL));
    TEST_ASSERT_EQUAL_INT(RT_STATE, http_route_find("GET", "/api/state?x=1#frag", NULL));
    TEST_ASSERT_EQUAL_INT(RT_STATE, http_route_find("GET", "/api/state#", NULL));
    TEST_ASSERT_EQUAL_INT(RT_INDEX, http_route_find("GET", "/#x", NULL));
    /* and it must not turn a non-route into one */
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api/statex#/api/state", NULL));
}

/* Fix round 1, IMPORTANT 4: the CLI reply -> HTTP status mapping lives here
 * because it is pure string work on a reply line, and because "which status
 * does ERR BUSY get" is a contract worth pinning in a test rather than in a
 * chain of strncmps nobody reads. */
static void test_reply_status_mapping(void) {
    TEST_ASSERT_EQUAL_INT(200, http_reply_status("OK NODES 2\n"));
    TEST_ASSERT_EQUAL_INT(200, http_reply_status("OK\n"));
    TEST_ASSERT_EQUAL_INT(200, http_reply_status("OK"));
    /* cmd_task's own pool exhaustion is a server-capacity answer, not a bad request */
    TEST_ASSERT_EQUAL_INT(503, http_reply_status("ERR BUSY\n"));
    TEST_ASSERT_EQUAL_INT(503, http_reply_status("ERR BUSY"));
    /* a dispatch that never completed is a server fault */
    TEST_ASSERT_EQUAL_INT(500, http_reply_status("ERR INTERNAL\n"));
    TEST_ASSERT_EQUAL_INT(500, http_reply_status("ERR INTERNAL detail\n"));
    /* everything else the operator asked for wrongly */
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("ERR NOT_LOCAL\n"));
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("ERR ZONE_OFFLINE\n"));
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("ERR\n"));
    /* token boundaries: a longer token that merely starts with one of ours */
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("ERR BUSYNESS\n"));
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("ERR INTERNALS\n"));
    /* "OK" is a prefix rule on purpose (OK<anything>), but ERR is not OK */
    TEST_ASSERT_EQUAL_INT(422, http_reply_status(""));
    TEST_ASSERT_EQUAL_INT(422, http_reply_status("O"));
    TEST_ASSERT_EQUAL_INT(422, http_reply_status(NULL));
}

static void test_login_is_the_only_unauthenticated_api_route(void) {
    int auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_LOGIN, http_route_find("POST", "/api/login", &auth));
    TEST_ASSERT_EQUAL_INT(0, auth);

    for (int i = 0; i < HTTP_ROUTES_N; i++) {
        if (strncmp(HTTP_ROUTES[i].path, "/api/", 5) != 0) continue;
        int want = (HTTP_ROUTES[i].id == RT_LOGIN) ? 0 : 1;
        char msg[96];
        snprintf(msg, sizeof msg, "%s %s auth", HTTP_ROUTES[i].method, HTTP_ROUTES[i].path);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, HTTP_ROUTES[i].auth, msg);
    }
}

static void test_method_must_match_exactly(void) {
    /* GET /api/login is not a row: a browser typing the URL must 404, not be
     * handed the login handler with an empty body. */
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api/login", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("PUT", "/api/state", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("DELETE", "/", NULL));
    /* the two methods that DO share a path stay separated */
    TEST_ASSERT_EQUAL_INT(RT_CONFIG_GET, http_route_find("GET", "/api/config", NULL));
    TEST_ASSERT_EQUAL_INT(RT_CONFIG_PUT, http_route_find("PUT", "/api/config", NULL));
    TEST_ASSERT_EQUAL_INT(RT_FLEET_POST, http_route_find("POST", "/api/fleet", NULL));
    TEST_ASSERT_EQUAL_INT(RT_FLEET_DELETE, http_route_find("DELETE", "/api/fleet", NULL));
}

static void test_static_and_fleet_paths_need_no_cookie(void) {
    int auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_INDEX, http_route_find("GET", "/", &auth));
    TEST_ASSERT_EQUAL_INT(0, auth);
    auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_APP_JS, http_route_find("GET", "/app.js", &auth));
    TEST_ASSERT_EQUAL_INT(0, auth);
    auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_APP_CSS, http_route_find("GET", "/app.css", &auth));
    TEST_ASSERT_EQUAL_INT(0, auth);
    /* SP3 rescue pulls carry no cookie: the AP password is this path's gate */
    auth = -1;
    TEST_ASSERT_EQUAL_INT(RT_FLEET_BIN, http_route_find("GET", "/fw/zone.bin", &auth));
    TEST_ASSERT_EQUAL_INT(0, auth);
}

static void test_prefix_and_suffix_paths_do_not_match(void) {
    /* exact-match only: no prefix matching, no trailing-slash tolerance, so
     * nothing outside the table can inherit a row's auth bit */
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api/state/", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api/statex", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/api/", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/index.html", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/fw/zone.bin.gz", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "", NULL));
}

static void test_auth_out_untouched_on_no_match(void) {
    int auth = 7;
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", "/nope", &auth));
    TEST_ASSERT_EQUAL_INT(7, auth);
    /* NULL method/uri must not crash the httpd worker */
    TEST_ASSERT_EQUAL_INT(-1, http_route_find(NULL, "/", NULL));
    TEST_ASSERT_EQUAL_INT(-1, http_route_find("GET", NULL, NULL));
}

static void test_every_expected_route_is_present(void) {
    /* the full brief table, spelled out once so a renamed path fails here */
    struct { const char *m, *p; int id; } want[] = {
        { "POST",   "/api/login",     RT_LOGIN },
        { "POST",   "/api/logout",    RT_LOGOUT },
        { "POST",   "/api/password",  RT_PASSWORD },
        { "POST",   "/api/cmd",       RT_CMD },
        { "GET",    "/api/help",      RT_HELP },
        { "GET",    "/api/state",     RT_STATE },
        { "GET",    "/api/schema",    RT_SCHEMA },
        { "GET",    "/api/config",    RT_CONFIG_GET },
        { "PUT",    "/api/config",    RT_CONFIG_PUT },
        { "GET",    "/api/alarms",    RT_ALARMS },
        { "POST",   "/api/fw/master", RT_FW_MASTER },
        { "POST",   "/api/fw/zone",   RT_FW_ZONE },
        { "POST",   "/api/fleet",     RT_FLEET_POST },
        { "DELETE", "/api/fleet",     RT_FLEET_DELETE },
        { "GET",    "/api/wifi/scan", RT_WIFI_SCAN },
        { "POST",   "/api/wifi",      RT_WIFI_SET },
        { "GET",    "/",              RT_INDEX },
        { "GET",    "/app.js",        RT_APP_JS },
        { "GET",    "/app.css",       RT_APP_CSS },
        { "GET",    "/fw/zone.bin",   RT_FLEET_BIN },
    };
    TEST_ASSERT_EQUAL_INT(RT_COUNT, (int)(sizeof want / sizeof want[0]));
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        TEST_ASSERT_EQUAL_INT(want[i].id, http_route_find(want[i].m, want[i].p, NULL));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_table_length_matches_route_count);
    RUN_TEST(test_query_string_is_stripped_before_matching);
    RUN_TEST(test_fragment_is_stripped_before_matching);
    RUN_TEST(test_reply_status_mapping);
    RUN_TEST(test_login_is_the_only_unauthenticated_api_route);
    RUN_TEST(test_method_must_match_exactly);
    RUN_TEST(test_static_and_fleet_paths_need_no_cookie);
    RUN_TEST(test_prefix_and_suffix_paths_do_not_match);
    RUN_TEST(test_auth_out_untouched_on_no_match);
    RUN_TEST(test_every_expected_route_is_present);
    return UNITY_END(); }
