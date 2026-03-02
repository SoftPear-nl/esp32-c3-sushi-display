#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c.h"

#define SSD1306_I2C_ADDR  0x3C
#define SSD1306_WIDTH      72
#define SSD1306_HEIGHT     40
#define SSD1306_COL_OFFSET 28   // 72-wide panel: RAM cols 28..99 are visible

esp_err_t ssd1306_init(i2c_port_t i2c_num);
void ssd1306_clear(i2c_port_t i2c_num);
void ssd1306_draw_text(i2c_port_t i2c_num, uint8_t row, uint8_t col, const char *text);

#endif // SSD1306_H
