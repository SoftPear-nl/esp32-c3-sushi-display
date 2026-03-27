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

// -------------------- Display config --------------------
#define LCD_HOST               SPI2_HOST

#define LCD_H_RES              240
#define LCD_V_RES              320
#define LCD_Y_OFFSET           0
#define LCD_X_OFFSET           0

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  1

#define PIN_NUM_MOSI           0   // Super Mini MOSI
#define PIN_NUM_SCLK           1   // Super Mini SCK
#define PIN_NUM_CS             4   // Super Mini SS
#define PIN_NUM_DC             2
#define PIN_NUM_RST            3
#define PIN_NUM_CS2            20

#define PIN_LETTER_S1       5 
#define PIN_LETTER_U        6
#define PIN_LETTER_S2       7
#define PIN_LETTER_H        8
#define PIN_LETTER_I        10

esp_lcd_panel_handle_t s_panel_handle;
esp_lcd_panel_handle_t s_panel_handle2;

// -------------------- BMP display functions --------------------

/* Parsed information from a BMP file header */
typedef struct {
    int32_t  width;
    int32_t  height;
    bool     top_down;      /* true when original height was negative */
    uint32_t row_stride;    /* bytes per scanline (padded to 4-byte boundary) */
    uint32_t pixel_offset;  /* byte offset where pixel data begins */
} bmp_info_t;

/* Read and validate BMP header from an already-open FILE*.
   Supports 16-bpp BI_BITFIELDS bitmaps (RGB565) as used in this project.
   NOTE: pixel data bytes are in native little-endian order as stored by BMP.
         If the display shows incorrect colours, byte-swap each uint16_t after
         reading (swapped = (v << 8) | (v >> 8)). */
static esp_err_t bmp_read_info(FILE *f, bmp_info_t *out)
{
    uint8_t hdr[66];
    rewind(f);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return ESP_ERR_INVALID_SIZE;
    if (hdr[0] != 'B' || hdr[1] != 'M')               return ESP_ERR_INVALID_ARG;

    uint16_t bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    if (bpp != 16) {
        ESP_LOGE("BMP", "Only 16-bpp BMPs are supported (got %d bpp)", bpp);
        return ESP_ERR_INVALID_ARG;
    }

    out->pixel_offset = (uint32_t)(hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24));

    int32_t raw_w = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8)
                             | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int32_t raw_h = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8)
                             | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));

    out->width     = raw_w < 0 ? -raw_w : raw_w;
    out->height    = raw_h < 0 ? -raw_h : raw_h;
    out->top_down  = (raw_h < 0);
    /* Row padded to 4-byte boundary */
    out->row_stride = (((uint32_t)out->width * 2u) + 3u) & ~3u;
    return ESP_OK;
}

/* Draw a BMP image at (x, y) on the given panel.
   Loads all pixel data into one heap buffer, byte-swaps, then sends in a
   single draw_bitmap call. */
static esp_err_t bmp_draw_direct(esp_lcd_panel_handle_t panel, FILE *f,
                                  const bmp_info_t *bmp, int x, int y)
{
    int vis_x1 = x < 0 ? 0 : x;
    int vis_y1 = y < 0 ? 0 : y;
    int vis_x2 = (x + bmp->width)  > LCD_H_RES ? LCD_H_RES : (x + bmp->width);
    int vis_y2 = (y + bmp->height) > LCD_V_RES ? LCD_V_RES : (y + bmp->height);
    if (vis_x1 >= vis_x2 || vis_y1 >= vis_y2) return ESP_OK;

    int vis_w   = vis_x2 - vis_x1;
    int vis_h   = vis_y2 - vis_y1;
    int src_col = vis_x1 - x;

    uint16_t *buf = malloc((size_t)vis_w * (size_t)vis_h * sizeof(uint16_t));
    if (!buf) {
        ESP_LOGE("BMP", "alloc failed (%u bytes)",
                 (unsigned)((size_t)vis_w * vis_h * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    for (int r = 0; r < vis_h; r++) {
        int bmp_row = (vis_y1 - y) + r;
        if (!bmp->top_down) bmp_row = bmp->height - 1 - bmp_row;
        fseek(f, (long)bmp->pixel_offset + (long)bmp_row * (long)bmp->row_stride
                 + (long)src_col * 2, SEEK_SET);
        uint16_t *dst = buf + (size_t)r * vis_w;
        fread(dst, sizeof(uint16_t), (size_t)vis_w, f);
        for (int c = 0; c < vis_w; c++) {
            uint16_t v = dst[c];
            dst[c] = (uint16_t)((v << 8) | (v >> 8));
        }
    }

    esp_lcd_panel_draw_bitmap(panel, vis_x1, vis_y1, vis_x2, vis_y2, buf);
    free(buf);
    return ESP_OK;
}

esp_err_t display_bitmap(esp_lcd_panel_handle_t panel, const char *path, int x, int y)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("BMP", "Cannot open: %s", path); return ESP_ERR_NOT_FOUND; }
    bmp_info_t bmp;
    esp_err_t err = bmp_read_info(f, &bmp);
    if (err == ESP_OK) err = bmp_draw_direct(panel, f, &bmp, x, y);
    fclose(f);
    return err;
}

// -------------------- Bitmap draw task --------------------
static void draw_bitmap_task(void *arg)
{
    (void)arg;
    while (1) {
        // Screen 1
        display_bitmap(s_panel_handle,  "/spiffs/sushiro.bmp", 0, 0);
        // Screen 2
        display_bitmap(s_panel_handle2, "/spiffs/zanmai.bmp",  0, 0);

        vTaskDelay(pdMS_TO_TICKS(5000));

        // Screen 1
        display_bitmap(s_panel_handle,  "/spiffs/okinomi.bmp", 0, 0);
        // Screen 2
        display_bitmap(s_panel_handle2, "/spiffs/pommes.bmp",  0, 0);

        vTaskDelay(pdMS_TO_TICKS(5000));
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

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    // Second panel on the same SPI bus — only needs a separate CS pin.
    // RST is shared with panel 0, so use software reset (reset_gpio_num = -1)
    // to avoid re-resetting the already-initialised first panel.
    esp_lcd_panel_io_handle_t io_handle2 = NULL;
    esp_lcd_panel_io_spi_config_t io_config2 = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS2,
        .pclk_hz           = 10 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config2, &io_handle2));

    esp_lcd_panel_dev_config_t panel_config2 = {
        .reset_gpio_num = -1,  // software reset; RST pin is shared with panel 0
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .rgb_endian     = LCD_RGB_DATA_ENDIAN_BIG,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle2, &panel_config2, &s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle2));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle2, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle2, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle2, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle2, true));

}

// -------------------- app_main --------------------
void app_main(void)
{
    ESP_LOGI("MEM", "Free heap: %u bytes, Minimum free heap: %u bytes", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    neon_letter_init(PIN_LETTER_S1, PIN_LETTER_U, PIN_LETTER_S2, PIN_LETTER_H, PIN_LETTER_I);

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
    xTaskCreate(draw_bitmap_task, "draw_bitmap", 8192, NULL, 2, NULL);
}
