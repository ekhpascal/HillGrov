#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cmd_core.h"

static const char *AREA_NAMES[CMD_AREA_COUNT] = {
    "SYSTEM", "CONFIG", "SESSION", "TIME", "FIRMWARE", "RING",
    "SHELF", "SENSORS", "FAULTS", "NETWORK", "DEBUG",
};

/* Verb-bit render order: SET before GET; BARE never co-occurs with either
 * (cmd_table_check rejects that combination). */
static const uint8_t VERB_ORDER[3] = { CMDV_SET, CMDV_GET, CMDV_BARE };

static int row_visible(const cmd_core_t *core, const cmd_entry_t *e) {
    if ((e->flags & CMDF_MASTER) && core->role != CMD_ROLE_MASTER) return 0;
    if ((e->flags & CMDF_ZONE) && core->role != CMD_ROLE_ZONE) return 0;
    return 1;
}

/* Bounded append: writes fmt at buf[*off] within buf[0..cap), advances *off
 * past what was actually written. snprintf/vsnprintf return the length they
 * WOULD have written even when truncating, so accumulating that return value
 * unclamped can push *off past cap on a later call, making buf+*off an
 * out-of-bounds pointer and cap-*off underflow (huge size_t). This clamps
 * *off to cap-1 (the NUL position) on truncation or when already full, so
 * buf remains NUL-terminated and every write stays in bounds; further calls
 * become safe no-ops. */
static void sappend(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (cap == 0 || *off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w < 0) return;                          /* encoding error: leave buf as-is (already NUL-terminated) */
    if ((size_t)w >= cap - *off) { *off = cap - 1; return; }  /* truncated; vsnprintf still NUL-terminated at buf[cap-1] */
    *off += (size_t)w;
}

static void render_arg(char *out, int cap, const cmd_arg_t *a) {
    switch (a->type) {
    case ARG_INT:  snprintf(out, (size_t)cap, "%s %ld-%ld", a->name, (long)a->min, (long)a->max); break;
    case ARG_ENUM: snprintf(out, (size_t)cap, "%s", a->enums); break;
    case ARG_STR:  snprintf(out, (size_t)cap, "%s", a->name); break;
    case ARG_TIME: snprintf(out, (size_t)cap, "%s HH:MM", a->name); break;
    case ARG_MAC:  snprintf(out, (size_t)cap, "%s xx:xx:xx:xx:xx:xx", a->name); break;
    default:       out[0] = '\0'; break;
    }
}

/* Builds one "+ ..." usage line for entry e under verb bit vbit
 * (CMDV_SET / CMDV_GET / CMDV_BARE). GET shows only the first n_key args.
 * A two-word row with n_key >= 1 is "split-shape": its key args print
 * between noun1 and noun2 (e.g. "SET NODE <zone 1-8> NAME <name>"); a
 * two-word row with n_key == 0 is "adjacent" (noun1 noun2 <all args>),
 * e.g. "SET FW ROLLBACK <CONFIRM>".
 * Deliberately not `static`: not part of the public API (not in
 * cmd_core.h), but host tests link against it directly to exercise the
 * sappend() buffer-clamping on a small, test-controlled cap. */
void render_usage(const cmd_entry_t *e, uint8_t vbit, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    size_t off = 0;
    sappend(out, (size_t)cap, &off, "+ %s%s",
            vbit == CMDV_SET ? "SET " : vbit == CMDV_GET ? "GET " : "",
            e->noun1);
    int nargs  = (vbit == CMDV_GET) ? e->n_key   : e->max_args;
    int minidx = (vbit == CMDV_GET) ? e->n_key   : e->min_args;
    int split  = e->noun2 && e->n_key >= 1;
    int keyn   = split ? (e->n_key < nargs ? e->n_key : nargs) : 0;

    int i = 0;
    for (; i < keyn; i++) {
        char a[40]; render_arg(a, (int)sizeof a, &e->args[i]);
        sappend(out, (size_t)cap, &off, i >= minidx ? " [<%s>]" : " <%s>", a);
    }
    if (e->noun2) sappend(out, (size_t)cap, &off, " %s", e->noun2);
    for (; i < nargs; i++) {
        char a[40]; render_arg(a, (int)sizeof a, &e->args[i]);
        sappend(out, (size_t)cap, &off, i >= minidx ? " [<%s>]" : " <%s>", a);
    }
    if (e->flags & CMDF_UNLOCK) sappend(out, (size_t)cap, &off, " [unlock]");
    if (vbit != CMDV_GET && e->desc) sappend(out, (size_t)cap, &off, "  -- %s", e->desc);
}

int cmd_help(const cmd_core_t *core, cmd_session_t *ses, const char *const *tok, int ntok, char *resp, int len) {
    (void)ses;
    resp[0] = '\0';

    if (ntok == 0) {
        /* grammar header + one index line per area with visible rows */
        cmd_linef(resp, len, "+ SET|GET <NOUN> [args] | <VERB> ZONE <z 0-8> <command> | <prefix> HELP");
        for (int a = 0; a < CMD_AREA_COUNT; a++) {
            char nouns[160] = ""; size_t off = 0; int any = 0;
            for (int i = 0; i < core->table_len; i++) {
                const cmd_entry_t *e = &core->table[i];
                if (e->area != a || !row_visible(core, e)) continue;
                if (any) sappend(nouns, sizeof nouns, &off, ", ");
                if (e->noun2) sappend(nouns, sizeof nouns, &off, "%s %s", e->noun1, e->noun2);
                else          sappend(nouns, sizeof nouns, &off, "%s", e->noun1);
                any = 1;
            }
            if (any) cmd_linef(resp, len, "+ %s: %s -- <NOUN> HELP for details", AREA_NAMES[a], nouns);
        }
        return 0;
    }

    /* prefix match: SET/GET select the verb bit and noun filters start at tok[1];
     * any other leading word is treated as a BARE-row noun filter starting at tok[0]. */
    uint8_t vbit; int nstart;
    if (cmd_ci_eq(tok[0], "SET"))      { vbit = CMDV_SET;  nstart = 1; }
    else if (cmd_ci_eq(tok[0], "GET")) { vbit = CMDV_GET;  nstart = 1; }
    else                                { vbit = CMDV_BARE; nstart = 0; }
    const char *f1 = ntok > nstart     ? tok[nstart]     : NULL;
    const char *f2 = ntok > nstart + 1 ? tok[nstart + 1] : NULL;

    int found = 0;
    for (int i = 0; i < core->table_len; i++) {
        const cmd_entry_t *e = &core->table[i];
        if (!(e->verbs & vbit) || !row_visible(core, e)) continue;
        if (f1 && !cmd_ci_eq(e->noun1, f1)) continue;
        if (f2 && !(e->noun2 && cmd_ci_eq(e->noun2, f2))) continue;
        /* a matched row prints its full usage: one line per verb bit it carries */
        for (int v = 0; v < 3; v++) {
            if (!(e->verbs & VERB_ORDER[v])) continue;
            char line[224];
            render_usage(e, VERB_ORDER[v], line, (int)sizeof line);
            cmd_linef(resp, len, "%s", line);
        }
        found = 1;
    }
    if (!found) return cmd_err(resp, len, "UNKNOWN_CMD");
    return 0;
}

int cmd_table_check(const cmd_entry_t *t, int n) {
    for (int i = 0; i < n; i++) {
        const cmd_entry_t *e = &t[i];
        if (e->n_key > e->max_args) return i;
        if (e->min_args > e->max_args) return i;
        if (e->max_args > CMD_MAX_ARGS) return i;
        if (!e->handler) return i;

        int enum_bad = 0;
        for (int a = 0; a < e->max_args; a++)
            if (e->args[a].type == ARG_ENUM && !e->args[a].enums) enum_bad = 1;
        if (enum_bad) return i;

        if ((e->verbs & CMDV_BARE) && (e->verbs & (CMDV_SET | CMDV_GET))) return i;

        if (e->flags & CMDF_ACTUATOR) {
            int ok = 0;
            for (int a = 0; a < e->max_args; a++)
                if (e->args[a].type == ARG_INT &&
                    (cmd_ci_eq(e->args[a].name, "seconds") || cmd_ci_eq(e->args[a].name, "minutes"))) { ok = 1; break; }
            if (!ok) return i;
        }

        int too_long = 0;
        for (int v = 0; v < 3; v++) {
            if (!(e->verbs & VERB_ORDER[v])) continue;
            char line[224];
            render_usage(e, VERB_ORDER[v], line, (int)sizeof line);
            if ((int)strlen(line) > 96) too_long = 1;
        }
        if (too_long) return i;

        for (int j = 0; j < i; j++) {
            const cmd_entry_t *p = &t[j];
            if (!(p->verbs & e->verbs) || !cmd_ci_eq(p->noun1, e->noun1)) continue;
            int same_noun2 = (p->noun2 && e->noun2 && cmd_ci_eq(p->noun2, e->noun2)) || (!p->noun2 && !e->noun2);
            if (same_noun2) return i;
        }

        /* one-word/two-word shadow (Task 7 ruling): a same-noun1, verb-overlapping
         * one-word row whose max_args >= 2 collides with the adjacent two-word match
         * (its noun2 falls in the one-word row's first arg slot) and the split
         * two-word match at noun0+2 (its noun2 falls in the one-word row's second
         * arg slot) -- ambiguous dispatch for any table lacking runtime tie-breaking. */
        for (int j = 0; j < i; j++) {
            const cmd_entry_t *p = &t[j];
            if (!(p->verbs & e->verbs) || !cmd_ci_eq(p->noun1, e->noun1)) continue;
            int p_one = !p->noun2, e_one = !e->noun2;
            if (p_one == e_one) continue;
            const cmd_entry_t *one = p_one ? p : e;
            if (one->max_args >= 2) return i;
        }
    }
    return -1;
}
