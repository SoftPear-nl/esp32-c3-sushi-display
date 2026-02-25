#include <stdbool.h>
#include <assert.h>
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

// -------------------- Display config --------------------
#define LCD_HOST               SPI2_HOST

#define LCD_V_RES              76
#define LCD_H_RES              284
#define LCD_X_OFFSET           18
#define LCD_Y_OFFSET           82

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  0

#define PIN_NUM_MOSI           21
#define PIN_NUM_SCLK           5
#define PIN_NUM_CS             10
#define PIN_NUM_DC             2
#define PIN_NUM_RST            3
#define PIN_NUM_LED            8

esp_lcd_panel_handle_t s_panel_handle;

void draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    // Allocate a buffer for the full rectangle
    uint16_t *rect_buf = heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(rect_buf != NULL);
    for (uint32_t i = 0; i < w * h; ++i) {
        rect_buf[i] = color;
    }
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, rect_buf);
    heap_caps_free(rect_buf);
}

// -------------------- LED blink task --------------------
static void led_blink_task(void *arg)
{
    (void)arg;
    bool led_on = false;

    while (1) {
        led_on = !led_on;
        gpio_set_level(PIN_NUM_LED, led_on);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// -------------------- ST7789 init --------------------
static void init_st7789(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_NUM_SCLK,
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_DISABLED));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS,
        .pclk_hz           = 10 * 1000 * 1000, // 10 MHz
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order  = PANEL_COLOR_ORDER_RGB ? LCD_RGB_ELEMENT_ORDER_RGB : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, LCD_X_OFFSET, LCD_Y_OFFSET));

    // Your orientation choices preserved
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, false));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));
}

// -------------------- app_main --------------------
void app_main(void)
{
    // LED GPIO
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << PIN_NUM_LED,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_conf));

    init_st7789();
    draw_rect(10, 10, 50, 30, 0xF800); // RGB565 red
    // LVGL removed: add manual display code here
    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 2, NULL);
}