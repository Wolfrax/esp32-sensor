#ifndef I2C_READER_H
#define I2C_READER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_reader_init(void);
esp_err_t i2c_reader_read(float *temp_c, float *hum_pct, float *press_hpa);

#ifdef __cplusplus
}
#endif

#endif // I2C_READER_H