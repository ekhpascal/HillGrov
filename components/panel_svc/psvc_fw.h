#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware-install state shared by every face. This task: upload progress,
 * moved out of http_upload.c so /api/state and the panel read ONE value and a
 * phone can watch a panel install (and the reverse). Task 30 adds the install
 * core to this header.
 *
 * Lock-free by construction: two word-sized stores by the one installer, two
 * loads by any reader. kind must be a string literal ("" | "master" | "zone"),
 * so the pointer stays valid however the install ends; pct is stored FIRST, so
 * a reader never sees a kind without its pct. */
void psvc_fw_progress_set(const char *kind, uint32_t pct);   /* NULL kind == "" */

/* [ANY] 1 while an install streams, else 0. *kind ("" when idle) and *pct
 * (0..100, clamped) are always written; either pointer may be NULL. */
int  psvc_fw_progress(const char **kind, uint8_t *pct);

#ifdef __cplusplus
}
#endif
