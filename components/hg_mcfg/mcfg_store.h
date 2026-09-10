#pragma once
#include <stdint.h>
#include "hg_mcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Master-only NVS glue for hg_mcfg_t: namespace "hg", key "mcfg" (own
 * nvs_open/NVS_READWRITE handle, opened and closed per call -- same pattern
 * as node_store_nvs.c). Not compiled into the zone build (see
 * components/hg_mcfg/CMakeLists.txt): zone links nvs_flash for other
 * reasons but never touches the "mcfg" key.
 *
 * RAM contract: the active config lives in one of two internal buffers.
 * mcfg_get() returns a pointer straight into the currently-active buffer --
 * no copy, no lock -- so any task may dereference it at any time. A commit
 * never mutates the buffer a caller might currently be reading: mcfg_commit()
 * writes NVS first, then fills the *inactive* buffer and flips the active
 * index under a FreeRTOS mutex, so a concurrent mcfg_get() reader always
 * sees either the whole old struct or the whole new one, never a torn mix.
 * Don't hold a pointer from mcfg_get() across a call to mcfg_commit() --
 * call mcfg_get() again afterwards if you need the fresh copy. */

int              mcfg_store_init(void);            /* nvs_open("hg") reuse; loads key "mcfg" -> RAM copy; absent/corrupt -> defaults (logged); 0 ok, -1 defaults in use */
const hg_mcfg_t *mcfg_get(void);                    /* pointer into the active RAM buffer; safe to read from any task */
int              mcfg_commit(const hg_mcfg_t *m);   /* validate -> nvs_set_blob("mcfg") + commit -> RAM (double-buffer flip), gen+1; 0 ok / -1 invalid / -2 nvs; ~50 ms, call rarely (credentials/password/tz) */
uint32_t         mcfg_gen(void);                    /* envelope generation: 0 on defaults, +1 per successful commit */

#ifdef __cplusplus
}
#endif
