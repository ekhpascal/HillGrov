#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "board.h"
#include "ring_link.h"

static const char *TAG = "ring_link";

#define RING_RX_TASK_STACK  4096
#define RING_RX_TASK_PRIO   6
#define RING_EVTQ_DEPTH     16
#define RING_RX_BUF         2048
#define RING_TX_BUF         1024
/* fix round minor #6: was 4. A CFG_GET pull's chunk burst (up to
 * RING_CFG_MAX_CHUNKS = 7 CFG_CHUNK frames, back-to-back and unACKed) can
 * land in this queue at the same moment a HEARTBEAT or ACK arrives from a
 * different zone; depth 4 risked rx_drop on that collision alone, before
 * node_mgr's consumer even got a chance to drain. 8 gives headroom for a
 * full chunk burst plus one more frame. */
#define RING_CONSUME_DEPTH  8
#define RING_READ_CHUNK     64

static int               s_is_master;
static uint8_t          (*s_my_id_fn)(void);
static uint8_t           s_my_mac[6];
static QueueHandle_t     s_uart_evtq;
static QueueHandle_t     s_consume_q;      /* items: ring_frame_t, depth RING_CONSUME_DEPTH (8) */
static ring_counters_t   s_ctr;
static volatile uint32_t s_ts_returned_ms;
static volatile int      s_trace;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ring_route only ttl-guards its unicast forward path (ttl<=1 -> DROP); the
 * broadcast (dst 0xFF -> CONSUME_FWD) and 0xFE-forward routes carry NO ttl
 * guarantee at all. So a ttl==0 frame can still reach here on those paths --
 * e.g. a mis-wired sub-loop that excludes the master lets a broadcast's ttl
 * run to 0. Without this guard the uint8_t decrement below would wrap 0 to
 * 255 and the frame would circulate forever, defeating spec §2.3's "drop at
 * 0" backstop -- so a ttl==0 arrival is dropped here, not forwarded. Once
 * past the guard, ring_route's own unicast check (or this one) means ttl-1
 * never underflows. Re-encodes fresh (own CRC per spec §2.2) rather than
 * patching the original wire bytes. */
static void ring_link_forward(const ring_hdr_t *h, const uint8_t *payload) {
    if (h->ttl == 0) { s_ctr.rx_drop++; return; }
    ring_hdr_t fh = *h;
    fh.ttl = (uint8_t)(fh.ttl - 1);
    uint8_t wire[RING_WIRE_MAX];
    int n = ring_frame_encode(&fh, payload, wire, sizeof wire);
    if (n < 0) return;   /* can't happen for an already-decoded frame; never write garbage */
    uart_write_bytes(UART_NUM_2, (const char *)wire, (size_t)n);
    s_ctr.fwd_count++;
}

static void ring_link_enqueue(const ring_hdr_t *h, const uint8_t *payload) {
    ring_frame_t f;
    f.hdr = *h;
    /* bound the copy to the validated payload length -- the rest of f.payload
     * would otherwise carry indeterminate stack bytes across the task boundary. */
    memcpy(f.payload, payload, h->len);
    if (h->len < RING_MAX_PAYLOAD) memset(f.payload + h->len, 0, RING_MAX_PAYLOAD - h->len);
    if (xQueueSend(s_consume_q, &f, 0) != pdTRUE) s_ctr.rx_drop++;
}

static void ring_link_dispatch(const ring_hdr_t *h, const uint8_t *payload) {
    uint8_t my_id = s_my_id_fn();   /* read fresh: a zone's id can change on ASSIGN_ID without restart */
    ring_rt_t rt = ring_route(s_is_master, my_id, s_my_mac, h, payload);
    if (s_trace)
        ESP_LOGI(TAG, "hdr src=%02x dst=%02x type=%02x flags=%02x ttl=%02x len=%02x seq=%04x rt=%d",
                 h->src, h->dst, h->type, h->flags, h->ttl, h->len, h->seq, (int)rt);
    switch (rt) {
    case RING_RT_DROP_SELF:
        s_ctr.drop_self++;
        /* master's own TIME_SYNC broadcast returning is the ring-closed probe
         * (spec §2.5/§2.7) -- stamp before dropping. A zone's DROP_SELF just counts. */
        if (s_is_master && h->type == RING_T_TIME_SYNC) s_ts_returned_ms = now_ms();
        /* Task 13 ruling #9 (authorized ring_link extension): a master-sent
         * TRACKED unicast (CMD/FW_UPDATE/CFG_COMMIT/CFG_GET) coming back as
         * DROP_SELF means the addressed id doesn't exist on the ring -- no
         * zone claimed it. node_mgr's tracker needs to see this to fail the
         * pending entry fast (ring_trk_unclaimed) instead of waiting out a
         * full ack-timeout ladder, so it's ALSO copied into the consume
         * queue here; the ttl-0 guard and the TIME_SYNC probe stamp above
         * are unchanged. */
        if (s_is_master &&
            (h->type == RING_T_CMD || h->type == RING_T_FW_UPDATE ||
             h->type == RING_T_CFG_COMMIT || h->type == RING_T_CFG_GET))
            ring_link_enqueue(h, payload);
        break;
    case RING_RT_CONSUME:
        ring_link_enqueue(h, payload);
        break;
    case RING_RT_FORWARD:
        ring_link_forward(h, payload);
        break;
    case RING_RT_CONSUME_FWD:
        ring_link_forward(h, payload);
        ring_link_enqueue(h, payload);
        break;
    case RING_RT_DROP:
    default:
        break;
    }
}

static void ring_rx_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    ring_dec_t dec;
    ring_dec_init(&dec);
    uart_event_t ev;
    uint8_t buf[RING_READ_CHUNK];
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_uart_evtq, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        switch (ev.type) {
        case UART_DATA: {
            int n;
            while ((n = uart_read_bytes(UART_NUM_2, buf, sizeof buf, 0)) > 0) {
                for (int i = 0; i < n; i++) {
                    ring_hdr_t hdr;
                    uint8_t payload[RING_MAX_PAYLOAD];
                    int rc = ring_dec_feed(&dec, buf[i], &hdr, payload);
                    if (rc == 1) ring_link_dispatch(&hdr, payload);
                    else if (rc < 0) s_ctr.rx_crc_err++;   /* decoder already resynced */
                }
            }
            break;
        }
        case UART_BREAK:
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            ring_dec_reset(&dec);   /* line-level fault: whatever is half-decoded is suspect */
            s_ctr.rx_uart_err++;
            break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            /* IDF's documented recovery for both: bytes have already been lost
             * and the driver's event queue can be backed up with events for
             * data that no longer exists, so flush the input and reset the
             * queue to restart from a known-empty state. Only FIFO_OVF resets
             * the decoder: a hardware FIFO overrun means the byte stream itself
             * has a hole, whereas BUFFER_FULL is back-pressure on the driver's
             * ring buffer -- the bytes already fed to the decoder are intact,
             * so the frame in flight is kept (and COBS's 0x00 delimiters resync
             * the stream by themselves if anything really was dropped). */
            if (ev.type == UART_FIFO_OVF) ring_dec_reset(&dec);
            uart_flush_input(UART_NUM_2);
            xQueueReset(s_uart_evtq);
            s_ctr.rx_uart_err++;
            break;
        default:
            break;
        }
    }
}

void ring_link_start(int is_master, uint8_t (*my_id_fn)(void), const uint8_t my_mac[6]) {
    s_is_master = is_master;
    s_my_id_fn  = my_id_fn;
    memcpy(s_my_mac, my_mac, 6);
    s_consume_q = xQueueCreate(RING_CONSUME_DEPTH, sizeof(ring_frame_t));

    uart_driver_install(UART_NUM_2, RING_RX_BUF, RING_TX_BUF, RING_EVTQ_DEPTH, &s_uart_evtq, 0);
    uart_config_t cfg = {
        .baud_rate  = HG_RING_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_NUM_2, &cfg);
    uart_set_pin(UART_NUM_2, HG_GPIO_RING_TX, HG_GPIO_RING_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /* uart_set_pin doesn't enable the RX pull on ESP32 classic (spec §2.1); without it an
     * unpowered upstream node leaves the pin floating and floods us with BREAK events. */
    gpio_set_pull_mode(HG_GPIO_RING_RX, GPIO_PULLUP_ONLY);

    xTaskCreatePinnedToCore(ring_rx_task, "ring_rx", RING_RX_TASK_STACK, NULL, RING_RX_TASK_PRIO, NULL, 0);
}

int ring_link_recv(ring_frame_t *f, uint32_t wait_ms) {
    return xQueueReceive(s_consume_q, f, pdMS_TO_TICKS(wait_ms)) == pdTRUE ? 0 : -1;
}

int ring_link_send(const ring_hdr_t *h, const uint8_t *payload) {
    uint8_t wire[RING_WIRE_MAX];
    int n = ring_frame_encode(h, payload, wire, sizeof wire);
    if (n < 0) return -1;
    uart_write_bytes(UART_NUM_2, (const char *)wire, (size_t)n);
    s_ctr.tx_count++;
    return 0;
}

int ring_link_send_raw(const uint8_t *wire, size_t len) {
    if (!wire || len == 0 || len > RING_WIRE_MAX) return -1;
    uart_write_bytes(UART_NUM_2, (const char *)wire, len);
    s_ctr.tx_count++;
    return 0;
}

void ring_link_counters(ring_counters_t *out) { *out = s_ctr; }

uint32_t ring_link_ts_returned_ms(void) { return s_ts_returned_ms; }

void ring_link_trace(int on) { s_trace = on; }
