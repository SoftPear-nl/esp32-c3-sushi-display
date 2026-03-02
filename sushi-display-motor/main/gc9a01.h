#pragma once

#include <stdint.h>
#include "esp_err.h"

#define GC9A01_WIDTH  240
#define GC9A01_HEIGHT 240

// RGB565 color helpers
#define GC9A01_COLOR_RED   0xF800
#define GC9A01_COLOR_GREEN 0x07E0
#define GC9A01_COLOR_BLUE  0x001F
#define GC9A01_COLOR_WHITE 0xFFFF
#define GC9A01_COLOR_BLACK 0x0000

/**
 * @brief Initialize the GC9A01 display over SPI.
 *
 * @param mosi  GPIO number for MOSI
 * @param clk   GPIO number for CLK (SCLK)
 * @param cs    GPIO number for Chip Select
 * @param dc    GPIO number for Data/Command
 * @param rst   GPIO number for Reset, or -1 if RST is tied high / not connected
 * @return ESP_OK on success
 */
esp_err_t gc9a01_init(int mosi, int clk, int cs, int dc, int rst);

/**
 * @brief Set the active drawing window on the display.
 */
void gc9a01_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief Fill the entire 240x240 screen with a single RGB565 colour.
 *
 * @param color 16-bit RGB565 colour value
 */
void gc9a01_fill_color(uint16_t color);

/**
 * @brief Draw a 16-bit RGB565 BMP from SPIFFS at the given screen position.
 *
 * The image is clipped to the screen edges if it would overflow.
 * SPIFFS must already be mounted, or will be auto-mounted on first call.
 *
 * @param path  Full VFS path to the file, e.g. "/spiffs/image.bmp"
 * @param x     Left edge of the destination rectangle (pixels from left)
 * @param y     Top edge of the destination rectangle (pixels from top)
 * @return ESP_OK on success, or an error code
 */
esp_err_t gc9a01_draw_bitmap_spiffs(const char *path, uint16_t x, uint16_t y);
