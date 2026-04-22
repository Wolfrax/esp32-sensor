#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "i2c_reader.h"

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_PIN     8
#define I2C_SCL_PIN     9
#define I2C_FREQ_HZ     100000

#define BME280_ADDR_1   0x76
#define BME280_ADDR_2   0x77
#define BME280_CHIP_ID  0x60

#define REG_CHIP_ID     0xD0
#define REG_RESET       0xE0
#define REG_STATUS      0xF3
#define REG_CTRL_HUM    0xF2
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7

#define REG_CALIB00     0x88
#define REG_CALIB26     0xE1

static const char *TAG = "bme280";

static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} bme280_calib_data_t;

static bme280_calib_data_t calib;
static int32_t t_fine = 0;

static esp_err_t bme280_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, 100);
}

static esp_err_t bme280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(dev_handle, buf, sizeof(buf), 100);
}

static esp_err_t bme280_add_device(uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
}

static esp_err_t bme280_probe_and_open(uint8_t *detected_addr)
{
    esp_err_t err;

    for (uint8_t addr = BME280_ADDR_1; addr <= BME280_ADDR_2; addr++) {
        err = i2c_master_probe(bus_handle, addr, 100);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at 0x%02X", addr);

            err = bme280_add_device(addr);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed adding device 0x%02X: %s", addr, esp_err_to_name(err));
                return err;
            }

            uint8_t chip_id = 0;
            err = bme280_read_regs(REG_CHIP_ID, &chip_id, 1);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed reading chip ID: %s", esp_err_to_name(err));
                return err;
            }

            if (chip_id == BME280_CHIP_ID) {
                *detected_addr = addr;
                ESP_LOGI(TAG, "BME280 detected at 0x%02X (chip id 0x%02X)", addr, chip_id);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "Device at 0x%02X is not BME280 (chip id 0x%02X)", addr, chip_id);
            return ESP_ERR_NOT_FOUND;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t bme280_read_calibration(void)
{
    uint8_t buf1[26];
    uint8_t buf2[7];

    ESP_ERROR_CHECK(bme280_read_regs(REG_CALIB00, buf1, sizeof(buf1)));
    ESP_ERROR_CHECK(bme280_read_regs(REG_CALIB26, buf2, sizeof(buf2)));

    calib.dig_T1 = (uint16_t)(buf1[1] << 8) | buf1[0];
    calib.dig_T2 = (int16_t)((buf1[3] << 8) | buf1[2]);
    calib.dig_T3 = (int16_t)((buf1[5] << 8) | buf1[4]);

    calib.dig_P1 = (uint16_t)(buf1[7] << 8) | buf1[6];
    calib.dig_P2 = (int16_t)((buf1[9] << 8) | buf1[8]);
    calib.dig_P3 = (int16_t)((buf1[11] << 8) | buf1[10]);
    calib.dig_P4 = (int16_t)((buf1[13] << 8) | buf1[12]);
    calib.dig_P5 = (int16_t)((buf1[15] << 8) | buf1[14]);
    calib.dig_P6 = (int16_t)((buf1[17] << 8) | buf1[16]);
    calib.dig_P7 = (int16_t)((buf1[19] << 8) | buf1[18]);
    calib.dig_P8 = (int16_t)((buf1[21] << 8) | buf1[20]);
    calib.dig_P9 = (int16_t)((buf1[23] << 8) | buf1[22]);

    calib.dig_H1 = buf1[25];
    calib.dig_H2 = (int16_t)((buf2[1] << 8) | buf2[0]);
    calib.dig_H3 = buf2[2];
    calib.dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    calib.dig_H5 = (int16_t)((buf2[5] << 4) | (buf2[4] >> 4));
    calib.dig_H6 = (int8_t)buf2[6];

    return ESP_OK;
}

static esp_err_t bme280_init(void)
{
    ESP_ERROR_CHECK(bme280_write_reg(REG_RESET, 0xB6));
    vTaskDelay(pdMS_TO_TICKS(10));

    // humidity oversampling x1
    ESP_ERROR_CHECK(bme280_write_reg(REG_CTRL_HUM, 0x01));

    // standby 1000 ms, filter off
    ESP_ERROR_CHECK(bme280_write_reg(REG_CONFIG, 0xA0));

    // temp oversampling x1, pressure oversampling x1, normal mode
    ESP_ERROR_CHECK(bme280_write_reg(REG_CTRL_MEAS, 0x27));

    return ESP_OK;
}

static int32_t compensate_temp(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)calib.dig_T1 << 1))) * ((int32_t)calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)calib.dig_T1))) >> 12) *
            ((int32_t)calib.dig_T3)) >> 14;

    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;   // 0.01 deg C
    return T;
}

static uint32_t compensate_press(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)calib.dig_P3) >> 8) + ((var1 * (int64_t)calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)calib.dig_P1)) >> 33;

    if (var1 == 0) {
        return 0;
    }

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)calib.dig_P8) * p) >> 19;

    p = ((p + var1 + var2) >> 8) + (((int64_t)calib.dig_P7) << 4);
    return (uint32_t)p; // Q24.8 Pa
}

static uint32_t compensate_hum(int32_t adc_H)
{
    int32_t v_x1_u32r;

    v_x1_u32r = t_fine - ((int32_t)76800);
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)calib.dig_H4) << 20) -
                    (((int32_t)calib.dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) *
                  (((((((v_x1_u32r * ((int32_t)calib.dig_H6)) >> 10) *
                       (((v_x1_u32r * ((int32_t)calib.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
                     ((int32_t)2097152)) * ((int32_t)calib.dig_H2) + 8192) >> 14));

    v_x1_u32r = v_x1_u32r -
                (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                  ((int32_t)calib.dig_H1)) >> 4);

    if (v_x1_u32r < 0) v_x1_u32r = 0;
    if (v_x1_u32r > 419430400) v_x1_u32r = 419430400;

    return (uint32_t)(v_x1_u32r >> 12); // Q22.10 %RH
}

static esp_err_t bme280_read_values(float *temp_c, float *press_hpa, float *hum_pct)
{
    uint8_t data[8];
    ESP_ERROR_CHECK(bme280_read_regs(REG_PRESS_MSB, data, sizeof(data)));

    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8)  | data[7];

    int32_t temp_x100 = compensate_temp(adc_T);
    uint32_t press_q24_8 = compensate_press(adc_P);
    uint32_t hum_q22_10 = compensate_hum(adc_H);

    *temp_c = temp_x100 / 100.0f;
    *press_hpa = (press_q24_8 / 256.0f) / 100.0f;
    *hum_pct = hum_q22_10 / 1024.0f;

    return ESP_OK;
}

esp_err_t i2c_reader_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t addr = 0;
    err = bme280_probe_and_open(&addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No BME280 found at 0x76 or 0x77");
        return err;
    }

    err = bme280_read_calibration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration: %s", esp_err_to_name(err));
        return err;
    }

    err = bme280_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BME280: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    float temp_c, hum_pct, press_hpa;
    err = bme280_read_values(&temp_c, &press_hpa, &hum_pct);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial dummy read failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "BME280 ready");
    return ESP_OK;
}

esp_err_t i2c_reader_read(float *temp_c, float *hum_pct, float *press_hpa)
{
    if (temp_c == NULL || hum_pct == NULL || press_hpa == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return bme280_read_values(temp_c, press_hpa, hum_pct);
}