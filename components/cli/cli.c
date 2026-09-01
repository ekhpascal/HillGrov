#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_app_desc.h"    /* declared by the esp_app_format component */
#include "cli_line.h"
#include "cmd_task.h"
#include "notify.h"
#include "board.h"
#include "cli.h"

/* IDF 6.0.1 offers LOG_VERSION_1 / LOG_VERSION_2; this CLI's cli_vprintf hook and
 * raw \n->\r\n rewriting assume the v1 plain-text log line format (spec 8 pin). */
#ifdef CONFIG_LOG_VERSION
_Static_assert(CONFIG_LOG_VERSION == 1, "pin log v1");
#endif

static const char *TAG = "cli";
#define MUTEX_MS     50
#define VPRINTF_BUF  256
#define UART_READ_SZ 64

static SemaphoreHandle_t s_mux;
static cli_line_t        s_line;
static char              s_resp[CMD_RESP_MAX];
static cmd_session_t     s_cli_ses = { .source = CMD_SRC_CLI, .echo = 1, .notify_mask = NTF_DEFAULT_CLI_MASK };
static uint32_t          s_log_drops;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* Writes bytes to UART0, expanding every '\n' to "\r\n"; caller must hold s_mux. */
static void write_raw(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && s[i] != '\n') i++;
        if (i > start) uart_write_bytes(UART_NUM_0, s + start, i - start);
        if (i < n && s[i] == '\n') { uart_write_bytes(UART_NUM_0, "\r\n", 2); i++; }
    }
}

/* Echo bytes from cli_line_feed are already fully formed (e.g. "\r\n", "\b \b"),
 * so they go straight to the wire without the \n->\r\n rewrite above. */
static void write_verbatim(const char *s, size_t n) {
    if (!n) return;
    if (xSemaphoreTakeRecursive(s_mux, pdMS_TO_TICKS(MUTEX_MS)) != pdTRUE) return;
    uart_write_bytes(UART_NUM_0, s, n);
    xSemaphoreGiveRecursive(s_mux);
}

static void cli_write(const char *s) {
    size_t n = strlen(s);
    if (xSemaphoreTakeRecursive(s_mux, pdMS_TO_TICKS(MUTEX_MS)) != pdTRUE) return;
    write_raw(s, n);
    xSemaphoreGiveRecursive(s_mux);
}

static void cli_notify_sink(void *ctx, const char *line) {
    (void)ctx;
    cli_write(line);
}

uint32_t cli_log_drops(void) { return s_log_drops; }

static int cli_vprintf(const char *fmt, va_list ap) {
    char buf[VPRINTF_BUF];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return 0;
    if ((size_t)n >= sizeof buf) {
        buf[sizeof buf - 2] = '~';
        buf[sizeof buf - 1] = '\0';
        n = (int)sizeof buf - 1;
    }
    if (xSemaphoreTakeRecursive(s_mux, pdMS_TO_TICKS(MUTEX_MS)) != pdTRUE) {
        s_log_drops++;
        return n;
    }
    const char *edit = cli_line_peek(&s_line);
    int redraw = s_cli_ses.echo && edit[0] != '\0';
    if (redraw) write_raw("\r\x1b[2K", 4);
    write_raw(buf, (size_t)n);
    if (redraw) write_raw(edit, strlen(edit));
    xSemaphoreGiveRecursive(s_mux);
    return n;
}

void cli_init(void) {
    uart_driver_install(UART_NUM_0, 512, 2048, 0, NULL, 0);
    uart_config_t cfg = {
        .baud_rate = HG_CONSOLE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_0, &cfg);
    uart_vfs_dev_use_driver(0);
    s_mux = xSemaphoreCreateRecursiveMutex();
    cli_line_init(&s_line, 1);
    esp_log_set_vprintf(cli_vprintf);
    notify_add_sink(cli_notify_sink, NULL, NTF_DEFAULT_CLI_MASK);
}

static void cli_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    uint8_t buf[UART_READ_SZ];
    char    echo[UART_READ_SZ];
    for (;;) {
        esp_task_wdt_reset();
        cli_line_set_echo(&s_line, s_cli_ses.echo);
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            size_t elen = 0;
            cli_evt_t ev = cli_line_feed(&s_line, buf[i], now_ms(), echo, sizeof echo, &elen);
            if (elen) write_verbatim(echo, elen);
            if (ev == CLI_EVT_LINE) {
                const char *line = cli_line_take(&s_line);
                cmd_task_execute(&s_cli_ses, line, s_resp, sizeof s_resp, 3500);
                cli_write(s_resp);
            } else if (ev == CLI_EVT_TOO_LONG) {
                cli_write("ERR TOO_LONG\r\n");
            }
        }
    }
}

void cli_start(void) {
    char banner[80];
    ESP_LOGI(TAG, "starting UART0 console at %u baud", (unsigned)HG_CONSOLE_BAUD);
    snprintf(banner, sizeof banner, "HillGrow %s %s -- HELP for commands\r\n",
             HG_ROLE_NAME, esp_app_get_description()->version);
    cli_write(banner);
    xTaskCreatePinnedToCore(cli_task, "cli0", 4096, NULL, 4, NULL, 0);
}
