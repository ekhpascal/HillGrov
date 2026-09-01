#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
    const char *role_name;                          /* "MASTER" / "ZONE" */
    uint8_t   (*zone_id)(void);
    void      (*get_mac)(uint8_t mac[6]);
    const char *(*node_name)(void);                 /* "-" when unnamed */
    uint32_t  (*uptime_s)(void);
    int       (*status_lines)(char *resp, int len); /* append "  Label : value" lines; 0 ok */
    int       (*log_set)(const char *level, const char *tag, char *eff, size_t n);
    int       (*time_get)(char *buf, size_t n);     /* "YYYY-MM-DD HH:MM:SS <RTC|NTP|RING|NONE> <age_s>" */
    int       (*time_set)(int y, int mo, int d, int h, int mi, int s);
    int       (*save_flush)(uint32_t timeout_ms);   /* 0 ok / -1 */
    int       (*fw_info)(char *buf, size_t n);      /* "<ver> <ota_0|ota_1> <VALID|PENDING> <other|NONE>" */
    int       (*fw_rollback)(void);                 /* reboots on success; returns -1 on failure */
    int       (*fw_update)(const char *ssid, const char *pass, const char *url); /* writes handover + RTC flag + reboots */
    void      (*reboot)(void);
    int       (*factory_reset)(void);               /* erases NVS then reboots; returns -1 on failure */
} app_if_t;
