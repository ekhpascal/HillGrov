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
/* This boot's reset reason as the CLI/NOTIFY reason token (the esp_reset_reason
 * name without its ESP_RST_ prefix: POWERON, SW, PANIC, TASK_WDT, BROWNOUT,
 * ...). One list for both apps -- NOTIFY BOOT used to print a hardcoded
 * "POWERON" regardless of why the board actually restarted. */
const char *hg_app_reset_reason(void);
int      hg_app_log_set(const char *level, const char *tag, char *eff, size_t n);
int      hg_app_time_get(char *buf, size_t n);
/* Same line, but with an EXTERNAL clock source offered alongside the local
 * SET TIME state: src is its app_if.h token ("RING" on a zone) and src_at the
 * UPTIME second at which it last set the clock (0 = never -- a source landing
 * inside the first second of uptime is simply reported one update late). The
 * more recent of the two sources names the reported token and its age; NONE
 * when neither has run. Kept here rather than in the caller so one function
 * owns the "YYYY-MM-DD HH:MM:SS <SRC> <age_s>" format. */
int      hg_app_time_get_ext(char *buf, size_t n, const char *src, uint32_t src_at);
int      hg_app_time_set(int y, int mo, int d, int h, int mi, int s);
int      hg_app_fw_info(char *buf, size_t n);
int      hg_app_fw_rollback(void);

#ifdef __cplusplus
}
#endif
