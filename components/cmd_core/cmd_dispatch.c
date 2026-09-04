#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmd_core.h"

/* SP3 spec §5.3: the caller's own wait on cmd_task_execute() is bounded to
 * 3500 ms; the forward hook gets the same budget so a forwarded call can
 * never outlive the request that made it. */
#define ZONE_FWD_TIMEOUT_MS 3500u

static const cmd_entry_t *match(const cmd_core_t *c, uint8_t vbit, const char *n1, const char *n2, int two) {
    for (int i = 0; i < c->table_len; i++) {
        const cmd_entry_t *e = &c->table[i];
        if (!(e->verbs & vbit)) continue;
        if (two) {
            if (e->noun2 && cmd_ci_eq(e->noun1, n1) && cmd_ci_eq(e->noun2, n2)) return e;
        } else {
            if (!e->noun2 && cmd_ci_eq(e->noun1, n1)) return e;
        }
    }
    return NULL;
}

static int parse_int_arg(const char *s, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) return -1;
    *out = v;
    return 0;
}

static int parse_mac(const char *s, uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        char *end;
        long v = strtol(s, &end, 16);
        if (end != s + 2 || v < 0 || v > 255) return -1;
        mac[i] = (uint8_t)v;
        s = end;
        if (i < 5 && *s++ != ':') return -1;
    }
    return *s == '\0' ? 0 : -1;
}

static int enum_index(const char *enums, const char *val, long *out) {
    long idx = 0;
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char name[20];
        if (n < sizeof name) {
            memcpy(name, p, n); name[n] = '\0';
            if (cmd_ci_eq(name, val)) { *out = idx; return 0; }
        }
        if (!bar) break;
        p = bar + 1; idx++;
    }
    /* idx is now the highest valid index (entry count - 1). A single-entry
     * enum -- e.g. REBOOT's A_CONF {"CONFIRM"} -- must reject the numeric
     * fallback entirely: "REBOOT 0" is not a confirmation, and index 0 is
     * both the only and the default-looking value, so letting it through
     * defeats the destructive-row confirm gate. */
    if (idx == 0) return -1;
    long v;
    if (parse_int_arg(val, &v) == 0 && v >= 0 && v <= idx) { *out = v; return 0; }
    return -1;
}

int cmd_dispatch(const cmd_core_t *core, cmd_session_t *ses, const char *line, char *resp, int resp_len) {
    resp[0] = '\0';
    if (strlen(line) >= CMD_LINE_MAX) return cmd_err(resp, resp_len, "TOO_LONG");
    char buf[CMD_LINE_MAX];
    strcpy(buf, line);
    const char *tok[CMD_MAX_TOKENS];
    int ntok = cmd_tokenize(buf, tok, CMD_MAX_TOKENS);
    if (ntok < 0) return cmd_err(resp, resp_len, "BAD_ARGS");
    if (ntok == 0) return cmd_err(resp, resp_len, "EMPTY");

    /* ZONE <z> address prefix: <VERB> ZONE <z> <tail...> */
    char tail[CMD_LINE_MAX];
    if (ntok >= 3 && cmd_ci_eq(tok[1], "ZONE")) {
        long z;
        if (parse_int_arg(tok[2], &z) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
        if (z < 0 || z > 8) return cmd_err(resp, resp_len, "OUT_OF_RANGE");
        if (ntok < 4) return cmd_err(resp, resp_len, "BAD_ARGS");
        size_t off = (size_t)snprintf(tail, sizeof tail, "%s", tok[0]);
        for (int i = 3; i < ntok; i++)
            off += (size_t)snprintf(tail + off, sizeof tail - off, " %s", tok[i]);
        if (core->role == CMD_ROLE_MASTER && z != 0) {
            if (!core->forward) return cmd_err(resp, resp_len, "ZONE_UNKNOWN");
            return core->forward((uint8_t)z, tail, resp, resp_len, ZONE_FWD_TIMEOUT_MS);
        }
        if (core->role == CMD_ROLE_ZONE && z != 0 && z != core->zone_id)
            return cmd_err(resp, resp_len, "WRONG_ZONE");
        return cmd_dispatch(core, ses, tail, resp, resp_len);   /* re-enter with the tail */
    }

    if (cmd_ci_eq(tok[ntok - 1], "HELP"))
        return cmd_help(core, ses, tok, ntok - 1, resp, resp_len);

    uint8_t vbit;
    int noun0;                                   /* index of noun1 */
    if (cmd_ci_eq(tok[0], "SET")) { vbit = CMDV_SET; noun0 = 1; }
    else if (cmd_ci_eq(tok[0], "GET")) { vbit = CMDV_GET; noun0 = 1; }
    else { vbit = CMDV_BARE; noun0 = 0; }
    if (vbit != CMDV_BARE && ntok < 2) return cmd_err(resp, resp_len, "UNKNOWN_CMD");

    const cmd_entry_t *e = NULL;
    int args0 = 0;
    int noun2_pos = -1;  /* position of noun2 if matched with one arg in between */

    if (ntok > noun0 + 1) {
        e = match(core, vbit, tok[noun0], tok[noun0 + 1], 1);
        if (e) args0 = noun0 + 2;
    }
    /* Try two-word noun with exactly one argument between them: noun0, arg, noun2 */
    if (!e && ntok > noun0 + 2) {
        e = match(core, vbit, tok[noun0], tok[noun0 + 2], 1);
        if (e) {
            args0 = noun0 + 1;
            noun2_pos = noun0 + 2;
        }
    }
    if (!e) {
        e = match(core, vbit, tok[noun0], NULL, 0);
        if (e) args0 = noun0 + 1;
    }
    if (!e) return cmd_err(resp, resp_len, "UNKNOWN_CMD");

    if ((e->flags & CMDF_MASTER) && core->role != CMD_ROLE_MASTER) return cmd_err(resp, resp_len, "MASTER_ONLY");
    if ((e->flags & CMDF_ZONE) && core->role != CMD_ROLE_ZONE) return cmd_err(resp, resp_len, "ZONE_ONLY");
    if ((e->flags & CMDF_SESSION) && ses->source == CMD_SRC_HTTP) return cmd_err(resp, resp_len, "NOT_LOCAL");
    if (e->flags & CMDF_UNLOCK) {
        uint32_t now = core->now_ms();
        if (!(ses->unlock_until_ms > now)) return cmd_err(resp, resp_len, "LOCKED");
    }

    cmd_req_t q = { .core = core, .ses = ses, .e = e, .verb = vbit };
    int n = ntok - args0;
    if (noun2_pos >= 0) n--;  /* account for noun2 token in the count */
    int want_min = (vbit == CMDV_GET) ? e->n_key : e->min_args;
    int want_max = (vbit == CMDV_GET) ? e->n_key : e->max_args;
    if (n < want_min || n > want_max) return cmd_err(resp, resp_len, "BAD_ARGS");
    q.n = (uint8_t)n;

    int arg_idx = 0;
    for (int i = args0; i < ntok && arg_idx < q.n; i++) {
        if (i == noun2_pos) continue;  /* skip the noun2 token */
        const cmd_arg_t *a = &e->args[arg_idx];
        q.tok[arg_idx] = tok[i];
        q.val[arg_idx] = 0;
        long v;
        switch (a->type) {
        case ARG_INT:
            if (parse_int_arg(q.tok[arg_idx], &v) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            if (v < a->min || v > a->max) return cmd_err(resp, resp_len, "OUT_OF_RANGE");
            q.val[arg_idx] = (int32_t)v;
            break;
        case ARG_ENUM:
            if (enum_index(a->enums, q.tok[arg_idx], &v) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            q.val[arg_idx] = (int32_t)v;
            break;
        case ARG_STR:
            if ((int)strlen(q.tok[arg_idx]) > a->max) return cmd_err(resp, resp_len, "BAD_ARGS");
            break;
        case ARG_TIME: {
            int m = cmd_parse_time(q.tok[arg_idx]);
            if (m < 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            q.val[arg_idx] = m;
            break;
        }
        case ARG_MAC:
            if (parse_mac(q.tok[arg_idx], q.mac) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            break;
        default:
            return cmd_err(resp, resp_len, "INTERNAL");
        }
        arg_idx++;
    }

    int rc = e->handler(&q, resp, resp_len);
    if (resp[0] == '\0') return cmd_err(resp, resp_len, "INTERNAL");
    if (rc == 0 && (e->flags & CMDF_ACTUATOR) && vbit == CMDV_SET && core->audit)
        core->audit(core->audit_ctx, ses->source, line);
    if (ses->unlock_until_ms > core->now_ms())
        ses->unlock_until_ms = core->now_ms() + CMD_UNLOCK_MS;
    return rc;
}
