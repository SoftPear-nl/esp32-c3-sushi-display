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

#include "lvgl.h"

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

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_BUFFER_LINES      40

static esp_lcd_panel_handle_t s_panel_handle;
static lv_display_t *s_display;

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

// -------------------- LVGL tick --------------------
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// -------------------- LVGL flush callback --------------------
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_map)
{
    // esp_lcd_panel_set_gap() already handles X/Y offsets.
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2;
    const int y2 = area->y2;

    // Draw bitmap: end coords are exclusive => +1
    esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2 + 1, y2 + 1, color_map);

    // Signal LVGL we’re done flushing this area
    lv_display_flush_ready(disp);
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
        .max_transfer_sz = LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

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

// -------------------- LVGL init (double buffer kept) --------------------
static void init_lvgl(void)
{
    lv_init();

    const size_t buffer_size = LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t);

    void *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buf1 && buf2);

    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    // Double buffering (kept)
    lv_display_set_buffers(s_display, buf1, buf2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // LVGL tick timer
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));
}

// -------------------- Create UI (called from LVGL task) --------------------
static void create_ui(void)
{
    // Draw a red square in the center
    lv_obj_t *square = lv_obj_create(lv_screen_active());
    lv_obj_set_size(square, 40, 40); // 40x40 pixels
    lv_obj_set_style_bg_color(square, lv_color_hex(0xFF0000), 0); // Red
    lv_obj_set_style_border_width(square, 0, 0);
    lv_obj_align(square, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(square, 0, 0);
}

// -------------------- LVGL task (single owner -> no mutex) --------------------
static void lvgl_task(void *arg)
{
    (void)arg;

    create_ui();

    while (1) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50) wait_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
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

    init_st7789();
    init_lvgl();

    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 4, NULL);
    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 2, NULL);
}