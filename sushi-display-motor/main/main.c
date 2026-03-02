#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "gc9a01.h"
#include "ssd1306.h"

// SSD1306 I2C pins
#define I2C_MASTER_SDA_IO           5
#define I2C_MASTER_SCL_IO           6

// GC9A01 SPI Display Pin Definitions
#define GC9A01_SPI_MOSI             1
#define GC9A01_SPI_CLK              2
#define GC9A01_SPI_CS               3
#define GC9A01_SPI_DC               4
#define GC9A01_SPI_RST              7

void app_main(void)
{
    printf("Dual display demo\n");

    // --- SSD1306 (I2C) ---
    i2c_config_t i2c_conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_NUM_0, &i2c_conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    ssd1306_init(I2C_NUM_0);
    ssd1306_clear(I2C_NUM_0);
    ssd1306_draw_text(I2C_NUM_0, 0, 0, "Hello, ESP32!");

    // --- GC9A01 (SPI) ---
    gc9a01_init(GC9A01_SPI_MOSI,
                GC9A01_SPI_CLK,
                GC9A01_SPI_CS,
                GC9A01_SPI_DC,
                -1); // No reset pin

    // Cycle the GC9A01 through colours and overlay the bitmap
    while (1) {
        printf("Fill: RED\n");
        ssd1306_draw_text(I2C_NUM_0, 1, 0, "RED       ");
        gc9a01_fill_color(GC9A01_COLOR_RED);
        gc9a01_draw_bitmap_spiffs("/spiffs/sushiro.bmp", 82, 82);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("Fill: GREEN\n");
        ssd1306_draw_text(I2C_NUM_0, 1, 0, "GREEN     ");
        gc9a01_fill_color(GC9A01_COLOR_GREEN);
        gc9a01_draw_bitmap_spiffs("/spiffs/sushiro.bmp", 82, 82);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("Fill: BLUE\n");
        ssd1306_draw_text(I2C_NUM_0, 1, 0, "BLUE      ");
        gc9a01_fill_color(GC9A01_COLOR_BLUE);
        gc9a01_draw_bitmap_spiffs("/spiffs/sushiro.bmp", 82, 82);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("Fill: BLACK\n");
        ssd1306_draw_text(I2C_NUM_0, 1, 0, "BLACK     ");
        gc9a01_fill_color(GC9A01_COLOR_BLACK);
        gc9a01_draw_bitmap_spiffs("/spiffs/sushiro.bmp", 82, 82);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
