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

// Position of the kirb_walk overlay (top-left corner, in screen pixels).
// Initialised to centred when the animation loads; adjusted by the 5-way switch.
static volatile int s_walk_x = 0;
static volatile int s_walk_y = 0;
static volatile int s_walk_dir = 1; // 1 = right (default), -1 = left (mirrored)

static const char *s_display_mode_names[] = {
    [DISPLAY_MODE_NORMAL] = "NORMAL",
    [DISPLAY_MODE_WIFI]   = "WIFI",
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

#define HOLD_REPEAT_MS    80  ///< Auto-repeat interval while held in DISPLAY_MODE_WIFI (ms)

static void five_way_switch_task(void *arg)
{
    bool prev[NUM_5WAY_BUTTONS];
    int  hold_ms[NUM_5WAY_BUTTONS];

    for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
        prev[i]    = gpio_get_level(s_5way_buttons[i].pin);
        hold_ms[i] = 0;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        for (int i = 0; i < (int)NUM_5WAY_BUTTONS; i++) {
            bool cur = gpio_get_level(s_5way_buttons[i].pin);
            int  pin = s_5way_buttons[i].pin;

            bool first_press = (!cur &&  prev[i]);
            bool held        = (!cur && !prev[i]);

            if (first_press) {
                hold_ms[i] = 0;
                ESP_LOGI(TAG, "Button pressed: %s", s_5way_buttons[i].name);
            } else if (held) {
                hold_ms[i] += 20;
            } else {
                hold_ms[i] = 0;
            }

            // Center: single fire only.
            // Directional in WIFI mode: fire on first press, then repeat every
            // HOLD_REPEAT_MS with no initial delay.
            // Directional in other modes: first press only.
            bool fire = false;
            if (pin == PIN_5WAY_CENTER) {
                fire = first_press;
            } else if (s_display_mode == DISPLAY_MODE_WIFI) {
                if (first_press) {
                    fire = true;
                } else if (held && hold_ms[i] >= HOLD_REPEAT_MS) {
                    fire = true;
                    hold_ms[i] -= HOLD_REPEAT_MS;
                }
            } else {
                fire = first_press;
            }

            if (fire) {
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
                        if (s_display_mode == DISPLAY_MODE_NORMAL) {
                            current_led_mode = (current_led_mode + 1) % LED_MODE_COUNT;
                            ESP_LOGI(TAG, "LED mode -> %s", s_led_mode_names[current_led_mode]);
                        } else if (s_display_mode == DISPLAY_MODE_WIFI) {
                            if      (pin == PIN_5WAY_LEFT)  { s_walk_x -= 10; s_walk_dir = -1; }
                            else if (pin == PIN_5WAY_RIGHT) { s_walk_x += 10; s_walk_dir =  1; }
                            else if (pin == PIN_5WAY_UP)    s_walk_y -= 10;
                            else if (pin == PIN_5WAY_DOWN)  s_walk_y += 10;
                            // Clamp to the virtual two-screen canvas
                            if (s_walk_x < 0)                  s_walk_x = 0;
                            if (s_walk_x > 2 * LCD_H_RES - 1) s_walk_x = 2 * LCD_H_RES - 1;
                            if (s_walk_y < 0)                  s_walk_y = 0;
                            if (s_walk_y > LCD_V_RES  - 1)     s_walk_y = LCD_V_RES  - 1;
                            ESP_LOGI(TAG, "walk pos -> x=%d y=%d", s_walk_x, s_walk_y);
                        }
                        break;
                    default:
                        break;
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

// -------------------- Display mode: kirb_back.bmp + kirb_walk.bin overlay --------------------
static void display_white_screens(void)
{
    uint16_t *bg1 = NULL;  // 240×LCD_V_RES, left  half → panel 1
    uint16_t *bg2 = NULL;  // 240×LCD_V_RES, right half → panel 2

    // ── Load and split background ─────────────────────────────────────────────
    do {
        FILE *f = fopen("/spiffs/kirb_back.bmp", "rb");
        if (!f) { ESP_LOGE(TAG, "Cannot open /spiffs/kirb_back.bmp"); break; }

        uint8_t hdr[66];
        if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr) ||
                hdr[0] != 'B' || hdr[1] != 'M') {
            ESP_LOGE(TAG, "kirb_back.bmp: bad header");
            fclose(f); break;
        }

        uint32_t po     = (uint32_t)(hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24));
        int32_t  w      = (int32_t) (hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24));
        int32_t  h      = (int32_t) (hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));
        int      width  = w < 0 ? -w : w;
        int      height = h < 0 ? -h : h;
        bool     top_down   = h < 0;
        uint32_t row_stride = ((uint32_t)(width * 2 + 3) / 4) * 4;

        uint16_t *img     = heap_caps_malloc((size_t)width * height * 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *row_buf = malloc(row_stride);
        if (!img || !row_buf) {
            ESP_LOGE(TAG, "kirb_back.bmp: OOM");
            free(img); free(row_buf); fclose(f); break;
        }

        fseek(f, (long)po, SEEK_SET);
        for (int disk_row = 0; disk_row < height; disk_row++) {
            size_t got = fread(row_buf, 1, row_stride, f);
            if (got < (size_t)row_stride)
                memset((uint8_t *)row_buf + got, 0, row_stride - got);
            int img_row = top_down ? disk_row : (height - 1 - disk_row);
            uint16_t *dst = img + (size_t)img_row * width;
            for (int c = 0; c < width; c++) {
                uint16_t px = row_buf[c];
                dst[c] = (uint16_t)((px >> 8) | (px << 8)); // LE→BE
            }
            if ((disk_row & 15) == 15) vTaskDelay(1);
        }
        free(row_buf);
        fclose(f);

        int bg_rows = height < LCD_V_RES ? height : LCD_V_RES;
        bg1 = heap_caps_calloc((size_t)LCD_H_RES * LCD_V_RES, 2,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bg2 = heap_caps_calloc((size_t)LCD_H_RES * LCD_V_RES, 2,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bg1 && bg2) {
            for (int y = 0; y < bg_rows; y++) {
                uint16_t *row = img + (size_t)y * width;
                memcpy(bg1 + (size_t)y * LCD_H_RES, row,             (size_t)LCD_H_RES * 2);
                memcpy(bg2 + (size_t)y * LCD_H_RES, row + LCD_H_RES, (size_t)LCD_H_RES * 2);
            }
        }
        free(img);
    } while (0);

    if (!bg1 || !bg2) {
        ESP_LOGE(TAG, "display_white_screens: failed to load background");
        free(bg1); free(bg2);
        while (s_display_mode == DISPLAY_MODE_WIFI)
            vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }

    // Draw background once
    esp_lcd_panel_draw_bitmap(s_panel1, 0, 0, LCD_H_RES, LCD_V_RES, bg1);
    esp_lcd_panel_draw_bitmap(s_panel2, 0, 0, LCD_H_RES, LCD_V_RES, bg2);

    // ── Animate kirb_walk.bin centred at 3× on top of background ─────────────
    do {
        FILE *af = fopen("/spiffs/kirb_walk.bin", "rb");
        if (!af) { ESP_LOGE(TAG, "Cannot open /spiffs/kirb_walk.bin"); break; }

        uint8_t ahdr[8];
        if (fread(ahdr, 1, 8, af) < 8) {
            ESP_LOGE(TAG, "kirb_walk.bin: short header"); fclose(af); break;
        }
        int     anim_w   = (int)(ahdr[0] | (ahdr[1] << 8));
        int     anim_h   = (int)(ahdr[2] | (ahdr[3] << 8));
        int     frames   = (int)(ahdr[4] | (ahdr[5] << 8));
        int     fps      = (int)(ahdr[6] | (ahdr[7] << 8));
        int64_t frame_us = (fps > 0) ? (1000000LL / fps) : 100000LL;
        long    data_ofs = ftell(af);
        int     sc       = 3;
        int     out_w    = anim_w * sc;
        int     out_h    = anim_h * sc;
        int     draw_w   = out_w < LCD_H_RES ? out_w : LCD_H_RES;
        int     draw_h   = out_h < LCD_V_RES ? out_h : LCD_V_RES;
        int     off_x    = (LCD_H_RES - draw_w) / 2;
        int     off_y    = (LCD_V_RES - draw_h) / 2;
        s_walk_x = off_x;
        s_walk_y = off_y;

        uint16_t *frame_buf = heap_caps_malloc((size_t)anim_w * anim_h * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *comp      = heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame_buf || !comp) {
            ESP_LOGE(TAG, "kirb_walk: OOM");
            free(frame_buf); free(comp); fclose(af); break;
        }

        ESP_LOGI(TAG, "kirb_walk: %dx%d %d frames @%d fps scale=3 off=(%d,%d)",
                 anim_w, anim_h, frames, fps, off_x, off_y);

        bool io_ok = true;
        while (io_ok && s_display_mode == DISPLAY_MODE_WIFI) {
            fseek(af, data_ofs, SEEK_SET);
            for (int fi = 0; fi < frames && s_display_mode == DISPLAY_MODE_WIFI; fi++) {
                int64_t t0 = esp_timer_get_time();

                if (fread(frame_buf, 2, (size_t)anim_w * anim_h, af) <
                        (size_t)(anim_w * anim_h)) { io_ok = false; break; }

                // Composite onto each panel: copy background, then overlay
                // animation pixels (black = transparent) at 3× nearest-neighbour.
                // Snapshot position once per frame so both panels are consistent.
                int cur_x   = s_walk_x;
                int cur_y   = s_walk_y;
                int cur_dir = s_walk_dir;
                for (int panel_idx = 0; panel_idx < 2; panel_idx++) {
                    uint16_t *bg = (panel_idx == 0) ? bg1 : bg2;
                    esp_lcd_panel_handle_t panel = (panel_idx == 0) ? s_panel1 : s_panel2;
                    int panel_origin = panel_idx * LCD_H_RES;

                    memcpy(comp, bg, (size_t)LCD_H_RES * LCD_V_RES * 2);

                    // Each panel renders whatever portion of the sprite's virtual X
                    // range falls within its own [0, LCD_H_RES) window, so the sprite
                    // spans both screens smoothly as it crosses the boundary.
                    for (int src_r = 0; src_r < anim_h; src_r++) {
                        const uint16_t *src_row = frame_buf + (size_t)src_r * anim_w;
                        for (int dr = 0; dr < sc; dr++) {
                            int dst_y = cur_y + src_r * sc + dr;
                            if ((unsigned)dst_y >= (unsigned)LCD_V_RES) continue;
                            uint16_t *dst_row = comp + (size_t)dst_y * LCD_H_RES;
                            for (int src_c = 0; src_c < anim_w; src_c++) {
                                int read_c = (cur_dir < 0) ? (anim_w - 1 - src_c) : src_c;
                                uint16_t px = src_row[read_c];
                                if (px == 0xFF07) continue; // #00FFFF (cyan) = transparent
                                for (int dc = 0; dc < sc; dc++) {
                                    int dst_x = cur_x + src_c * sc + dc - panel_origin;
                                    if ((unsigned)dst_x < (unsigned)LCD_H_RES)
                                        dst_row[dst_x] = px;
                                }
                            }
                        }
                    }

                    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, comp);
                }

                int64_t elapsed_us   = esp_timer_get_time() - t0;
                int64_t remaining_ms = (frame_us - elapsed_us) / 1000;
                TickType_t ticks = (remaining_ms > 0) ? pdMS_TO_TICKS((uint32_t)remaining_ms) : 0;
                vTaskDelay(ticks > 0 ? ticks : 1);
            }
        }

        free(frame_buf);
        free(comp);
        fclose(af);
    } while (0);

    free(bg1);
    free(bg2);
    // Guard: idle if we exited the animation early (e.g. file error)
    while (s_display_mode == DISPLAY_MODE_WIFI)
        vTaskDelay(pdMS_TO_TICKS(100));
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
