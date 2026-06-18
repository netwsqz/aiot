#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ── SPI (e-paper) ─────────────────────────────────── */
#define EPD_SPI_NUM         SPI2_HOST
#define EPD_DC_PIN          GPIO_NUM_10
#define EPD_CS_PIN          GPIO_NUM_11
#define EPD_SCK_PIN         GPIO_NUM_12
#define EPD_MOSI_PIN        GPIO_NUM_13
#define EPD_RST_PIN         GPIO_NUM_9
#define EPD_BUSY_PIN        GPIO_NUM_8

/* ── Power (active levels in comments) ─────────────── */
#define EPD_PWR_PIN         GPIO_NUM_6   /* Low  */
#define AUDIO_PWR_PIN       GPIO_NUM_42  /* Low  */
#define VBAT_PWR_PIN        GPIO_NUM_17  /* High */

/* ── Buttons ───────────────────────────────────────── */
#define BOOT_BUTTON_PIN     GPIO_NUM_0
#define PWR_BUTTON_PIN      GPIO_NUM_18

/* ── I2C bus (shared: SHTC3 / RTC / Touch) ─────────── */
#define I2C_SDA_PIN         GPIO_NUM_47
#define I2C_SCL_PIN         GPIO_NUM_48
#define I2C_PORT            I2C_NUM_0

/* ── I2C device addresses ──────────────────────────── */
#define I2C_ADDR_SHTC3      0x70
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_TOUCH      0x38

/* ── Touch ─────────────────────────────────────────── */
#define TP_RST_PIN          GPIO_NUM_7
#define TP_INT_PIN          GPIO_NUM_21

/* ── LED ───────────────────────────────────────────── */
#define LED_PIN             GPIO_NUM_3

#endif /* APP_CONFIG_H */
