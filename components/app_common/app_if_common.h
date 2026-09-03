#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Shared app_if_t member bodies -- byte-identical across zone and master in
 * SP1, consolidated here (SP1 review parking item) so the two apps link one
 * copy of the source instead of carrying two copies that must be kept in
 * sync by hand. Each app still owns its own app_if_t table and assigns these
 * function pointers directly; role-specific members (zone_id, node_name,
 * status_lines, save_flush, fw_update, reboot, factory_reset) stay put. */

void     hg_app_get_mac(uint8_t mac[6]);
uint32_t hg_app_uptime_s(void);
int      hg_app_log_set(const char *level, const char *tag, char *eff, size_t n);
int      hg_app_time_get(char *buf, size_t n);
int      hg_app_time_set(int y, int mo, int d, int h, int mi, int s);
int      hg_app_fw_info(char *buf, size_t n);
int      hg_app_fw_rollback(void);

#ifdef __cplusplus
}
#endif
