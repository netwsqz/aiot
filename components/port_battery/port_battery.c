#include <stdio.h>
#include "port_battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

static adc_cali_handle_t cali_handle = NULL;
static adc_oneshot_unit_handle_t adc1_handle = NULL;

#define BATT_VOLT_FULL      4.12f
#define BATT_VOLT_EMPTY     3.0f

void PortBattery_Init(void)
{
    /* ADC calibration */
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

    /* ADC unit */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    /* ADC channel (GPIO4 = ADC1_CH3) */
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_config));

    ESP_LOGI(TAG, "Battery ADC initialized (GPIO4, ADC1_CH3)");
}

float PortBattery_GetVoltage(void)
{
    int adc_raw = 0;
    int voltage_mv = 0;

    if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &adc_raw) != ESP_OK)
        return 0.0f;

    adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv);
    /* Voltage divider: 2:1, so multiply by 2 */
    return 0.001f * voltage_mv * 2.0f;
}

uint8_t PortBattery_GetLevel(void)
{
    float vol = PortBattery_GetVoltage();
    if (vol <= BATT_VOLT_EMPTY) return 0;
    if (vol >= BATT_VOLT_FULL) return 100;
    return (uint8_t)((vol - BATT_VOLT_EMPTY) / (BATT_VOLT_FULL - BATT_VOLT_EMPTY) * 100.0f);
}
