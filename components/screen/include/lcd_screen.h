#pragma once

#define LCD_H_RES   200
#define LCD_V_RES   200

#define PIN_NUM_DC  9
#define PIN_NUM_CS  10
#define LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8
#define PIN_NUM_BUSY    18
#define PIN_NUM_RST     4

void lcd_screen_init();

void lcd_lvgl_init();