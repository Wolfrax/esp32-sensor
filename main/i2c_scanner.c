#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"

#define I2C_PORT       I2C_NUM_0
#define I2C_SDA_PIN    8
#define I2C_SCL_PIN    9
#define I2C_FREQ_HZ    100000

static const char *TAG = "i2c_scanner";

void app_main(void)
{
    esp_err_t err;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle;
    err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Scanning I2C bus on SDA=%d SCL=%d", I2C_SDA_PIN, I2C_SCL_PIN);

    while (1) {
        int found = 0;

        for (uint8_t addr = 1; addr < 127; addr++) {
            err = i2c_master_probe(bus_handle, addr, 50);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Found device at 0x%02X", addr);
                found++;
            }
        }

        if (found == 0) {
            ESP_LOGW(TAG, "No I2C devices found");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}