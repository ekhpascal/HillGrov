#include "cmd_core.h"

int cmd_help(const cmd_core_t *core, cmd_session_t *ses, const char *const *tok, int ntok, char *resp, int len) {
    (void)core; (void)ses; (void)tok; (void)ntok;
    return cmd_err(resp, len, "NOT_IMPLEMENTED");
}

int cmd_table_check(const cmd_entry_t *t, int n) {
    (void)t; (void)n;
    return -1;
}
