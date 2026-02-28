#include "esp_log.h"
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

#define LCD_H_RES              76
#define LCD_V_RES              284
#define LCD_Y_OFFSET           18
#define LCD_X_OFFSET           82

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  1

#define PIN_NUM_MOSI           0   // Super Mini MOSI
#define PIN_NUM_SCLK           1   // Super Mini SCK
#define PIN_NUM_CS             4   // Super Mini SS
#define PIN_NUM_DC             2
#define PIN_NUM_RST            3
//#define PIN_NUM_LED            8

#define PIN_LETTER_S1       5
#define PIN_LETTER_U        6
#define PIN_LETTER_S2       7
#define PIN_LETTER_H        8
#define PIN_LETTER_I        9

esp_lcd_panel_handle_t s_panel_handle;

typedef uint16_t (*color_swap_fn)(uint16_t);

// Generalized bitmap draw with color swap function
// direction: 0 = horizontal, 1 = vertical
void draw_bitmap_from_spiffs_swap(const char *path, uint16_t bmp_width, uint16_t bmp_height,
                                  uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  uint16_t bmp_x, uint16_t bmp_y,
                                  size_t header_size, uint16_t row_stride,
                                  uint8_t direction, uint16_t speed_ms) {
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Failed to open bitmap: %s\n", path);
        return;
    }
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    // Allocate full image buffer
    uint16_t *img_buf = heap_caps_malloc(bmp_width * bmp_height * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    if (!img_buf) {
        printf("Failed to allocate full image buffer\n");
        fclose(f);
        return;
    }
    // Read full image into buffer
    for (uint16_t row = 0; row < bmp_height; ++row) {
        size_t offset = header_size + row * row_stride * sizeof(uint16_t);
        fseek(f, offset, SEEK_SET);
        size_t read = fread(&img_buf[row * bmp_width], sizeof(uint16_t), bmp_width, f);
        if (read != bmp_width) {
            printf("Failed to read row %u\n", row);
            free(img_buf);
            fclose(f);
            return;
        }
        // Swap bytes for each pixel
        for (uint16_t col = 0; col < bmp_width; ++col) {
            img_buf[row * bmp_width + col] = (img_buf[row * bmp_width + col] >> 8) | (img_buf[row * bmp_width + col] << 8);
        }
        vTaskDelay(1);        
    }
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    fclose(f);

    uint16_t pan_steps = 0;
    if (direction == 0) {
        pan_steps = bmp_width - w;
    } else {
        pan_steps = bmp_height - h;
    }
    uint16_t *rect_buf = heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    if (!rect_buf) {
        printf("Failed to allocate rect buffer\n");
        free(img_buf);
        return;
    }
    for (uint16_t step = 0; step <= pan_steps; ++step) {
        uint16_t cur_bmp_x = bmp_x;
        uint16_t cur_bmp_y = bmp_y;
        if (direction == 0) {
            cur_bmp_x = step;
        } else {
            cur_bmp_y = step;
        }
        // Copy window from full image buffer
        for (uint16_t row = 0; row < h; ++row) {
            for (uint16_t col = 0; col < w; ++col) {
                uint16_t src_row = cur_bmp_y + row;
                uint16_t src_col = cur_bmp_x + col;
                if (src_row < bmp_height && src_col < bmp_width) {
                    rect_buf[row * w + col] = img_buf[src_row * bmp_width + src_col];
                } else {
                    rect_buf[row * w + col] = 0;
                }
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, rect_buf);        
        vTaskDelay(pdMS_TO_TICKS(speed_ms));
    }
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    free(rect_buf);
    free(img_buf);
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

// Draws a full bitmap from SPIFFS at (x, y) coordinate
void draw_bitmap_at(const char *path, uint16_t bmp_width, uint16_t bmp_height, uint16_t x, uint16_t y, size_t header_size, uint16_t row_stride) {
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Failed to open bitmap: %s\n", path);
        return;
    }
    uint16_t *img_buf = heap_caps_malloc(bmp_width * bmp_height * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!img_buf) {
        printf("Failed to allocate image buffer\n");
        fclose(f);
        return;
    }
    // Read full image into buffer
    for (uint16_t row = 0; row < bmp_height; ++row) {
        size_t offset = header_size + row * row_stride * sizeof(uint16_t);
        fseek(f, offset, SEEK_SET);
        size_t read = fread(&img_buf[row * bmp_width], sizeof(uint16_t), bmp_width, f);
        if (read != bmp_width) {
            printf("Failed to read row %u\n", row);
            free(img_buf);
            fclose(f);
            return;
        }
        // Swap bytes for each pixel
        for (uint16_t col = 0; col < bmp_width; ++col) {
            img_buf[row * bmp_width + col] = (img_buf[row * bmp_width + col] >> 8) | (img_buf[row * bmp_width + col] << 8);
        }
        vTaskDelay(1);
    }
    fclose(f);
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + bmp_width, y + bmp_height, img_buf);
    free(img_buf);
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
}

// -------------------- LED blink task --------------------

// New: Heartbeat pattern (all on, pulse, all off)
static void led_heartbeat() {
    for (int i = 0; i < 3; ++i) {
        // All on (long)
        gpio_set_level(PIN_LETTER_S1, 1);
        gpio_set_level(PIN_LETTER_U, 1);
        gpio_set_level(PIN_LETTER_S2, 1);
        gpio_set_level(PIN_LETTER_H, 1);
        gpio_set_level(PIN_LETTER_I, 1);
        vTaskDelay(pdMS_TO_TICKS(320));
        // All off (short)
        gpio_set_level(PIN_LETTER_S1, 0);
        gpio_set_level(PIN_LETTER_U, 0);
        gpio_set_level(PIN_LETTER_S2, 0);
        gpio_set_level(PIN_LETTER_H, 0);
        gpio_set_level(PIN_LETTER_I, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
        // All on (short)
        gpio_set_level(PIN_LETTER_S1, 1);
        gpio_set_level(PIN_LETTER_U, 1);
        gpio_set_level(PIN_LETTER_S2, 1);
        gpio_set_level(PIN_LETTER_H, 1);
        gpio_set_level(PIN_LETTER_I, 1);
        vTaskDelay(pdMS_TO_TICKS(160));
        // All off (long)
        gpio_set_level(PIN_LETTER_S1, 0);
        gpio_set_level(PIN_LETTER_U, 0);
        gpio_set_level(PIN_LETTER_S2, 0);
        gpio_set_level(PIN_LETTER_H, 0);
        gpio_set_level(PIN_LETTER_I, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

// New: Marquee pattern (two lights move together)
static void led_marquee() {
    int pins[5] = {PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 5; ++k) {
                gpio_set_level(pins[k], (k==j || k==j+1) ? 1 : 0);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (int j = 3; j >= 0; --j) {
            for (int k = 0; k < 5; ++k) {
                gpio_set_level(pins[k], (k==j || k==j+1) ? 1 : 0);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    // All off at end
    for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], 0);
}

// New: Random walk (one light moves randomly)
static void led_random_walk() {
    int pins[5] = {PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I};
    int pos = rand() % 5;
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k==pos ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        int dir = rand() % 2 ? 1 : -1;
        pos += dir;
        if (pos < 0) pos = 0;
        if (pos > 4) pos = 4;
    }
    for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], 0);
}
// Alternate each letter, then reverse
static void led_alternate(){
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            gpio_set_level(PIN_LETTER_S1, j==0 ? 1 : 0);
            gpio_set_level(PIN_LETTER_U, j==1 ? 1 : 0);
            gpio_set_level(PIN_LETTER_S2, j==2 ? 1 : 0);
            gpio_set_level(PIN_LETTER_H, j==3 ? 1 : 0);
            gpio_set_level(PIN_LETTER_I, j==4 ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        for (int j = 3; j > 0; --j) {
            gpio_set_level(PIN_LETTER_S1, j==0 ? 1 : 0);
            gpio_set_level(PIN_LETTER_U, j==1 ? 1 : 0);
            gpio_set_level(PIN_LETTER_S2, j==2 ? 1 : 0);
            gpio_set_level(PIN_LETTER_H, j==3 ? 1 : 0);
            gpio_set_level(PIN_LETTER_I, j==4 ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
}

// Wave pattern: lights move outward and inward
static void led_wave(){
    int pattern[8][5] = {
        {1,0,0,0,0},
        {0,1,0,0,0},
        {0,0,1,0,0},
        {0,0,0,1,0},
        {0,0,0,0,1},
        {0,0,0,1,0},
        {0,0,1,0,0},
        {0,1,0,0,0}
    };
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 8; ++j) {
            gpio_set_level(PIN_LETTER_S1, pattern[j][0]);
            gpio_set_level(PIN_LETTER_U,  pattern[j][1]);
            gpio_set_level(PIN_LETTER_S2, pattern[j][2]);
            gpio_set_level(PIN_LETTER_H,  pattern[j][3]);
            gpio_set_level(PIN_LETTER_I,  pattern[j][4]);
            vTaskDelay(pdMS_TO_TICKS(240));
        }
    }
}

// All letters flicker rapidly, then slow
static void led_full_flicker(){
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(PIN_LETTER_S1, 1);
        gpio_set_level(PIN_LETTER_U, 1);
        gpio_set_level(PIN_LETTER_S2, 1);
        gpio_set_level(PIN_LETTER_H, 1);
        gpio_set_level(PIN_LETTER_I, 1);
        vTaskDelay(pdMS_TO_TICKS(160 + i*60));
        gpio_set_level(PIN_LETTER_S1, 0);
        gpio_set_level(PIN_LETTER_U, 0);
        gpio_set_level(PIN_LETTER_S2, 0);
        gpio_set_level(PIN_LETTER_H, 0);
        gpio_set_level(PIN_LETTER_I, 0);
        vTaskDelay(pdMS_TO_TICKS(160 + i*60));
    }
}
// New: chase pattern (lights up one by one, then all off)
static void led_chase() {
    for (int i = 0; i < 3; ++i) {
        gpio_set_level(PIN_LETTER_S1, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(PIN_LETTER_U, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(PIN_LETTER_S2, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(PIN_LETTER_H, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(PIN_LETTER_I, 1); vTaskDelay(pdMS_TO_TICKS(160));
        gpio_set_level(PIN_LETTER_S1, 0);
        gpio_set_level(PIN_LETTER_U, 0);
        gpio_set_level(PIN_LETTER_S2, 0);
        gpio_set_level(PIN_LETTER_H, 0);
        gpio_set_level(PIN_LETTER_I, 0);
        vTaskDelay(pdMS_TO_TICKS(240));
    }
}

// New: bounce pattern (lights move back and forth)
static void led_bounce() {
    int pins[5] = {PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 5; ++j) {
            for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k==j ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (int j = 3; j > 0; --j) {
            for (int k = 0; k < 5; ++k) gpio_set_level(pins[k], k==j ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// New: sparkle pattern (randomly flicker letters)
#include <stdlib.h>
static void led_sparkle() {
    for (int i = 0; i < 20; ++i) {
        gpio_set_level(PIN_LETTER_S1, rand()%2);
        gpio_set_level(PIN_LETTER_U, rand()%2);
        gpio_set_level(PIN_LETTER_S2, rand()%2);
        gpio_set_level(PIN_LETTER_H, rand()%2);
        gpio_set_level(PIN_LETTER_I, rand()%2);
        vTaskDelay(pdMS_TO_TICKS(160));
    }
    // All off at end
    gpio_set_level(PIN_LETTER_S1, 0);
    gpio_set_level(PIN_LETTER_U, 0);
    gpio_set_level(PIN_LETTER_S2, 0);
    gpio_set_level(PIN_LETTER_H, 0);
    gpio_set_level(PIN_LETTER_I, 0);
}

static void led_blink_task(void *arg)
{
    (void)arg;
    while (1) {
        led_alternate();
        led_wave();
        led_full_flicker();
        led_chase();
        led_bounce();
        led_sparkle();
        led_heartbeat();
        led_marquee();
        led_random_walk();
    }
}

// -------------------- Bitmap draw task --------------------
static void draw_bitmap_task(void *arg)
{
    (void)arg;
    while(1) {
        draw_bitmap_at("/spiffs/zanmai.bmp", 76, 76, 0,0, 66, 76);
        draw_bitmap_from_spiffs_swap("/spiffs/zanmaisu.bmp", 370, 208, 0, 76, 76, 208, 0, 0, 66, 370, 0, 40);

        draw_bitmap_at("/spiffs/sushiro.bmp", 76, 76, 0,LCD_V_RES-76, 66, 76);
        draw_bitmap_from_spiffs_swap("/spiffs/sushirosu.bmp", 76, 275, 0, 0, 76, 208, 0, 0, 66, 76, 1, 50);

        ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        draw_bitmap_from_spiffs_swap("/spiffs/okinomi.bmp", 255, 284, 0, 0, 76, 284, 0, 0, 66, 256, 0, 40);
        ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        draw_bitmap_from_spiffs_swap("/spiffs/eel.bmp", 76, 400, 0, 0, 76, 284, 0, 0, 66, 76, 1, 40);
        ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
    vTaskDelete(NULL);
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
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .rgb_endian     = LCD_RGB_DATA_ENDIAN_BIG // 16-bit color data sent as low byte first
        
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, LCD_X_OFFSET, LCD_Y_OFFSET));

    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));
}

// -------------------- app_main --------------------
void app_main(void)
{
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    // LED GPIOs (main LED + letter LEDs)
    gpio_config_t led_cfg = {
        .pin_bit_mask =
            (1ULL << PIN_LETTER_S1)
            | (1ULL << PIN_LETTER_U)
            | (1ULL << PIN_LETTER_S2)
            | (1ULL << PIN_LETTER_H)
            | (1ULL << PIN_LETTER_I),
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


    // Start bitmap drawing task
    xTaskCreate(draw_bitmap_task, "draw_bitmap", 4096, NULL, 2, NULL);

    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 2, NULL);
}