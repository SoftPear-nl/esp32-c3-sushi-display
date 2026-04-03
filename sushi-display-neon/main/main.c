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

static const char *TAG = "main";

static const char *s_led_mode_names[] = {
    [LED_MODE_ALL_PATTERNS] = "ALL_PATTERNS",
    [LED_MODE_ON]           = "ON",
    [LED_MODE_OFF]          = "OFF",
};

// -------------------- Display mode --------------------
typedef enum {
    DISPLAY_MODE_NORMAL = 0,
    DISPLAY_MODE_WIFI,
    DISPLAY_MODE_COUNT,
} display_mode_t;

static volatile display_mode_t s_display_mode = DISPLAY_MODE_NORMAL;

static const char *s_display_mode_names[] = {
    [DISPLAY_MODE_NORMAL] = "NORMAL",
    [DISPLAY_MODE_WIFI]   = "WIFI",
};

// -------------------- Cursor (arrow) --------------------
#define CURSOR_W    9                      // bounding-box width  (cols 0-8)
#define CURSOR_H   17                      // bounding-box height (rows 0-16)
#define CURSOR_STEP 10                     // pixels per d-pad press
#define VIRTUAL_W  (LCD_H_RES * 2)         // total virtual width spanning both screens

static volatile int s_cursor_x = 0;  // set to centre each time WIFI mode opens
static volatile int s_cursor_y = 0;

/* Standard NW-pointing arrow cursor.
 * 16-bit row bitmasks; bit 15 = leftmost column (col 0), 1 = black pixel.
 *
 *   col: 0 1 2 3 4 5 6 7 8
 *   row 0:  *
 *   row 1:  * *
 *   row 2:  * * *
 *   row 3:  * * * *
 *   row 4:  * * * * *
 *   row 5:  * * * * * *
 *   row 6:  * * * * * * *
 *   row 7:  * * * * * * * *
 *   row 8:  * * * * * * * * *
 *   row 9:  * * * * * *
 *   row10:  * * * . * * *
 *   row11:  * . . . . * * *
 *   row12:  . . . . . * * *
 *   row13:  . . . . . . * * *
 *   row14:  . . . . . . * * *
 *   row15:  . . . . . . . * *
 *   row16:  . . . . . . . * *
 */
static const uint16_t s_arrow_bitmap[CURSOR_H] = {
    0x8000, // row  0 – tip
    0xC000, // row  1
    0xE000, // row  2
    0xF000, // row  3
    0xF800, // row  4
    0xFC00, // row  5
    0xFE00, // row  6
    0xFF00, // row  7
    0xFF80, // row  8 – widest
    0xFC00, // row  9 – notch cuts right side
    0xEE00, // row 10 – gap between head and shaft
    0x8700, // row 11 – shaft begins
    0x0700, // row 12
    0x0380, // row 13
    0x0380, // row 14
    0x0180, // row 15
    0x0180, // row 16
};

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
#define PIN_LETTER_I        4

// -------------------- 5-way switch --------------------
// NOTE: GPIO 26-32 = internal flash; GPIO 33-40 = OctalSPI PSRAM (N16R8).
//       Use only GPIOs outside those ranges for user I/O.
#define PIN_5WAY_UP         2
#define PIN_5WAY_DOWN       1
#define PIN_5WAY_LEFT      47
#define PIN_5WAY_RIGHT     21
#define PIN_5WAY_CENTER    48

// -------------------- 5-way switch --------------------
static const struct { int pin; const char *name; } s_5way_buttons[] = {
    { PIN_5WAY_UP,     "UP"     },
    { PIN_5WAY_DOWN,   "DOWN"   },
    { PIN_5WAY_LEFT,   "LEFT"   },
    { PIN_5WAY_RIGHT,  "RIGHT"  },
    { PIN_5WAY_CENTER, "CENTER" },
};
#define NUM_5WAY_BUTTONS (sizeof(s_5way_buttons) / sizeof(s_5way_buttons[0]))

#define CURSOR_REPEAT_DELAY_MS  400   // ms before auto-repeat starts
#define CURSOR_REPEAT_RATE_MS    80   // ms between repeated steps while held

// Move cursor one step for the given pin; call on press and on each repeat tick.
static void cursor_move(int pin)
{
    switch (pin) {
        case PIN_5WAY_LEFT: {
            int nx = s_cursor_x - CURSOR_STEP;
            s_cursor_x = nx < 0 ? 0 : nx;
            break;
        }
        case PIN_5WAY_RIGHT: {
            int nx = s_cursor_x + CURSOR_STEP;
            s_cursor_x = nx > VIRTUAL_W - CURSOR_W ? VIRTUAL_W - CURSOR_W : nx;
            break;
        }
        case PIN_5WAY_UP: {
            int ny = s_cursor_y - CURSOR_STEP;
            s_cursor_y = ny < 0 ? 0 : ny;
            break;
        }
        case PIN_5WAY_DOWN: {
            int ny = s_cursor_y + CURSOR_STEP;
            s_cursor_y = ny > LCD_V_RES - CURSOR_H ? LCD_V_RES - CURSOR_H : ny;
            break;
        }
        default:
            break;
    }
}

static void five_way_switch_task(void *arg)
{
    bool     prev[NUM_5WAY_BUTTONS];
    TickType_t held_since[NUM_5WAY_BUTTONS];
    TickType_t last_repeat[NUM_5WAY_BUTTONS];

    for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
        prev[i]        = gpio_get_level(s_5way_buttons[i].pin);
        held_since[i]  = 0;
        last_repeat[i] = 0;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
            bool cur = gpio_get_level(s_5way_buttons[i].pin);
            int  pin = s_5way_buttons[i].pin;

            if (!cur && prev[i]) {
                // ── Falling edge: first press ────────────────────────────────
                ESP_LOGI(TAG, "Button pressed: %s", s_5way_buttons[i].name);
                held_since[i]  = now;
                last_repeat[i] = now;

                switch (pin) {
                    case PIN_5WAY_CENTER:
                        s_display_mode = (s_display_mode + 1) % DISPLAY_MODE_COUNT;
                        ESP_LOGI(TAG, "Display mode -> %s", s_display_mode_names[s_display_mode]);
                        display_sequence_request_abort();
                        break;
                    case PIN_5WAY_LEFT:
                    case PIN_5WAY_RIGHT:
                    case PIN_5WAY_UP:
                    case PIN_5WAY_DOWN:
                        if (s_display_mode == DISPLAY_MODE_WIFI) {
                            cursor_move(pin);
                        } else if (s_display_mode == DISPLAY_MODE_NORMAL) {
                            current_led_mode = (current_led_mode + 1) % LED_MODE_COUNT;
                            ESP_LOGI(TAG, "LED mode -> %s", s_led_mode_names[current_led_mode]);
                        }
                        break;
                    default:
                        break;
                }

            } else if (!cur && !prev[i]) {
                // ── Held: auto-repeat cursor movement in WIFI mode only ───────
                if (s_display_mode == DISPLAY_MODE_WIFI &&
                    (pin == PIN_5WAY_LEFT || pin == PIN_5WAY_RIGHT ||
                     pin == PIN_5WAY_UP   || pin == PIN_5WAY_DOWN)) {
                    TickType_t held_ms   = (now - held_since[i])  * portTICK_PERIOD_MS;
                    TickType_t repeat_ms = (now - last_repeat[i]) * portTICK_PERIOD_MS;
                    if (held_ms  >= CURSOR_REPEAT_DELAY_MS &&
                        repeat_ms >= CURSOR_REPEAT_RATE_MS) {
                        cursor_move(pin);
                        last_repeat[i] = now;
                    }
                }
            }

            prev[i] = cur;
        }
    }
}

static void five_way_switch_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_5WAY_UP)   |
                        (1ULL << PIN_5WAY_DOWN)  |
                        (1ULL << PIN_5WAY_LEFT)  |
                        (1ULL << PIN_5WAY_RIGHT) |
                        (1ULL << PIN_5WAY_CENTER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    xTaskCreate(five_way_switch_task, "5way_sw", 4096, NULL, 5, NULL);
}

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
        .pclk_hz           = 80 * 1000 * 1000,  // 40 MHz; bump to 80 MHz if stable
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

// -------------------- Display mode: white screens + movable cursor --------------------
static void display_white_screens(void)
{
    const size_t line_bytes = LCD_H_RES * sizeof(uint16_t);
    uint16_t *line = heap_caps_malloc(line_bytes, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "Failed to allocate white line buffer");
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }

    // Centre the cursor in the virtual canvas (both screens) every time we enter this mode
    s_cursor_x = (VIRTUAL_W - CURSOR_W) / 2;
    s_cursor_y = (LCD_V_RES - CURSOR_H) / 2;

    int prev_cx = -1, prev_cy = -1; // force a full redraw on first iteration

    while (s_display_mode == DISPLAY_MODE_WIFI) {
        int cx = s_cursor_x;
        int cy = s_cursor_y;

        if (cx == prev_cx && cy == prev_cy) {
            vTaskDelay(pdMS_TO_TICKS(16)); // ~60 fps check rate; skip if nothing moved
            continue;
        }

        // Redraw screen 1 (virtual x 0..239): white + cursor pixels with virtual_x < LCD_H_RES
        for (int y = 0; y < LCD_V_RES; y++) {
            memset(line, 0xFF, line_bytes);
            int row = y - cy;
            if (row >= 0 && row < CURSOR_H) {
                uint16_t mask = s_arrow_bitmap[row];
                for (int bit = 0; bit < 16; bit++) {
                    if (mask & (1u << (15 - bit))) {
                        int vx = cx + bit; // virtual x
                        if (vx >= 0 && vx < LCD_H_RES)
                            line[vx] = 0x0000;
                    }
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel1, 0, y, LCD_H_RES, y + 1, line);
        }

        // Redraw screen 2 (virtual x 240..479): white + cursor pixels with virtual_x >= LCD_H_RES
        for (int y = 0; y < LCD_V_RES; y++) {
            memset(line, 0xFF, line_bytes);
            int row = y - cy;
            if (row >= 0 && row < CURSOR_H) {
                uint16_t mask = s_arrow_bitmap[row];
                for (int bit = 0; bit < 16; bit++) {
                    if (mask & (1u << (15 - bit))) {
                        int vx = cx + bit;           // virtual x
                        int s2x = vx - LCD_H_RES;    // screen-2-local x
                        if (s2x >= 0 && s2x < LCD_H_RES)
                            line[s2x] = 0x0000;
                    }
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel2, 0, y, LCD_H_RES, y + 1, line);
        }

        prev_cx = cx;
        prev_cy = cy;
    }

    heap_caps_free(line);
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
        .first_frame_ms = 10000,
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
    five_way_switch_init(); // also starts the switch polling task

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

    // Run the display loop (loops forever)
    while (1) {
        switch (s_display_mode) {
            case DISPLAY_MODE_WIFI:
                display_white_screens();
                break;
            case DISPLAY_MODE_NORMAL:
            default:
                display_sequence_run(s_panel1, s_panel2,
                                     s_display_sequence,
                                     sizeof(s_display_sequence) / sizeof(s_display_sequence[0]));
                break;
        }
    }
}
