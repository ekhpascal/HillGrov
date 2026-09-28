#include "wifi_mgr.h"    /* the real prototypes, so a signature drift fails the host build */
#include "time_svc.h"
#include "fake_apply.h"

int fake_apply_wifi_n;
int fake_apply_time_n;
int fake_apply_wifi_rc;

void fake_apply_reset(void) {
    fake_apply_wifi_n = 0;
    fake_apply_time_n = 0;
    fake_apply_wifi_rc = 0;
}

int wifi_mgr_apply(void) { fake_apply_wifi_n++; return fake_apply_wifi_rc; }
void time_svc_apply_mcfg(void) { fake_apply_time_n++; }
