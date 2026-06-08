#include "system_init.h"
#include "lcd_screen.h"

#include "freertos/FreeRTOS.h"
#include "lvgl.h"

void app_main(void)
{
    ESP_ERROR_CHECK(spi_init());

    lcd_screen_init();

    lcd_lvgl_init();

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
