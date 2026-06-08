idf_component_register(SRCS "lcd_screen.c"
                    REQUIRES system_init lvgl__lvgl esp_lcd espressif__esp_lcd_ssd1681
                    INCLUDE_DIRS "include")
