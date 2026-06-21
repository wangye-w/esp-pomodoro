#include "system_init.h"
#include "lcd_screen.h"

#include "freertos/FreeRTOS.h"
#include "lvgl.h"

extern _lock_t lvgl_api_lock;

void app_main(void)
{
    ESP_ERROR_CHECK(spi_init());

    lcd_screen_init();

    lcd_lvgl_init();

    while (1) {
        _lock_acquire(&lvgl_api_lock);
        lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
