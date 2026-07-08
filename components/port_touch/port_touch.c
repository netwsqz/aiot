#include <stdio.h>
#include "port_touch.h"
#include "i2c_bsp.h"
#include "app_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define FT6336_ADDR     0x38
#define FT6336_STATUS   0x02
#define FT6336_TOUCH1   0x03

static i2c_master_dev_handle_t touch_dev = NULL;

void PortTouch_Init(void)
{
    /* Reset: HIGH → LOW → HIGH */
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << TP_RST_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TP_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Add device to I2C bus at 400kHz */
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = FT6336_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    esp_err_t ret = i2c_master_bus_add_device(i2c_get_bus_handle(), &dev_cfg, &touch_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add device failed: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "FT6336 initialized");
}

void PortTouch_Deinit(void)
{
    /* Detach the FT6336 from the shared I2C bus. The device handle is local
     * to this file, so i2c_master_Deinit() cannot remove it itself; if it
     * stays attached when i2c_del_master_bus() runs, ESP-IDF's behavior is
     * undefined (and on some revisions leaks the slot). Safe to call before
     * Init or after a failed Init (touch_dev == NULL → no-op). */
    if (touch_dev) {
        i2c_master_bus_rm_device(touch_dev);
        touch_dev = NULL;
    }
}

uint8_t PortTouch_GetPoint(uint16_t *x, uint16_t *y)
{
    if (!touch_dev) return 0;

    uint8_t data = 0;
    uint8_t buf[4];

    if (i2c_read_buff(touch_dev, FT6336_STATUS, &data, 1) != 0)
        return 0;

    if (data & 0x0F) {
        if (i2c_read_buff(touch_dev, FT6336_TOUCH1, buf, 4) != 0)
            return 0;
        *x = (((uint16_t)buf[0] & 0x0F) << 8) | (uint16_t)buf[1];
        *y = (((uint16_t)buf[2] & 0x0F) << 8) | (uint16_t)buf[3];
        if (*x > 200) *x = 200;
        if (*y > 200) *y = 200;
        return 1;
    }
    return 0;
}
