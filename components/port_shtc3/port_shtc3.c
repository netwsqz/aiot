#include <stdio.h>
#include "port_shtc3.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

/* SHTC3 command codes (2-byte, MSB first) */
#define SHTC3_WAKEUP            0x3517
#define SHTC3_SLEEP             0xB098
#define SHTC3_SOFT_RESET        0x805D
#define SHTC3_READ_ID           0xEFC8
#define SHTC3_MEAS_T_RH_POLL    0x7866

#define CRC_POLYNOMIAL          0x131
#define SHTC3_TEMP_OFFSET       4.0f

static uint16_t shtc3_id = 0;

static int send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff) };
    return i2c_write_buff(shtc3_handle, -1, buf, 2);
}

static int check_crc(uint8_t data[], uint8_t nbrOfBytes, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < nbrOfBytes; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80)
                crc = (crc << 1) ^ CRC_POLYNOMIAL;
            else
                crc = (crc << 1);
        }
    }
    return (crc == checksum) ? 0 : -1;
}

void PortShtc3_Init(void)
{
    send_cmd(SHTC3_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(50));
    send_cmd(SHTC3_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Read ID */
    uint8_t cmd[2] = { (uint8_t)(SHTC3_READ_ID >> 8), (uint8_t)(SHTC3_READ_ID & 0xff) };
    uint8_t rbuf[3] = {0};
    if (i2c_master_write_read_dev(shtc3_handle, cmd, 2, rbuf, 3) == 0) {
        if (check_crc(rbuf, 2, rbuf[2]) == 0) {
            shtc3_id = (rbuf[0] << 8) | rbuf[1];
            ESP_LOGI(TAG, "ID: 0x%04x", shtc3_id);
        }
    }
    send_cmd(SHTC3_SLEEP);
}

shtc3_data_t PortShtc3_Read(void)
{
    shtc3_data_t data = { .Temp = 0, .RH = 0 };

    /* Wakeup */
    send_cmd(SHTC3_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Start measurement */
    if (send_cmd(SHTC3_MEAS_T_RH_POLL) != 0) {
        ESP_LOGE(TAG, "Measure cmd failed");
        send_cmd(SHTC3_SLEEP);
        return data;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Read 6 bytes: [T_MSB, T_LSB, T_CRC, H_MSB, H_LSB, H_CRC] */
    uint8_t bytes[6] = {0};
    if (i2c_read_buff(shtc3_handle, -1, bytes, 6) != 0) {
        ESP_LOGE(TAG, "Read failed");
        send_cmd(SHTC3_SLEEP);
        return data;
    }

    /* CRC check */
    if (check_crc(bytes, 2, bytes[2]) != 0) {
        ESP_LOGE(TAG, "Temp CRC error");
        send_cmd(SHTC3_SLEEP);
        return data;
    }
    if (check_crc(bytes + 3, 2, bytes[5]) != 0) {
        ESP_LOGE(TAG, "Humi CRC error");
        send_cmd(SHTC3_SLEEP);
        return data;
    }

    /* Calculate */
    uint16_t rawT = (bytes[0] << 8) | bytes[1];
    uint16_t rawH = (bytes[3] << 8) | bytes[4];
    data.Temp = 175.0f * rawT / 65536.0f - 45.0f - SHTC3_TEMP_OFFSET;
    data.RH = 100.0f * rawH / 65536.0f;

    /* Sleep */
    send_cmd(SHTC3_SLEEP);
    return data;
}
