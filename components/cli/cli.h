#pragma once
#include <stdint.h>
/* UART0 CLI: echo human console + notify sink + esp_log redirection. */
void cli_init(void);          /* installs the UART0 driver, mutex, esp_log vprintf hook, notify sink */
void cli_start(void);         /* prints the boot banner, spawns the cli0 read/dispatch task */
uint32_t cli_log_drops(void); /* count of log lines dropped because the console mutex was busy */
