#include <stdio.h>
#include <string.h>
#include "pnl_msg.h"

#define MCFG_BUSY "Master config busy (another change is being applied), retry"

static const char *tok_or(psvc_rc_t rc, const char *fallback) {
    const char *t = psvc_rc_token(rc);
    return (t && t[0]) ? t : fallback;
}

static void fw_fail(psvc_rc_t rc, const char *kind, char *out, size_t cap) {
    switch (rc) {
    case PSVC_E_IMAGE_MISMATCH: snprintf(out, cap, "Not a %s image -- nothing was erased", kind); break;
    case PSVC_E_TRIAL_PENDING:  snprintf(out, cap, "An OTA trial is running -- wait for it to pass or SET OTA CONFIRM"); break;
    case PSVC_E_FLEET_ACTIVE:   snprintf(out, cap, "A fleet update is running"); break;
    case PSVC_E_UPLOAD_ACTIVE:  snprintf(out, cap, "Another upload is in progress"); break;
    case PSVC_E_ZONE_FW_BUSY:   snprintf(out, cap, "A zone is downloading the image -- retry in a minute"); break;
    case PSVC_E_RECV_FAILED:    snprintf(out, cap, "microSD read failed"); break;
    default:                    snprintf(out, cap, "%s", tok_or(rc, "Failed")); break;
    }
}

int pnl_msg(pnl_msg_ctx_t ctx, psvc_rc_t rc, const pnl_msg_arg_t *arg, char *out, size_t cap) {
    static const pnl_msg_arg_t none = { 0, NULL, NULL, 0 };
    if (!out || cap == 0) return -1;
    const pnl_msg_arg_t *a = arg ? arg : &none;
    const char *ver  = (a->version && a->version[0]) ? a->version : "?";
    const char *slot = (a->slot && a->slot[0]) ? a->slot : "?";
    out[0] = '\0';
    switch (ctx) {
    case PNL_CTX_ZONE_LOAD:
        if (rc == PSVC_OK) snprintf(out, cap, "Loaded.");
        else if (rc == PSVC_E_NO_CACHE) snprintf(out, cap, "Zone config not adopted yet -- the zone must come online and sync at least once before it can be configured.");
        else if (rc == PSVC_E_ZONE_UNKNOWN) snprintf(out, cap, "Unknown zone.");
        else if (rc == PSVC_E_ZONE_NOT_ONLINE) snprintf(out, cap, "Zone is offline.");
        else if (rc == PSVC_E_LOW_HEAP) snprintf(out, cap, "Master is low on memory -- try again shortly.");
        else snprintf(out, cap, "%s", tok_or(rc, "Failed to load"));
        break;
    case PNL_CTX_ZONE_SAVE:
        if (rc == PSVC_OK) snprintf(out, cap, "Queued, pushing to zone");
        else if (rc == PSVC_E_BUSY) snprintf(out, cap, "Zone busy, retry");
        else if (rc == PSVC_E_ZONE_NOT_ONLINE) snprintf(out, cap, "Zone is offline -- nothing was saved");
        else snprintf(out, cap, "%s", tok_or(rc, "Save failed"));
        break;
    case PNL_CTX_MCFG_SAVE:
        if (rc == PSVC_OK) snprintf(out, cap, "Saved.");
        else if (rc == PSVC_E_BUSY) snprintf(out, cap, MCFG_BUSY);
        else snprintf(out, cap, "%s", tok_or(rc, "Save failed"));
        break;
    case PNL_CTX_WIFI_JOIN:
    case PNL_CTX_WIFI_AP:
    case PNL_CTX_SCAN:
    case PNL_CTX_TZ:
        if (rc == PSVC_OK) {
            snprintf(out, cap, "%s", ctx == PNL_CTX_WIFI_JOIN ? "Saved -- joining..." :
                                     ctx == PNL_CTX_WIFI_AP   ? "Saved." :
                                     ctx == PNL_CTX_SCAN      ? "Scan done." : "Time zone saved.");
        } else if (rc == PSVC_E_BUSY) {
            snprintf(out, cap, MCFG_BUSY);
        } else {
            snprintf(out, cap, "%s", tok_or(rc, ctx == PNL_CTX_SCAN ? "Scan failed" : "Failed"));
        }
        break;
    case PNL_CTX_PASSWORD:
        if (rc == PSVC_OK) snprintf(out, cap, "Password changed. Every phone and browser was logged out -- log in again with the new password.");
        else if (rc == PSVC_E_INVALID) snprintf(out, cap, "New password invalid (8 to 63 characters)");
        else snprintf(out, cap, "Failed (%s)", tok_or(rc, "INTERNAL"));
        break;
    case PNL_CTX_FLEET_ZONE:
        if (rc == PSVC_OK) snprintf(out, cap, "Update queued for zone %d.", a->zone);
        else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
        break;
    case PNL_CTX_FLEET_ALL:
        if (rc == PSVC_OK) snprintf(out, cap, "Fleet update queued.");
        else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
        break;
    case PNL_CTX_FLEET_ABORT:
        if (rc == PSVC_OK) snprintf(out, cap, "Fleet update aborted.");
        else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
        break;
    case PNL_CTX_FW_MASTER:
        if (rc == PSVC_OK) snprintf(out, cap, "Uploaded v%s to %s.", ver, slot);
        else fw_fail(rc, "master", out, cap);
        break;
    case PNL_CTX_FW_ZONE:
        if (rc == PSVC_OK) snprintf(out, cap, "Uploaded (%lu bytes).", (unsigned long)a->len);
        else fw_fail(rc, "zone", out, cap);
        break;
    default:
        snprintf(out, cap, "%s", tok_or(rc, "Failed"));
        break;
    }
    if (!out[0]) snprintf(out, cap, "Failed");
    for (char *p = out; *p; p++)                      /* the ASCII rule holds even for foreign version strings */
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '?';
    return (int)strlen(out);
}
