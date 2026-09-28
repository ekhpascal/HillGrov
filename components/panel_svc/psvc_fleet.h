#pragma once
/* Fleet start/abort for both faces: node_mgr's sequencer (SP3) behind the shared refusal vocabulary. The sequencer pulls
 * the zone_fw image over GET /fw/zone.bin; nothing here touches flash. */
#include <stdint.h>
#include "psvc_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

psvc_rc_t psvc_fleet_zone(uint8_t zone);   /* [WORKER] zone outside 1..HG_MAX_ZONES -> PSVC_E_INVALID */
psvc_rc_t psvc_fleet_all(void);            /* [WORKER] */
psvc_rc_t psvc_fleet_abort(void);          /* [WORKER] */
int       psvc_fleet_idle(const char *fleet_line);   /* pure: 1 when "IDLE" -- the web's enable rule (app.js:995-1008) */

#ifdef __cplusplus
}
#endif
