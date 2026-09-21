#pragma once
#include "sdkconfig.h"   /* an undefined CONFIG_HG_ROLE_MASTER silently evaluates to 0 in #if below */

/* Pin map per target. The ESP32 block is the SP1-SP4 bench reality for the
 * DevKitC master, the zones and rescue, and must stay byte-for-byte as it was.
 *
 * The ESP32-P4 block exists because Master v2 CANNOT reuse the ESP32 numbers:
 * the ring's 18/19 are the ESP32-C6 SDIO CLK and CMD on the Waveshare board, so
 * porting them unchanged would aim the ring UART straight at the Wi-Fi
 * co-processor bus -- both would misbehave intermittently with nothing
 * obviously wrong in either. Values come from docs/pin-mapping.md's Master v2
 * table, which was read off the board schematic and confirmed on hardware.
 */
#if CONFIG_IDF_TARGET_ESP32P4

#define HG_GPIO_RING_RX    29   /* header P3; ring UART2 -- NOT 18/19 (C6 SDIO) */
#define HG_GPIO_RING_TX    28
#define HG_GPIO_I2C_SDA    8    /* the board's own I2C header, level-shifted */
#define HG_GPIO_I2C_SCL    7
/* Placeholders until the migration settles them against the enclosure: the P4
 * board has its own BOOT/RESET buttons and a STAT LED whose nets are not in the
 * schematic text, and the PCA9685 OE line is an external-wiring choice. All
 * three come from the documented carefree local-I/O pool (IO28-31, IO2-5,
 * IO34/36) minus the two the ring now takes. Nothing in the ring bring-up
 * touches them. */
#define HG_GPIO_RESCUE_BTN 34
#define HG_GPIO_STATUS_LED 30
#define HG_GPIO_PCA_OE     31

#else  /* ESP32 classic: DevKitC master, zones, rescue */

#define HG_GPIO_RING_RX    18
#define HG_GPIO_RING_TX    19
#define HG_GPIO_I2C_SDA    21
#define HG_GPIO_I2C_SCL    22
#define HG_GPIO_RESCUE_BTN 15
#define HG_GPIO_STATUS_LED 2
#define HG_GPIO_PCA_OE     23

#endif

#define HG_CONSOLE_BAUD    115200
#define HG_RING_BAUD       115200
#if CONFIG_HG_ROLE_MASTER
#define HG_ROLE_NAME "MASTER"
#else
#define HG_ROLE_NAME "ZONE"
#endif
