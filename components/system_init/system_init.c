#include "system_init.h"
#include "lcd_screen.h"

#include "esp_log.h"
#include "driver/spi_master.h"

static const char *TAG = "system_init";

esp_err_t spi_init()
{
    esp_err_t ret;
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES / 8,
    };
    ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    return ret;
}