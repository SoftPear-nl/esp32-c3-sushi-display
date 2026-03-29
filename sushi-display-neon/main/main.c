#include "esp_log.h"
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"

#include "esp_spiffs.h"
#include "neon_letter_service.h"
#include "display_sequence.h"

// -------------------- Display config --------------------
// ESP32-S3 N16R8: two independent SPI buses, one per screen
#define LCD_HOST               SPI2_HOST   // Screen 1
#define LCD_HOST2              SPI3_HOST   // Screen 2

#define LCD_H_RES              240
#define LCD_V_RES              320
#define LCD_Y_OFFSET           0
#define LCD_X_OFFSET           0

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  1

// Screen 1 — SPI2
#define PIN_NUM_MOSI           11
#define PIN_NUM_SCLK           12
#define PIN_NUM_CS             10
#define PIN_NUM_DC             13
#define PIN_NUM_RST            14

// Screen 2 — SPI3 (independent bus: runs in parallel with SPI2)
#define PIN_NUM_MOSI2          5
#define PIN_NUM_SCLK2          6
#define PIN_NUM_CS2            7
#define PIN_NUM_DC2            8
#define PIN_NUM_RST2           9

#define PIN_LETTER_S1       15
#define PIN_LETTER_U        16
#define PIN_LETTER_S2       17
#define PIN_LETTER_H        18
#define PIN_LETTER_I        21

// -------------------- LCD handles --------------------
static esp_lcd_panel_handle_t s_panel1 = NULL;
static esp_lcd_panel_handle_t s_panel2 = NULL;

// -------------------- LCD init --------------------
static void lcd_init(void)
{
    // ── SPI bus 1 (SPI2) ─────────────────────────────────────────────────────
    spi_bus_config_t bus1 = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_NUM_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // Allow full-screen DMA in one shot (240×320×2 bytes)
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus1, SPI_DMA_CH_AUTO));

    // ── SPI bus 2 (SPI3) ─────────────────────────────────────────────────────
    spi_bus_config_t bus2 = {
        .mosi_io_num     = PIN_NUM_MOSI2,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_NUM_SCLK2,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST2, &bus2, SPI_DMA_CH_AUTO));

    // ── Panel IO 1 ────────────────────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io1 = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg1 = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS,
        .pclk_hz           = 40 * 1000 * 1000,  // 40 MHz; bump to 80 MHz if stable
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg1, &io1));

    // ── Panel IO 2 ────────────────────────────────────────────────────────────
    esp_lcd_panel_io_handle_t io2 = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg2 = {
        .dc_gpio_num       = PIN_NUM_DC2,
        .cs_gpio_num       = PIN_NUM_CS2,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST2, &io_cfg2, &io2));

    // ── ST7789 panel 1 ────────────────────────────────────────────────────────
    esp_lcd_panel_dev_config_t pcfg1 = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order  = PANEL_COLOR_ORDER_RGB
                              ? LCD_RGB_ELEMENT_ORDER_RGB
                              : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io1, &pcfg1, &s_panel1));

    // ── ST7789 panel 2 ────────────────────────────────────────────────────────
    esp_lcd_panel_dev_config_t pcfg2 = {
        .reset_gpio_num = PIN_NUM_RST2,
        .rgb_ele_order  = PANEL_COLOR_ORDER_RGB
                              ? LCD_RGB_ELEMENT_ORDER_RGB
                              : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io2, &pcfg2, &s_panel2));

    // ── Reset, init, turn on ──────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel1));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel1));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel2));

    // Uncomment if colours are inverted on your specific module:
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel1, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel2, true));

    // Pixel-memory offset (usually 0,0 for 240×320 ST7789)
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel1, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel2, LCD_X_OFFSET, LCD_Y_OFFSET));

    // Rotate 180°: mirror both axes (no XY-swap needed)
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel1, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel2, true, true));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel1, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel2, true));
}

// -------------------- Display sequence --------------------
// Edit this table to configure what plays on which screen and for how long.
static const scene_t s_display_sequence[] = {

    // 1. Static zanmai.bmp on screen 1  ──┐ run simultaneously
    {                                      //  │
        .type        = SCENE_STATIC,       //  │
        .file_path   = "/spiffs/zanmai.bmp",
        .screen      = SCREEN_1,
        .duration_ms = 3000,
        .parallel    = true,  // <-- paired with the scene below
    },
    // 2. Pan okinomi.bmp on screen 2    ──┘
    {
        .type        = SCENE_PAN,
        .file_path   = "/spiffs/okinomi.bmp",
        .screen      = SCREEN_2,
        .pan_dir     = DIR_LEFT,
        .pan_step_ms = 80,
    },

    // 3. Bounce sushiro.bmp on screen 1  ──┐ run simultaneously
    {                                       //  │
        .type            = SCENE_BOUNCE,    //  │
        .file_path       = "/spiffs/sushiro.bmp",
        .screen          = SCREEN_1,
        .bounce_dx       = 3,
        .bounce_dy       = 2,
        .bounce_step_ms  = 16,
        .bounce_dur_ms   = 6000,
        .parallel        = true,  // <-- paired with the scene below
    },
    // 4. Static zanmai.bmp on screen 2  ──┘
    {
        .type        = SCENE_STATIC,
        .file_path   = "/spiffs/zanmai.bmp",
        .screen      = SCREEN_2,
        .duration_ms = 6000,
    },

    // 5. Kirby fly animation spanning both screens
    {
        .type           = SCENE_ANIM_DUAL,
        .file_path      = "/spiffs/kirbyfly.bin",
        .first_frame_ms = 2000,
        .last_frame_ms  = 10000,
        .dual_mode      = DUAL_SPLIT_H,
        .scale          = 2,
    },
};

// -------------------- app_main --------------------
void app_main(void)
{
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    neon_letter_init(PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I);

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path            = "/spiffs",
        .partition_label      = NULL,
        .max_files            = 5,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_conf));

    // Initialise LCD panels
    lcd_init();

    // Run the display sequence (loops forever)
    while (1) {
        display_sequence_run(s_panel1, s_panel2,
                             s_display_sequence,
                             sizeof(s_display_sequence) / sizeof(s_display_sequence[0]));
    }
}
