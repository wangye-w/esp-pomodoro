#pragma once

#include "esp_err.h"

#define LCD_HOST        SPI2_HOST
#define PIN_NUM_SCLK    6
#define PIN_NUM_MOSI    7
#define PIN_NUM_MISO    -1

esp_err_t spi_init();