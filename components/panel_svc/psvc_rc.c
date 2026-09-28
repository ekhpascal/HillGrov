#include "psvc_rc.h"

/* Enum order, one per value. */
static const char *const TOK[] = {
    "OK", "BUSY", "INVALID", "BAD_JSON", "INVALID_FIELD", "VALIDATION",
    "NO_CACHE", "ZONE_UNKNOWN", "ZONE_NOT_ONLINE", "STORAGE", "INTERNAL",
    "FLEET_BUSY", "FLEET_REJECTED", "NOT_ACTIVE",
    "UPLOAD_ACTIVE", "FLEET_ACTIVE", "TRIAL_PENDING", "NO_SLOT", "LOW_HEAP",
    "TOO_LARGE", "IMAGE_MISMATCH", "WRITE_FAILED", "STALLED", "RECV_FAILED",
    "ZONE_FW_BUSY",
};
_Static_assert(sizeof TOK / sizeof TOK[0] == PSVC_RC_COUNT, "one token per psvc_rc_t, in enum order");

const char *psvc_rc_token(psvc_rc_t rc) {
    unsigned i = (unsigned)rc;   /* a negative value wraps high and lands in the fallback */
    return i < (unsigned)PSVC_RC_COUNT ? TOK[i] : "INTERNAL";
}

int psvc_rc_to_net_legacy(psvc_rc_t rc) {
    switch (rc) {
    case PSVC_OK:
        return 0;
    case PSVC_E_INVALID:
    case PSVC_E_VALIDATION:
    case PSVC_E_INVALID_FIELD:
    case PSVC_E_BAD_JSON:
    case PSVC_E_ZONE_UNKNOWN:
        return -1;
    case PSVC_E_BUSY:
    case PSVC_E_STORAGE:
        return -2;
    default:
        return -3;
    }
}
