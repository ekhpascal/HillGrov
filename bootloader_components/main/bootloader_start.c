/* Custom 2nd-stage bootloader: rescue / NVS-erase boot selection (spec S1.4).
 *
 * Selection order, checked once per cold/warm reset (skipped after a deep-sleep
 * wake, matching the stock bootloader's own fast path):
 *   1. RTC retain-mem software rescue flag, written by hg_reboot_to_rescue()
 *      (zone/main/app_if_zone.c) -> boot factory (rescue) once, then clear it.
 *   2. HG_RESCUE_GPIO held low at reset (idle high via internal pull-up):
 *        >= 10 s  -> boot factory (rescue)
 *        1 s..9 s -> erase the "nvs" partition, then continue normal boot
 *        < 1 s    -> ignored
 *
 * Modelled on IDF's examples/custom_bootloader/bootloader_override, adapted for
 * IDF 6.0.1 (esp32 target): ROM gpio_pad_* helpers are namespaced rom_gpio_pad_*
 * in this IDF, PIN_INPUT_ENABLE needs an explicit soc/io_mux_reg.h include, and
 * __getreent() must stay newlib-only -- this project builds with CONFIG_LIBC_PICOLIBC,
 * whose own __getreent() (esp_libc/src/picolibc/getreent.c) is linked into the
 * bootloader subproject instead.
 *
 * Builds for both the ESP32 (zones, rescue, Master v1) and the ESP32-P4 (Master
 * v2). Three things are target-specific and are selected below, never removed:
 * the ROM gpio.h path, the rescue pin number, and how the pad level is read.
 * Everything else -- the RTC retain-mem flag, the timings and the boot-image
 * selection -- is target-neutral IDF API, so the rescue contract is identical
 * on both.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp32p4/rom/gpio.h"
#else
#include "esp32/rom/gpio.h"
#endif
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"

static const char *TAG = "hg_boot";
#define HG_RESCUE_FLAG     0xB0FAAF0Bu
/* MUST agree with HG_GPIO_RESCUE_BTN in components/board/board.h. It is
 * duplicated rather than included because the bootloader subproject does not
 * link the board component. On the P4, GPIO15 (the ESP32 value) is the C6 SDIO
 * D1 line -- using it here would drive a Wi-Fi bus line from a button and read
 * the C6 as a button press. */
#if CONFIG_IDF_TARGET_ESP32P4
#define HG_RESCUE_GPIO     34
#else
#define HG_RESCUE_GPIO     15
#endif
#define HG_HOLD_FACTORY_MS 10000u
#define HG_HOLD_ERASE_MS   1000u

/* Reading the pad level is not portable either: GPIO_INPUT_GET() is defined
 * only in the esp32 and esp32s2 ROM headers. The P4 ROM exports its own
 * rom_gpio_get_input_level() (esp32p4.rom.ld), which takes the pin number and
 * handles the >= 32 register split internally -- which GPIO34 needs. The ESP32
 * arm expands to exactly the previous expression, so its object code is
 * unchanged. */
#if CONFIG_IDF_TARGET_ESP32P4
#define HG_RESCUE_PRESSED() (rom_gpio_get_input_level(HG_RESCUE_GPIO) == 0)
#else
#define HG_RESCUE_PRESSED() (GPIO_INPUT_GET(HG_RESCUE_GPIO) == 0)
#endif

/* This fully replaces the stock call_start_cpu0(), so the stock bootloader's
 * weak bootloader_before_init()/bootloader_after_init() hooks (bootloader_hooks.h)
 * are intentionally not called here -- that hook mechanism only fires from
 * the stock implementation we are overriding. */
void __attribute__((noreturn)) call_start_cpu0(void) {
    if (bootloader_init() != ESP_OK) bootloader_reset();
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "partition table load failed");
        bootloader_reset();
    }
    int boot_index = bootloader_utility_get_selected_boot_partition(&bs);
    if (boot_index == INVALID_INDEX) bootloader_reset();

    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        /* 1. software rescue request via RTC retain memory. custom[] is excluded
         * from the retain-mem CRC in this build (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
         * without ..._IN_CRC), so the magic word itself is the sole authority -- never
         * gate this on retain-mem CRC validity. On a genuine first power-on the CRC
         * check over the pre-custom fields fails and bootloader_common_update_rtc_retain_mem()
         * resets the WHOLE struct -- including custom[] -- to zero before we read it
         * (see bootloader_common_reset_rtc_retain_mem() in bootloader_common_loader.c),
         * so power-on garbage cannot fake the flag; no extra first-boot guard is needed. */
        bootloader_common_update_rtc_retain_mem(NULL, true);
        rtc_retain_mem_t *rtc = bootloader_common_get_rtc_retain_mem();
        uint32_t flag;
        memcpy(&flag, rtc->custom, sizeof flag);
        bool to_rescue = (flag == HG_RESCUE_FLAG);
        flag = 0;
        memcpy(rtc->custom, &flag, sizeof flag);
        bootloader_common_update_rtc_retain_mem(NULL, false);
        if (to_rescue) {
            ESP_LOGI(TAG, "rescue flag set -> factory");
            bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
        }

        /* 2. rescue button: HG_RESCUE_GPIO, idle high (pull-up), pressed = low */
        rom_gpio_pad_select_gpio(HG_RESCUE_GPIO);
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[HG_RESCUE_GPIO]);
        rom_gpio_pad_pullup(HG_RESCUE_GPIO);
        if (HG_RESCUE_PRESSED()) {
            uint32_t t0 = esp_log_early_timestamp(), held;
            do {
                held = esp_log_early_timestamp() - t0;
                if (held >= HG_HOLD_FACTORY_MS) {
                    ESP_LOGI(TAG, "button held %u ms -> factory", (unsigned)held);
                    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
                }
            } while (HG_RESCUE_PRESSED());
            if (held >= HG_HOLD_ERASE_MS) {
                ESP_LOGW(TAG, "button held %u ms -> erase nvs", (unsigned)held);
                if (!bootloader_common_erase_part_type_data("nvs", false))
                    ESP_LOGE(TAG, "nvs erase failed");
            }
        }
    }
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
/* Only needed when built against newlib; this project uses CONFIG_LIBC_PICOLIBC,
 * which supplies its own __getreent() (linked into the bootloader subproject via
 * the esp_libc component) -- defining one here too would collide with it. */
struct _reent *__getreent(void) { return _GLOBAL_REENT; }
#endif
