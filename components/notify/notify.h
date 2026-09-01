#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NTF_BOOT, NTF_ALARM, NTF_SAFE, NTF_NODE, NTF_RING,
               NTF_WATER, NTF_LIGHT, NTF_SOIL, NTF_FW, NTF_CMD, NTF_COUNT } ntf_type_t;
#define NTF_LINE_MAX  128
#define NTF_MAX_SINKS 3
#define NTF_MASK(t)   (uint16_t)(1u << (t))
#define NTF_MASK_ALL  ((uint16_t)((1u << NTF_COUNT) - 1))
/* CLI default per spec §5.5: everything ON except LIGHT and SOIL */
#define NTF_DEFAULT_CLI_MASK (uint16_t)(NTF_MASK_ALL & ~(NTF_MASK(NTF_LIGHT) | NTF_MASK(NTF_SOIL)))
typedef void (*ntf_sink_fn)(void *ctx, const char *line);   /* line ends with '\n' */
void        notify_init(uint32_t (*now_ms)(void), uint8_t node_id);
void        notify_set_node_id(uint8_t id);
int         notify_add_sink(ntf_sink_fn fn, void *ctx, uint16_t mask);  /* index or -1 */
void        notify_set_sink_mask(int sink, uint16_t mask);
uint16_t    notify_sink_mask(int sink);
int         notify_parse(const char *name);   /* type index; NTF_COUNT for "ALL"; -1 unknown */
const char *notify_type_name(int t);
void        notify_emit(ntf_type_t t, uint8_t idx, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
