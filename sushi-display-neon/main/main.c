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

#define LCD_H_RES              170
#define LCD_V_RES              320
#define LCD_Y_OFFSET           0
#define LCD_X_OFFSET           35

// Panel color order: set to 1 for RGB, 0 for BGR
#define PANEL_COLOR_ORDER_RGB  1

#define PIN_NUM_MOSI           0   // Super Mini MOSI
#define PIN_NUM_SCLK           1   // Super Mini SCK
#define PIN_NUM_CS             4   // Super Mini SS
#define PIN_NUM_DC             2
#define PIN_NUM_RST            3
#define PIN_NUM_BL             20

#define PIN_LETTER_S1       5
#define PIN_LETTER_U        6
#define PIN_LETTER_S2       7
#define PIN_LETTER_H        8
#define PIN_LETTER_I        10

esp_lcd_panel_handle_t s_panel_handle;

// -------------------- BMP display functions --------------------

/* Static pixel buffer: holds raw pixel data loaded from SPIFFS.
   Must fit the largest image's pixel data (~154 KB for zanmaisu.bmp).
   The draw buffer is allocated from heap at call time, which is safe because
   reducing this buffer from 240 KB frees enough heap for even a full-screen
   draw buffer (170 x 320 x 2 = ~106 KB). */
#define BMP_PIXEL_BUF_SIZE (170u * 1024u)
static uint8_t s_pixel_buf[BMP_PIXEL_BUF_SIZE];

/* Direction for display_bitmap_pan() */
typedef enum {
    PAN_LEFT_TO_RIGHT,
    PAN_RIGHT_TO_LEFT,
    PAN_TOP_TO_BOTTOM,
    PAN_BOTTOM_TO_TOP,
} pan_direction_t;

/* Parsed information from a BMP file header */
typedef struct {
    int32_t  width;
    int32_t  height;
    bool     top_down;      /* true when original height was negative */
    uint32_t row_stride;    /* bytes per scanline (padded to 4-byte boundary) */
    uint32_t pixel_offset;  /* byte offset where pixel data begins */
} bmp_info_t;

/* Fill a clipped rectangle on the display with a single RGB565 colour.
   Coordinates are in display pixels (0,0 = top-left). */
static void lcd_fill_rect(int x1, int y1, int x2, int y2, uint16_t color)
{
    if (x1 < 0)          x1 = 0;
    if (y1 < 0)          y1 = 0;
    if (x2 > LCD_H_RES)  x2 = LCD_H_RES;
    if (y2 > LCD_V_RES)  y2 = LCD_V_RES;
    if (x1 >= x2 || y1 >= y2) return;

    int w = x2 - x1;
    int h = y2 - y1;
    uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
    if (!buf) return;
    for (int i = 0; i < w * h; i++) buf[i] = color;
    esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, buf);
    free(buf);
}

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

/* Internal: load all pixel data from an already-open BMP file into the
   supplied buffer.  Uses the static s_pixel_buf â€” no heap allocation.
   Returns ESP_OK on success. */
static esp_err_t bmp_load_pixels(FILE *f, const bmp_info_t *bmp,
                                  uint8_t *buf, size_t buf_size)
{
    size_t data_size = (size_t)bmp->height * bmp->row_stride;
    ESP_LOGI("BMP", "static buf: %u bytes, loading %u bytes for pixel data. free mem: %u bytes",
             (unsigned)buf_size, (unsigned)data_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (data_size > buf_size) {
        ESP_LOGE("BMP", "Pixel data %u bytes exceeds static buffer %u bytes",
                 (unsigned)data_size, (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }
    fseek(f, (long)bmp->pixel_offset, SEEK_SET);
    if (fread(buf, 1, data_size, f) != data_size) {
        ESP_LOGE("BMP", "Short read of pixel data");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Internal: draw the visible portion of a BMP from a RAM pixel buffer placed
   at display position (img_x, img_y), clipped to the rectangle
   [clip_x1, clip_x2) x [clip_y1, clip_y2) (further clamped to display bounds).
   Pass clip_x1=0, clip_y1=0, clip_x2=LCD_H_RES, clip_y2=LCD_V_RES for a
   full-screen draw. No SPIFFS/flash access occurs here. */
static esp_err_t bmp_draw_clipped(const bmp_info_t *bmp, const uint8_t *pixels,
                                   int img_x, int img_y,
                                   int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                                   uint16_t *ext_buf)
{
    /* Clamp clip rect to display bounds */
    if (clip_x1 < 0)         clip_x1 = 0;
    if (clip_y1 < 0)         clip_y1 = 0;
    if (clip_x2 > LCD_H_RES) clip_x2 = LCD_H_RES;
    if (clip_y2 > LCD_V_RES) clip_y2 = LCD_V_RES;

    /* Compute intersection of image rect with clip rect */
    int vis_x1 = img_x < clip_x1 ? clip_x1 : img_x;
    int vis_y1 = img_y < clip_y1 ? clip_y1 : img_y;
    int vis_x2 = (img_x + bmp->width)  > clip_x2 ? clip_x2 : (img_x + bmp->width);
    int vis_y2 = (img_y + bmp->height) > clip_y2 ? clip_y2 : (img_y + bmp->height);
    if (vis_x1 >= vis_x2 || vis_y1 >= vis_y2) return ESP_OK;

    int src_col = vis_x1 - img_x;   /* first visible column in BMP space */
    int src_row = vis_y1 - img_y;   /* first visible row in BMP space    */
    int vis_w   = vis_x2 - vis_x1;
    int vis_h   = vis_y2 - vis_y1;

    uint16_t *draw_buf = ext_buf ? ext_buf : malloc((size_t)vis_w * vis_h * sizeof(uint16_t));
    if (!draw_buf) {
        ESP_LOGE("BMP", "draw buffer alloc failed (%u bytes)",
                 (unsigned)((size_t)vis_w * vis_h * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    for (int r = 0; r < vis_h; r++) {
        int bmp_row = src_row + r;
        if (!bmp->top_down) {
            bmp_row = bmp->height - 1 - bmp_row;
        }
        const uint16_t *src_row_ptr = (const uint16_t *)(
            pixels + (size_t)bmp_row * bmp->row_stride + (size_t)src_col * 2);
        uint16_t *dst_row_ptr = &draw_buf[(size_t)r * vis_w];
        for (int c = 0; c < vis_w; c++) {
            uint16_t v = src_row_ptr[c];
            /* BMP is little-endian; esp_lcd with LCD_RGB_DATA_ENDIAN_BIG needs
               big-endian, so byte-swap each pixel. */
            dst_row_ptr[c] = (uint16_t)((v << 8) | (v >> 8));
        }
        if ((r & 15) == 15) taskYIELD();
    }

    esp_lcd_panel_draw_bitmap(s_panel_handle, vis_x1, vis_y1, vis_x2, vis_y2, draw_buf);
    if (!ext_buf) free(draw_buf);
    return ESP_OK;
}

/**
 * @brief Draw a static BMP image at pixel position (x, y) on the display.
 *
 * The image is clipped to the display boundaries, so (x, y) can be negative
 * or partially off-screen.  All 16-bpp RGB565 BMPs stored in SPIFFS are
 * supported.
 *
 * @param path  SPIFFS path, e.g. "/spiffs/sushiro.bmp"
 * @param x     Horizontal pixel offset (display left edge = 0)
 * @param y     Vertical   pixel offset (display top  edge = 0)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file cannot be opened,
 *         ESP_ERR_INVALID_ARG for unsupported format, ESP_ERR_NO_MEM if
 *         allocation fails.
 */
esp_err_t display_bitmap(const char *path, int x, int y)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE("BMP", "Cannot open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    bmp_info_t bmp;
    esp_err_t err = bmp_read_info(f, &bmp);
    if (err != ESP_OK) { fclose(f); return err; }

    /* Load all pixel data into the static RAM buffer, then close the file
       immediately so SPIFFS flash is no longer accessed during the draw. */
    err = bmp_load_pixels(f, &bmp, s_pixel_buf, sizeof(s_pixel_buf));
    fclose(f);
    if (err != ESP_OK) return err;

    return bmp_draw_clipped(&bmp, s_pixel_buf, x, y, 0, 0, LCD_H_RES, LCD_V_RES, NULL);
}

/**
 * @brief Pan a BMP image inside a defined display window.
 *
 * The pan scrolls the image viewport from one edge to the other within the
 * specified window rectangle.  Only pixels inside the window are updated;
 * the rest of the display is untouched.  The window is filled with black
 * once before the animation starts.
 *
 * @param path       SPIFFS path, e.g. "/spiffs/eel.bmp"
 * @param direction  PAN_LEFT_TO_RIGHT, PAN_RIGHT_TO_LEFT,
 *                   PAN_TOP_TO_BOTTOM, or PAN_BOTTOM_TO_TOP
 * @param win_x      Left edge of the pan window on the display (pixels)
 * @param win_y      Top  edge of the pan window on the display (pixels)
 * @param win_w      Width  of the pan window (pixels)
 * @param win_h      Height of the pan window (pixels)
 * @param step_px    Pixels to advance per frame (1 = smoothest, higher = faster)
 * @param delay_ms   Milliseconds to wait between frames (0 = no delay)
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t display_bitmap_pan(const char *path, pan_direction_t direction,
                              int win_x, int win_y, int win_w, int win_h,
                              int step_px, int delay_ms)
{
    if (step_px <= 0) step_px = 1;
    if (win_w   <= 0) win_w   = LCD_H_RES;
    if (win_h   <= 0) win_h   = LCD_V_RES;

    /* Allocate the frame draw buffer FIRST, before loading pixel data, to
       guarantee the heap has a large contiguous block available. */
    uint16_t *draw_buf = malloc((size_t)win_w * win_h * sizeof(uint16_t));
    if (!draw_buf) {
        ESP_LOGE("BMP", "pan draw buffer alloc failed (%u bytes)",
                 (unsigned)((size_t)win_w * win_h * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE("BMP", "Cannot open: %s", path);
        free(draw_buf);
        return ESP_ERR_NOT_FOUND;
    }
    bmp_info_t bmp;
    esp_err_t err = bmp_read_info(f, &bmp);
    if (err != ESP_OK) { fclose(f); free(draw_buf); return err; }

    /* Load all pixel data into the static RAM buffer then close the file
       immediately.  All animation frames draw from RAM — no SPIFFS access
       in the loop. */
    err = bmp_load_pixels(f, &bmp, s_pixel_buf, sizeof(s_pixel_buf));
    fclose(f);
    if (err != ESP_OK) { free(draw_buf); return err; }
    uint8_t *pixels = s_pixel_buf;

    /* Maximum scroll offsets relative to the window size */
    int max_ox = bmp.width  > win_w ? bmp.width  - win_w : 0;
    int max_oy = bmp.height > win_h ? bmp.height - win_h : 0;

    /* Starting source offset into the image */
    int ox = 0, oy = 0;
    switch (direction) {
        case PAN_LEFT_TO_RIGHT:  ox = 0;       break;
        case PAN_RIGHT_TO_LEFT:  ox = max_ox;  break;
        case PAN_TOP_TO_BOTTOM:  oy = 0;       break;
        case PAN_BOTTOM_TO_TOP:  oy = max_oy;  break;
    }

    /* Image top-left is placed at (win_x - ox, win_y - oy) so that image
       pixel (ox, oy) lands exactly at window corner (win_x, win_y). */
    bmp_draw_clipped(&bmp, pixels, win_x - ox, win_y - oy,
                     win_x, win_y, win_x + win_w, win_y + win_h, draw_buf);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));

        switch (direction) {
            case PAN_LEFT_TO_RIGHT: ox += step_px; if (ox > max_ox) ox = max_ox; break;
            case PAN_RIGHT_TO_LEFT: ox -= step_px; if (ox < 0)      ox = 0;      break;
            case PAN_TOP_TO_BOTTOM: oy += step_px; if (oy > max_oy) oy = max_oy; break;
            case PAN_BOTTOM_TO_TOP: oy -= step_px; if (oy < 0)      oy = 0;      break;
        }

        bmp_draw_clipped(&bmp, pixels, win_x - ox, win_y - oy,
                         win_x, win_y, win_x + win_w, win_y + win_h, draw_buf);

        if (direction == PAN_LEFT_TO_RIGHT && ox >= max_ox) break;
        if (direction == PAN_RIGHT_TO_LEFT && ox <= 0)      break;
        if (direction == PAN_TOP_TO_BOTTOM && oy >= max_oy) break;
        if (direction == PAN_BOTTOM_TO_TOP && oy <= 0)      break;
    }

    free(draw_buf);
    return ESP_OK;
}

// -------------------- Bitmap draw task --------------------
static void draw_bitmap_task(void *arg)
{
    (void)arg;
    while (1) {
        display_bitmap_pan("/spiffs/okinomi.bmp",   PAN_RIGHT_TO_LEFT, 0, 0, LCD_H_RES, LCD_V_RES, 1, 10);

        display_bitmap("/spiffs/pommes.bmp", 0, LCD_V_RES-53);
        display_bitmap_pan("/spiffs/pommesomel.bmp",  PAN_LEFT_TO_RIGHT, 0, 0, LCD_H_RES, LCD_V_RES-53, 1, 10);

        display_bitmap("/spiffs/sushiro.bmp", 0, 0);
        display_bitmap_pan("/spiffs/sushirosu.bmp", PAN_BOTTOM_TO_TOP, 0, 100, LCD_H_RES, LCD_V_RES-100, 1, 10);

        display_bitmap("/spiffs/zanmai.bmp", 0, LCD_V_RES - 112);
        display_bitmap_pan("/spiffs/zanmaisu.bmp",  PAN_LEFT_TO_RIGHT, 0, 0, LCD_H_RES, LCD_V_RES-112, 1, 10);

       

        

        
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

    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_NUM_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_NUM_BL, 1);
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
