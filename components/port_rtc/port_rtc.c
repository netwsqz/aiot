#include <stdio.h>
#include "port_rtc.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtc";

/* PCF85063 register addresses */
#define PCF85063_CTRL1_REG      0x00
#define PCF85063_SEC_REG        0x04
#define PCF85063_RAM_REG        0x03

/* BCD conversion */
static uint8_t bcd2dec(uint8_t val) { return ((val >> 4) * 10) + (val & 0x0F); }
static uint8_t dec2bcd(uint8_t val) { return ((val / 10) << 4) | (val % 10); }

/* Zeller's congruence: 0=Mon ... 6=Sun, we convert to 0=Sun ... 6=Sat */
static uint8_t calc_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3) {
        month += 12;
        year--;
    }
    uint32_t part1 = day;
    uint32_t part2 = ((month + 1) * 26) / 10;
    uint32_t part3 = year;
    uint32_t part4 = year / 4;
    uint32_t part5 = 6 * (year / 100);
    uint32_t part6 = year / 400;
    uint32_t val = (part1 + part2 + part3 + part4 + part5 + part6) % 7;
    /* Zeller: 0=Sat, 1=Sun ... 6=Fri → convert to 0=Sun, 1=Mon ... 6=Sat */
    if (val == 0) val = 7;
    return (uint8_t)(val - 1);
}

void PortRtc_Init(void)
{
    /* Verify device is online by reading RAM register */
    uint8_t ram_val = 0;
    if (i2c_read_buff(rtc_dev_handle, PCF85063_RAM_REG, &ram_val, 1) != 0) {
        ESP_LOGE(TAG, "Device offline");
        return;
    }

    /* Force 24-hour mode and start RTC */
    uint8_t ctrl1 = 0;
    i2c_read_buff(rtc_dev_handle, PCF85063_CTRL1_REG, &ctrl1, 1);
    ctrl1 &= ~(1 << 1);  /* Clear 12h bit → 24h mode */
    ctrl1 &= ~(1 << 5);  /* Clear STOP bit → start RTC */
    i2c_write_buff(rtc_dev_handle, PCF85063_CTRL1_REG, &ctrl1, 1);

    ESP_LOGI(TAG, "PCF85063 initialized (24h mode)");
}

void PortRtc_SetTime(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second)
{
    uint8_t buf[7];
    buf[0] = dec2bcd(second) & 0x7F;
    buf[1] = dec2bcd(minute);
    buf[2] = dec2bcd(hour);
    buf[3] = dec2bcd(day);
    buf[4] = calc_weekday(year, month, day);
    buf[5] = dec2bcd(month);
    buf[6] = dec2bcd(year % 100);

    i2c_write_buff(rtc_dev_handle, PCF85063_SEC_REG, buf, 7);
    ESP_LOGI(TAG, "Set time: %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
}

rtc_datetime_t PortRtc_GetTime(void)
{
    rtc_datetime_t dt = {0};
    uint8_t buf[7] = {0};

    if (i2c_read_buff(rtc_dev_handle, PCF85063_SEC_REG, buf, 7) != 0) {
        ESP_LOGE(TAG, "Read failed");
        return dt;
    }

    dt.second = bcd2dec(buf[0] & 0x7F);
    dt.minute = bcd2dec(buf[1] & 0x7F);
    dt.hour   = bcd2dec(buf[2] & 0x3F);
    dt.day    = bcd2dec(buf[3] & 0x3F);
    dt.week   = bcd2dec(buf[4] & 0x07);
    dt.month  = bcd2dec(buf[5] & 0x1F);
    dt.year   = bcd2dec(buf[6]) + 2000;

    return dt;
}
