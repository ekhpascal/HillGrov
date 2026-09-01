#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CMD_LINE_MAX   192
#define CMD_MAX_TOKENS 12
#define CMD_MAX_ARGS   8
#define CMD_RESP_MAX   4096
#define CMD_UNLOCK_MS  600000u

typedef enum { CMD_SRC_CLI = 0, CMD_SRC_HTTP, CMD_SRC_RING, CMD_SRC_INTERNAL } cmd_src_t;
typedef enum { ARG_INT = 0, ARG_ENUM, ARG_STR, ARG_TIME, ARG_MAC } cmd_arg_type_t;
typedef struct { const char *name; uint8_t type; int32_t min, max; const char *enums; } cmd_arg_t;

#define CMDV_SET  0x01
#define CMDV_GET  0x02
#define CMDV_BARE 0x04

#define CMDF_ACTUATOR 0x0001  /* audited; must carry a bounded duration arg */
#define CMDF_UNLOCK   0x0002  /* needs DEBUG ENABLE on this session */
#define CMDF_MASTER   0x0004
#define CMDF_ZONE     0x0008
#define CMDF_SESSION  0x0010  /* acts on the calling session; ERR NOT_LOCAL from HTTP */
#define CMDF_SLOW     0x0020  /* handler budget 1000 ms instead of 50 ms */

enum { CMD_AREA_SYSTEM = 0, CMD_AREA_CONFIG, CMD_AREA_SESSION, CMD_AREA_TIME, CMD_AREA_FW,
       CMD_AREA_RING, CMD_AREA_SHELF, CMD_AREA_SENSOR, CMD_AREA_FAULT, CMD_AREA_NET,
       CMD_AREA_DEBUG, CMD_AREA_COUNT };

#define CMD_ROLE_MASTER 0
#define CMD_ROLE_ZONE   1

typedef struct { cmd_src_t source; uint8_t echo; uint16_t notify_mask; uint32_t unlock_until_ms; } cmd_session_t;

typedef struct cmd_core cmd_core_t;
typedef struct cmd_entry cmd_entry_t;

typedef struct {
    const cmd_core_t  *core;
    cmd_session_t     *ses;
    const cmd_entry_t *e;
    uint8_t  verb;                       /* the CMDV_* bit actually used */
    uint8_t  n;                          /* args given */
    const char *tok[CMD_MAX_ARGS];       /* raw arg tokens, case preserved */
    int32_t  val[CMD_MAX_ARGS];          /* INT value / ENUM index / TIME minutes; 0 for STR/MAC */
    uint8_t  mac[6];                     /* last ARG_MAC parsed */
} cmd_req_t;

typedef int (*cmd_handler_fn)(cmd_req_t *q, char *resp, int len);   /* 0 OK / -1 ERR; must write one reply */

struct cmd_entry {
    uint8_t         verbs, area;
    const char     *noun1, *noun2;       /* noun2 NULL for one-word nouns */
    const cmd_arg_t *args;               /* max_args entries */
    uint8_t         n_key;               /* leading args GET also takes */
    uint8_t         min_args, max_args;  /* for SET / BARE */
    uint16_t        flags;
    cmd_handler_fn  handler;
    const char     *desc;                /* <= 40 chars or NULL */
};

/* 0 = forwarded OK (resp holds the zone's reply line); -1 = resp holds an ERR line */
typedef int (*cmd_forward_fn)(void *ctx, uint8_t zone, const char *line, char *resp, int len);

struct cmd_core {
    const cmd_entry_t *table;
    int                table_len;
    uint8_t            role;             /* CMD_ROLE_* */
    uint8_t            zone_id;          /* own ring id; 0 on master/unassigned */
    uint32_t         (*now_ms)(void);
    cmd_forward_fn     forward;          /* master only; NULL elsewhere */
    void              *forward_ctx;
    void             (*audit)(void *ctx, cmd_src_t src, const char *line);
    void              *audit_ctx;
    const char        *debug_key;        /* for cmd_common's DEBUG ENABLE row */
};

int cmd_dispatch(const cmd_core_t *core, cmd_session_t *ses, const char *line, char *resp, int resp_len);
int cmd_help(const cmd_core_t *core, cmd_session_t *ses, const char *const *tok, int ntok, char *resp, int len);
int cmd_table_check(const cmd_entry_t *t, int n);   /* -1 ok, else index of first bad row */

int cmd_err(char *resp, int len, const char *token);            /* writes "ERR <token>\n", returns -1 */
int cmd_okf(char *resp, int len, const char *fmt, ...);         /* writes "OK " + fmt + "\n", returns 0 */
int cmd_linef(char *resp, int len, const char *fmt, ...);       /* appends a line (help/continuations), 0/-1 full */
int cmd_ci_eq(const char *a, const char *b);
int cmd_tokenize(char *line, const char *tok[], int max);       /* in place; count or -1 if > max */
int cmd_parse_time(const char *s);                              /* "H:MM"/"HH:MM"/minutes -> 0..1439, -1 bad */

#ifdef __cplusplus
}
#endif
