#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "psvc_fw.h"
#include "psvc_rc.h"
#include "http_upload.h"
#include "http_srv_internal.h"

static const char *TAG = "http_upload";

/* rescue's rule verbatim: this many CONSECUTIVE recv timeouts (~60 s of no data at all at httpd's 5 s
 * recv_wait_timeout) gives up rather than letting a client that walked out of AP range mid-upload pin the single httpd
 * task with a half-written slot open. */
#define MAX_TIMEOUTS 12

static int type_is_octet_stream(httpd_req_t *req) {
    char ct[48];
    /* TRUNC means the value was longer than this buffer -- a legal header with a long parameter list. The 24-byte type
     * prefix is fully present either way, which is all this compares, so a truncated read is accepted (fix round 1). */
    esp_err_t rc = httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct);
    if (rc != ESP_OK && rc != ESP_ERR_HTTPD_RESULT_TRUNC) return 0;
    static const char want[] = "application/octet-stream";
    size_t n = sizeof want - 1;
    if (strncasecmp(ct, want, n) != 0) return 0;
    return ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ';   /* parameters are allowed */
}

/* Reads away the rest of a body this handler has already decided to refuse, so the answer goes into a quiet socket (the
 * Task 13 bench finding: closing on a still-streaming client RSTs the connection and the peer loses the response body).
 * 1 = fully consumed, 0 = the peer stalled or went away. Two bounds: 3 consecutive timeouts (~15 s) and a 10 s wall-clock
 * budget, because either alone is escapable (fix round 1). It runs after the install core released its TWDT
 * subscription, so the kick below is a silent no-op; the budget is the bound. */
#define DRAIN_MAX_TIMEOUTS 3
#define DRAIN_BUDGET_US    (10 * 1000 * 1000LL)
#define DRAIN_BUF          1024u

static int drain_body(httpd_req_t *req, uint8_t *buf, size_t cap, size_t got) {
    int timeouts = 0;
    int64_t deadline = esp_timer_get_time() + DRAIN_BUDGET_US;
    while (got < req->content_len) {
        psvc_fw_wdt_kick();
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "drain budget spent with %u B still unread -- closing", (unsigned)(req->content_len - got));
            return 0;
        }
        size_t want = req->content_len - got;
        if (want > cap) want = cap;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > DRAIN_MAX_TIMEOUTS) return 0;
            continue;
        }
        if (n <= 0) return 0;
        timeouts = 0;
        got += (size_t)n;
    }
    return 1;
}

/* The install core's byte source for an HTTP body. */
typedef struct { httpd_req_t *req; int timeouts; const char *kind; } recv_src_t;

static int recv_src(void *src, void *buf, size_t cap) {
    recv_src_t *s = (recv_src_t *)src;
    int n = httpd_req_recv(s->req, (char *)buf, cap);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++s->timeouts > MAX_TIMEOUTS) {
            ESP_LOGW(TAG, "%s upload stalled (~%d s of silence), aborting", s->kind, MAX_TIMEOUTS * 5);
            return PSVC_FW_SRC_STALLED;
        }
        return PSVC_FW_SRC_AGAIN;
    }
    if (n <= 0) return PSVC_FW_SRC_FAILED;
    s->timeouts = 0;
    return n;
}

static int status_for(psvc_rc_t rc) {
    switch (rc) {
    case PSVC_E_UPLOAD_ACTIVE: case PSVC_E_FLEET_ACTIVE: case PSVC_E_TRIAL_PENDING: case PSVC_E_ZONE_FW_BUSY: return 409;
    case PSVC_E_LOW_HEAP:                                                                                   return 503;
    case PSVC_E_TOO_LARGE:                                                                                  return 413;
    case PSVC_E_STALLED: case PSVC_E_RECV_FAILED:                                                           return 400;
    case PSVC_E_IMAGE_MISMATCH: case PSVC_E_WRITE_FAILED:                                                   return 422;
    default:                                                                                                return 500;
    }
}

/* Every exit answers exactly once and returns through http_srv_done(): drained = 1 only when the whole body was read,
 * so a refusal with a body still on the wire closes the socket instead of letting httpd purge megabytes. */
static esp_err_t fw_upload(httpd_req_t *req, psvc_fw_kind_t kind) {
    const char *kname = kind == PSVC_FW_MASTER ? "master" : "zone";
    /* Framing first, before anything is claimed (fix round 1): malformed whatever the target's state is. */
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0) {
        http_srv_error(req, 400, "CHUNKED_UNSUPPORTED", NULL);
        return http_srv_done(req, 0);
    }
    if (!type_is_octet_stream(req)) {
        http_srv_error(req, 400, "BAD_TYPE", NULL);
        return http_srv_done(req, 0);
    }
    if (req->content_len == 0) {
        http_srv_error(req, 400, "EMPTY_BODY", NULL);
        return http_srv_done(req, 0);
    }

    ESP_LOGW(TAG, "%s upload: %u B", kname, (unsigned)req->content_len);
    recv_src_t src = { .req = req, .timeouts = 0, .kind = kname };
    psvc_fw_result_t res;
    psvc_fw_stats_t st;
    psvc_rc_t rc = psvc_fw_install(kind, req->content_len, recv_src, &src, &res, &st);

    if (rc != PSVC_OK) {
        ESP_LOGE(TAG, "%s upload refused after %u/%u B: %s", kname, (unsigned)st.consumed,
                 (unsigned)req->content_len, psvc_rc_token(rc));
        int drained = st.consumed == req->content_len;
        /* A guard refusal (started == 0) closes without reading: content_len is whatever the client claimed, and reading
         * megabytes away to be polite would hand the single httpd task to any logged-in client. Past the guards, drain
         * BEFORE answering (curl stops sending once it sees an error status), unless the socket itself failed. */
        if (!drained && st.started && !st.src_failed) {
            /* Transient, not static: the drain is its only user, and a permanent buffer would come out of the ESP32
             * master's internal-RAM margin (the install core's 4 KB one is private to it now). A failed allocation
             * closes instead of draining -- the status line still reaches the client. */
            uint8_t *dbuf = malloc(DRAIN_BUF);
            if (dbuf) {
                drained = drain_body(req, dbuf, DRAIN_BUF, st.consumed);
                free(dbuf);
            }
        }
        http_srv_error(req, status_for(rc), psvc_rc_token(rc), NULL);
        return http_srv_done(req, drained);
    }

    char resp[128];
    if (kind == PSVC_FW_MASTER)
        snprintf(resp, sizeof resp, "{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\"}", res.slot, res.version);
    else
        snprintf(resp, sizeof resp, "{\"ok\":true,\"len\":%lu}", (unsigned long)res.len);
    http_srv_json(req, 200, resp);
    return http_srv_done(req, 1);
}

esp_err_t h_fw_master(httpd_req_t *req) { return fw_upload(req, PSVC_FW_MASTER); }

esp_err_t h_fw_zone(httpd_req_t *req) { return fw_upload(req, PSVC_FW_ZONE); }
