#pragma once
#define HG_GPIO_RING_RX    18
#define HG_GPIO_RING_TX    19
#define HG_GPIO_I2C_SDA    21
#define HG_GPIO_I2C_SCL    22
#define HG_GPIO_RESCUE_BTN 15
#define HG_GPIO_STATUS_LED 2
#define HG_GPIO_PCA_OE     23
#define HG_CONSOLE_BAUD    115200
#define HG_RING_BAUD       115200
#if CONFIG_HG_ROLE_MASTER
#define HG_ROLE_NAME "MASTER"
#else
#define HG_ROLE_NAME "ZONE"
#endif
