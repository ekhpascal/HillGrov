#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include "board.h"

#define RING_RD_SZ 128

static void ring_fwd_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    uint8_t buf[RING_RD_SZ];
    for (;;) {
        esp_task_wdt_reset();
        int n = uart_read_bytes(UART_NUM_2, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (n > 0) uart_write_bytes(UART_NUM_2, (const char *)buf, n);
    }
}

/* Rescue has no ring protocol of its own (that's Master/zone in SP3); it just
 * repeats whatever bytes arrive on the ring UART back out the same port, so
 * a chain of zones stays passable while one of them is stuck in rescue. */
void ring_fwd_start(void) {
    uart_driver_install(UART_NUM_2, 1024, 1024, 0, NULL, 0);
    uart_config_t cfg = {
        .baud_rate = HG_RING_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_2, &cfg);
    uart_set_pin(UART_NUM_2, HG_GPIO_RING_TX, HG_GPIO_RING_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    xTaskCreatePinnedToCore(ring_fwd_task, "ring_fwd", 2048, NULL, 5, NULL, 0);
}
